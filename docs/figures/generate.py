#!/usr/bin/env python3
"""Render English explanatory figures and charts from the committed server data."""
from pathlib import Path
import json
import statistics as st
import xml.etree.ElementTree as ET
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch, Rectangle

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(__file__).resolve().parent
C = dict(ink='#19334b', muted='#566b80', line='#cbd5e1', blue='#2563eb',
         bluebg='#edf4ff', teal='#0f766e', tealbg='#e9f6f1', orange='#b96b12',
         orangebg='#fff5e5', gray='#f3f5f8', purple='#7554ba', white='#ffffff')
plt.rcParams.update({'font.family': 'sans-serif',
                     'font.sans-serif': ['DejaVu Sans', 'Arial', 'Liberation Sans', 'sans-serif'],
                     'font.size': 12,
                     'text.color': C['ink'], 'axes.labelcolor': C['muted'],
                     'xtick.color': C['muted'], 'ytick.color': C['muted'],
                     'svg.fonttype': 'none', 'svg.hashsalt': 'kvquery-readme-v1',
                     'savefig.facecolor': 'white'})
MANIFEST = []

def save(fig, name, title, desc, sources):
    fig.savefig(OUT / (name+'.svg'), metadata={'Date': None, 'Title': title,
                                             'Description': desc})
    fig.savefig(OUT / (name+'.png'), dpi=150, metadata={'Title': title})
    plt.close(fig)
    # Explicit accessible name/description, also readable in a standalone SVG.
    path = OUT / (name+'.svg')
    ET.register_namespace('', 'http://www.w3.org/2000/svg')
    ET.register_namespace('xlink', 'http://www.w3.org/1999/xlink')
    tree = ET.parse(path); svg = tree.getroot()
    svg.set('role', 'img'); svg.set('aria-labelledby', name+'-title '+name+'-desc')
    for tag, text, suffix in [('title', title, '-title'), ('desc', desc, '-desc')]:
        el = ET.Element('{http://www.w3.org/2000/svg}'+tag, {'id': name+suffix})
        el.text = text; svg.insert(0, el)
    tree.write(path, encoding='utf-8', xml_declaration=True)
    MANIFEST.append({'file': name+'.svg', 'title': title, 'description': desc,
                     'sources': sources})

def canvas(height, title, subtitle):
    fig = plt.figure(figsize=(12, height/100), facecolor='white')
    ax = fig.add_axes((0,0,1,1)); ax.set(xlim=(0,1200), ylim=(height,0)); ax.axis('off')
    txt(ax, 30, 33, title, size=19, weight='bold')
    txt(ax, 30, 64, subtitle, size=11, color='muted')
    return fig, ax

def txt(ax,x,y,s,size=12,color='ink',weight='normal',ha='left',va='center'):
    return ax.text(x,y,s,fontsize=size,color=C.get(color,color),weight=weight,
                   ha=ha,va=va,linespacing=1.45)

def box(ax,x,y,w,h,title,body='',color='blue',face=None):
    ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle='round,pad=0,rounding_size=10',
                 edgecolor=C[color],facecolor=C[face or color+'bg'],linewidth=1.2))
    txt(ax,x+16,y+27,title,13,color,'bold')
    if body: txt(ax,x+16,y+52,body,11,'ink',va='top')

def arrow(ax,start,end,color='muted',rad=0,style='-'):
    ax.add_patch(FancyArrowPatch(start,end,arrowstyle='-|>',mutation_scale=14,
                   linewidth=1.4,color=C[color],connectionstyle=f'arc3,rad={rad}',linestyle=style))

# 1. Component flow: a concrete batch, including a miss and a duplicate.
fig,ax=canvas(490,'Build a snapshot. Look up a batch. Keep the original order.',
              'Opaque byte values, immutable storage, and per-call execution state')
box(ax,30,100,250,90,'Input records','7 → "hello"    0 → empty',color='teal')
box(ax,365,100,360,90,'Snapshot::build / load','Pack values, place nodes, resolve collisions',color='teal')
arrow(ax,(280,145),(365,145),'teal')
box(ax,825,100,340,90,'Snapshot storage','16-byte nodes + external value region',color='teal')
arrow(ax,(725,145),(825,145),'teal')
box(ax,30,280,250,110,'Query keys','[7, 8, 0, 7]\nOne input position per result')
box(ax,365,270,360,130,'Per-call executor','Scalar / Prefetch / AMAC / SIMD\nState and output slots belong to this call')
box(ax,825,270,340,130,'Ordered value views','["hello", missing, empty, "hello"]\nBatchResult retains the snapshot')
arrow(ax,(280,335),(365,335),'blue');arrow(ax,(725,335),(825,335),'blue')
ax.plot([995,995,650],[190,225,225],color=C['teal'],linewidth=1.4)
arrow(ax,(650,225),(650,270),'teal')
txt(ax,800,212,'read shared data',10,'teal',ha='center')
txt(ax,30,449,'The same snapshot serves many calls; each call preserves duplicates and distinguishes empty values from misses.',11,'muted')
save(fig,'overview','Batch KV lookup: input, storage and ordered results',
     'Build input records into immutable nodes and external values. Each calling thread runs its own executor. A batch [7,8,0,7] returns hello, missing, empty and hello, retaining its snapshot.',
     ['include/kvquery/kvquery.hpp','src/kvquery.cpp'])

# 2. Source-aligned construction example: B is moved from slot 3 to empty slot 1.
fig,ax=canvas(535,'Move a lodger, repair its chain, then claim the home bucket.',
              'Illustrative keys: A and B hash to bucket 2; the incoming C hashes to bucket 3')
for p,(heading,content,link) in enumerate([
    ('1  C needs bucket 3',{1:'empty',2:'A  ·  home 2',3:'B  ·  home 2',4:'empty'},(2,3,'+1')),
    ('2  Move B; repair A → B',{1:'B  ·  home 2',2:'A  ·  home 2',3:'empty',4:'empty'},(2,1,'−1')),
    ('3  Place C at its home',{1:'B  ·  home 2',2:'A  ·  home 2',3:'C  ·  home 3',4:'empty'},(2,1,'−1'))]):
    x=30+p*400
    txt(ax,x,118,heading,13,weight='bold')
    for k in range(1,5):
        y=156+(k-1)*65; label=content[k]
        fill='gray' if label=='empty' else ('orangebg' if label.startswith('C') else 'bluebg')
        edge='line' if label=='empty' else ('orange' if label.startswith('C') else 'blue')
        ax.add_patch(FancyBboxPatch((x+38,y),232,48,boxstyle='round,pad=0,rounding_size=6',
                     facecolor=C[fill],edgecolor=C[edge],linewidth=1.1))
        txt(ax,x+17,y+24,str(k),11,'muted',ha='center')
        txt(ax,x+54,y+24,label,12)
    start,end,label=link
    sy=180+(start-1)*65;ey=180+(end-1)*65
    arrow(ax,(x+275,sy),(x+275,ey),'blue',rad=.65 if ey>sy else -.65)
    txt(ax,x+326,(sy+ey)/2,label,11,'blue',ha='center')
    txt(ax,x,450,['B occupies another key’s home.','B’s payload moves with the node.','Queries follow A → B or find C directly.'][p],10,'muted')
txt(ax,30,503,'All relocation happens before publication. The read path only compares keys and follows stored relative links.',11,'muted')
save(fig,'collision-relocation','Construction-time collision relocation',
     'Before insertion, A in bucket 2 links +1 to B in bucket 3. To insert C whose home is bucket 3, move B to empty bucket 1, change A link to minus 1, then place C at bucket 3. Inline payload moves with the node.',
     ['include/kvquery/detail/neighbor_index.hpp:NeighborIndex::Insert_'])

# 3. Two representations, with a common logical six-byte value for the control.
fig,ax=canvas(530,'Keep a small value in the node you already found.',
              'Both representations use a 16-byte node. Field widths below are schematic, not to scale.')
for y,inline in [(145,False),(345,True)]:
    txt(ax,30,y-34,'Inlining enabled: 0–6 bytes' if inline else 'External-only control',14,weight='bold')
    cols=([('Key','64 bits',290,'blue'),('Chain','10 bits',130,'blue'),('Tag','3 bits',100,'teal'),
           ('Length','3 bits',100,'teal'),('Payload','48 bits',280,'teal')] if inline else
          [('Key','64 bits',290,'blue'),('Chain','10 bits',130,'blue'),('Length','18 bits',190,'blue'),
           ('Offset','36 bits',290,'orange')])
    x=30
    for title,bits,width,color in cols:
        ax.add_patch(Rectangle((x,y),width,76,facecolor=C[color+'bg'],edgecolor=C[color],linewidth=1.2))
        txt(ax,x+width/2,y+25,title,13,color,'bold',ha='center')
        txt(ax,x+width/2,y+53,bits,11,color,ha='center');x+=width
    if inline:
        txt(ax,45,y+103,'Key = 101',11);txt(ax,464,y+103,'111',11,'teal')
        txt(ax,565,y+103,'6',11,'teal');txt(ax,767,y+103,'"hello!"',11,'teal')
        txt(ax,965,y+29,'View points',11,'teal');txt(ax,965,y+53,'into this node.',11,'teal')
    else:
        box(ax,1015,y-5,155,90,'Value bytes','"hello!"',color='orange')
        arrow(ax,(935,y+38),(1010,y+38),'orange')
        txt(ax,974,y+104,'base + 128',10,'orange',ha='center')
        txt(ax,45,y+103,'Key = 101',11);txt(ax,500,y+103,'6 bytes',11)
        txt(ax,762,y+103,'Offset = 128',11,'orange')
txt(ax,30,499,'Full keys and relative links stay intact. Larger values continue using external storage; file records are layout-independent.',11,'muted')
save(fig,'value-layout','External values versus six-byte inline values',
     'An external node stores a 64-bit key, 10-bit link, 18-bit length encoding and 36-bit offset. An inline node stores the same key and link with a 3-bit tag, 3-bit length and 48-bit payload. Both are 16 bytes. A hello-exclamation value can live in either layout.',
     ['include/kvquery/detail/neighbor_index.hpp:FlatNode','src/kvquery.cpp:Snapshot::Impl::view'])

# 4. Snapshot/result ownership, using the actual shared-owner mechanism.
fig,ax=canvas(410,'Retained results keep their own snapshot alive.',
              'The application chooses the handle for new calls; each BatchResult retains its source snapshot')
box(ax,30,155,255,110,'Retained BatchResult','Views of old bytes',color='blue')
box(ax,355,150,230,120,'Old Snapshot','Stable nodes and values\nOwned by old results',color='blue')
box(ax,640,150,230,120,'New Snapshot','Built or loaded separately\nUsed by new requests',color='teal')
box(ax,930,155,240,110,'New BatchResult','Views of new bytes',color='teal')
arrow(ax,(285,210),(355,210),'blue');arrow(ax,(930,210),(870,210),'teal')
txt(ax,320,138,'owns',11,'blue',ha='center');txt(ax,900,138,'owns',11,'teal',ha='center')
txt(ax,30,321,'Old storage is released after its final snapshot/result owner is gone.',11,'muted')
txt(ax,30,358,'Borrowed string_views require a surviving result or snapshot owner. No per-key copy is needed for a returned view.',11,'muted')
save(fig,'snapshot-ownership','Snapshot ownership across an application-managed refresh',
     'Retained old results own the old snapshot while new results own a separately prepared new snapshot. Each group reads stable bytes. Storage is freed only when its final snapshot or result owner is gone.',
     ['include/kvquery/kvquery.hpp:BatchResult','src/kvquery.cpp:Snapshot::query'])

# 5. Residual state: the two comparisons happen sequentially on one calling thread.
fig,ax=canvas(565,'Finish a probe, refill its slot; keep unfinished probes in place.',
              'SIMD example with window = 4. Two candidate-key pairs are processed on one calling thread.')
for p in range(2):
    x=30+p*600
    ax.add_patch(FancyBboxPatch((x,100),540,40,boxstyle='round,pad=0,rounding_size=8',
                 facecolor=C['gray'],edgecolor=C['line']))
    txt(ax,x+270,120,'One 128-bit comparison: two full 64-bit keys',11,'muted',ha='center')
first=[('A  ·  input 0','Match → write result[0]'),('B  ·  input 1','Follow collision link'),
       ('C  ·  input 2','Miss → write result[2]'),('D  ·  input 3','Follow collision link')]
second=[('E  ·  input 4','New key; prefetch home'),('B  ·  input 1','Keep key; prefetch next'),
        ('F  ·  input 5','New key; prefetch home'),('D  ·  input 3','Keep key; prefetch next')]
for j,((head,body),(head2,body2)) in enumerate(zip(first,second)):
    x=30+j*300
    box(ax,x,160,240,90,head,body,color='blue')
    color='teal' if j%2==0 else 'blue'
    arrow(ax,(x+120,250),(x+120,320),color)
    txt(ax,x+135,285,'refill' if j%2==0 else 'retain',11,color)
    box(ax,x,325,240,90,head2,body2,color=color)
box(ax,30,454,1140,79,'Result slots stay in input order',
    '[ value(A), pending(B), missing(C), pending(D), pending(E), pending(F) ]',color='teal')
save(fig,'simd-window','SIMD residual states and slot refill',
     'In a width-four window, pair A/B and pair C/D are compared sequentially on one thread. A matches and C misses, freeing slots for E and F. B and D retain their collision positions. Each completed result is written to its original input position.',
     ['src/kvquery.cpp:Snapshot::query SIMD branch','include/kvquery/detail/simd_probe.hpp'])

# Committed data drives all quantitative charts.
def load(name):return json.loads((ROOT/'bench/results'/name).read_text())
def cases(data,name):return [c['report'] for c in data['cases'] if c['name']==name]
def rates(data,name,strategy,window,layout=None):
    return [next(v['keys_per_second']/1e6 for v in c['results'] if v['strategy']==strategy
            and v['window']==window and (layout is None or v['layout']==layout)) for c in cases(data,name)]
def ratio(data,name,strategy='simd',window=32):
    return st.median(i/e for i,e in zip(rates(data,name,strategy,window,'inline'),rates(data,name,strategy,window,'external')))
STD='duke-x86-inline-2026-10-24.json'; PAIR='duke-inline-2026-10-24.json'; SCALE='duke-inline-scale-2026-10-24.json'
std,paired,scale=load(STD),load(PAIR),load(SCALE)

def clean(ax):
    ax.spines[['top','right','left']].set_visible(False)
    ax.spines['bottom'].set_color(C['line']);ax.tick_params(axis='both',length=0)
    ax.set_axisbelow(True);ax.grid(axis='x',color='#e5eaf0',linewidth=.8)

# 6. Standard executors; same fixed window and full dataset rows as README.
fig,ax=plt.subplots(figsize=(12,8.3));fig.subplots_adjust(left=.20,right=.94,top=.84,bottom=.14)
names=list(dict.fromkeys(c['name'] for c in std['cases']))
methods=[('unordered_map',1,'Map','muted'),('scalar',1,'Scalar','blue'),('amac',32,'AMAC/32','teal'),('simd',32,'SIMD/32','purple')]
for j,(strategy,window,label,color) in enumerate(methods):
    ys=[k*1.35+j*.23 for k in range(len(names))]
    vals=[rates(std,n,strategy,window) for n in names];med=[st.median(v) for v in vals]
    ax.barh(ys,med,height=.19,color=C[color],label=label,
            xerr=([v-min(a) for v,a in zip(med,vals)],[max(a)-v for v,a in zip(med,vals)]),
            error_kw={'elinewidth':.7,'ecolor':C['ink'],'capsize':1.4})
    for y,value,allv in zip(ys,med,vals):ax.text(max(allv)+.35,y,f'{value:.2f}',va='center',fontsize=9)
ax.set_yticks([k*1.35+.345 for k in range(len(names))],names,fontsize=11);ax.invert_yaxis()
ax.set_xlim(0,45);ax.set_xlabel('Million keys per second (higher is better)',labelpad=13)
clean(ax);ax.legend(loc='lower left',bbox_to_anchor=(0,1.03),ncol=4,frameon=False,fontsize=11)
fig.text(.04,.955,'Executor throughput on the Duke server',fontsize=20,weight='bold')
fig.text(.04,.915,'One thread • CPU 0 • process medians; whiskers show process min/max',fontsize=11,color=C['muted'])
fig.text(.04,.025,'GCC 11.4 / -O3  |  Xeon E5-2440 v2  |  3 processes × 3 rounds  |  Prefetch and all windows remain in the full table.',fontsize=10,color=C['muted'])
save(fig,'executor-throughput','Executor throughput across eight server workloads',
     'Grouped bars show map, Scalar, AMAC window 32 and SIMD window 32 for all eight standard workloads. Throughput medians and process min/max are computed directly from the committed Duke server JSON; all axes start at zero.',
     ['bench/results/'+STD])

# 7. Show both the gain region and the measured mixed-value regression.
fig,ax=plt.subplots(figsize=(12,5.4));fig.subplots_adjust(left=.23,right=.91,top=.78,bottom=.17)
items=[('value-1','1 byte'),('value-2','2 bytes'),('value-4','4 bytes'),('value-6','6 bytes'),
       ('value-7','7 bytes (external)'),('value-32','32 bytes (external)'),('mixed-6-32','50% 6 B + 50% 32 B')]
for y,(name,label) in enumerate(items):
    rr=[i/e for i,e in zip(rates(paired,name,'simd',32,'inline'),rates(paired,name,'simd',32,'external'))]
    value=st.median(rr);color='teal' if value>=1 else 'orange'
    ax.barh(y,value-1,left=1,height=.56,color=C[color],alpha=.88)
    ax.errorbar(value,y,xerr=[[value-min(rr)],[max(rr)-value]],fmt='o',markersize=4,
                color=C['ink'],elinewidth=1,capsize=3)
    ax.text(max(1.01,max(rr)+.016),y,f'{(value-1)*100:+.1f}%',va='center',fontsize=12,weight='bold',color=C[color])
ax.axvline(1,color=C['muted'],linewidth=1,linestyle='--');ax.set_xlim(.91,1.43)
ax.set_yticks(range(len(items)),[label for _,label in items],fontsize=12);ax.invert_yaxis()
ax.set_xlabel('Inlining enabled / external-only throughput',labelpad=12);clean(ax)
fig.text(.04,.94,'Where inlining helps — and where it costs',fontsize=20,weight='bold')
fig.text(.04,.875,'SIMD/32 • 1 million records • batch 256 • 90% target hits • paired process ratios',fontsize=11,color=C['muted'])
fig.text(.04,.04,'Points: paired medians. Whiskers: process min/max. Values above 6 bytes remain external in both layouts.',fontsize=10,color=C['muted'])
save(fig,'inline-tradeoffs','Inlining gains and the mixed-value tradeoff',
     'Paired SIMD ratios increase with inline payload length: six-byte values improve 31.0%. Seven and 32-byte controls remain external. An even mix of six and 32-byte values regresses 4.0%. Whiskers show paired process ranges.',
     ['bench/results/'+PAIR])

# 8. Scaling throughput and logical storage from the same three recorded cases.
fig,(a,b)=plt.subplots(1,2,figsize=(12,5.8));fig.subplots_adjust(left=.08,right=.96,top=.76,bottom=.21,wspace=.34)
counts=[1,4,8];x=list(range(3))
for layout,label,color in [('external','External-only','muted'),('inline','Inlining enabled','blue')]:
    allv=[rates(scale,f'scale-{n}m-six','simd',32,layout) for n in counts];med=[st.median(v) for v in allv]
    a.errorbar(counts,med,yerr=([v-min(vs) for v,vs in zip(med,allv)],
                               [max(vs)-v for v,vs in zip(med,allv)]),
               color=C[color],marker='o',linewidth=2.2,capsize=4,label=label)
    for n,v in zip(counts,med):a.annotate(f'{v:.2f}',(n,v),xytext=(0,10 if layout=='inline' else -19),
                              textcoords='offset points',ha='center',fontsize=10,color=C[color])
a.set(xlim=(.4,8.6),ylim=(0,40),xticks=counts,xlabel='Million records',ylabel='Million keys per second')
a.set_title('SIMD/32 throughput',loc='left',fontsize=14,weight='bold',pad=15)
a.legend(loc='lower left',frameon=False,fontsize=10)
for i,layout,label,color in [(0,'external','External-only','muted'),(1,'inline','Inlining enabled','blue')]:
    values=[]
    for n in counts:
        c=cases(scale,f'scale-{n}m-six')[0]
        values.append((c['index_bytes']+c['external_control_value_bytes' if layout=='external' else 'value_storage_bytes'])/1e6)
    positions=[p+(-.19 if i==0 else .19) for p in x]
    b.bar(positions,values,width=.34,color=C[color],label=label)
    for p,v in zip(positions,values):b.text(p,v+5,f'{v:.0f}',ha='center',fontsize=11,color=C[color])
b.set(xticks=x,xticklabels=['1 M','4 M','8 M'],ylim=(0,245),xlabel='Records',ylabel='Logical storage (MB)')
b.set_title('Index + external value bytes',loc='left',fontsize=14,weight='bold',pad=15)
for ax in [a,b]:
    ax.spines[['top','right']].set_visible(False)
    for spine in ['left','bottom']:ax.spines[spine].set_color(C['line'])
    ax.grid(axis='y',color='#e5eaf0');ax.set_axisbelow(True)
fig.text(.04,.95,'Larger working sets make inlining more valuable in this run',fontsize=18,weight='bold')
gain_text=' → '.join(f'+{100*(ratio(scale,f"scale-{n}m-six")-1):.1f}%' for n in counts)
last=cases(scale,'scale-8m-six')[0]
before=(last['index_bytes']+last['external_control_value_bytes'])/1e6
after=(last['index_bytes']+last['value_storage_bytes'])/1e6
fig.text(.04,.885,f'SIMD paired gain: {gain_text}   |   8 M records: {before:.0f} → {after:.0f} MB',fontsize=12,color=C['blue'])
fig.text(.04,.075,'6-byte values • batch 256 • 90% target hits • 1,048,576 queries/round • 3 processes × 3 rounds • CPU 0',fontsize=10,color=C['muted'])
fig.text(.04,.025,'Storage is logical array size, excluding allocator overhead and other process memory. Throughput axis starts at zero.',fontsize=9,color=C['muted'])
save(fig,'inline-scaling','Inlining throughput and storage at one, four and eight million records',
     'At one, four and eight million six-byte records, SIMD inline throughput medians are 31.94, 28.78 and 26.57 million keys per second. Paired gains are 27.4, 41.8 and 46.0 percent. Logical storage drops from 26 to 20, 104 to 80 and 208 to 160 decimal megabytes.',
     ['bench/results/'+SCALE])
(OUT/'manifest.json').write_text(json.dumps(MANIFEST,indent=2)+'\n')
print(f'Generated {len(MANIFEST)} SVG figures with PNG previews in {OUT.relative_to(ROOT)}')

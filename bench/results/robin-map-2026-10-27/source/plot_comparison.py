import sys,pathlib,csv,json
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
import argparse
parser=argparse.ArgumentParser()
parser.add_argument('--input',required=True)
parser.add_argument('--output',required=True)
args=parser.parse_args()
out=pathlib.Path(args.output);out.mkdir(parents=True,exist_ok=True)
rows=list(csv.DictReader((pathlib.Path(args.input)/'summary.csv').open()))
methods=['std_unordered_map','tsl_robin_map','kvquery_scalar','kvquery_amac32','kvquery_simd32']
labels=['std::unordered_map','tsl::robin_map','KVQuery Scalar','KVQuery AMAC/32','KVQuery SIMD/32']
colors=['#a1aab6','#e5a12a','#789cc6','#3276ba','#174a7e']
cases=[('cache-resident','Cache-resident batches','10K records Â· 32-byte values Â· batch 256'),('single-key','Single-key requests','1M records Â· 32-byte values Â· batch 1'),('large-batch','Large batches','1M records Â· 32-byte values Â· batch 1,024'),('large-small-values','Large small-value dataset','8M records Â· 6-byte values Â· batch 256')]
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':12,'svg.fonttype':'none','axes.spines.top':False,'axes.spines.right':False,'axes.spines.left':False,'axes.edgecolor':'#d7dee7','text.color':'#16283b','axes.labelcolor':'#45576b','xtick.color':'#5e7081','ytick.color':'#263b50'})
fig,axes=plt.subplots(2,2,figsize=(15.5,10.5));fig.patch.set_facecolor('#f5f8fc')
for ax,(case,title,sub) in zip(axes.flat,cases):
 group=[next(r for r in rows if r['case']==case and r['method']==m) for m in methods]
 med=np.array([float(r['median_mkeys_s']) for r in group]);lo=np.array([float(r['min_mkeys_s']) for r in group]);hi=np.array([float(r['max_mkeys_s']) for r in group]);y=np.arange(5)
 ax.set_facecolor('#f5f8fc');ax.barh(y,med,color=colors,height=.64,zorder=3)
 ax.errorbar(med,y,xerr=np.vstack([med-lo,hi-med]),fmt='none',ecolor='#24384b',elinewidth=1.3,capsize=3,zorder=4)
 for i,value in enumerate(med):ax.text(max(value,hi[i])+.025*hi.max(),i,f'{value:.2f}',va='center',fontsize=12,fontweight='bold')
 ax.set_yticks(y,labels=labels,fontsize=11);ax.tick_params(axis='y',length=0,pad=8);ax.invert_yaxis();ax.set_xlim(0,hi.max()*1.25);ax.set_xlabel('Million keys per second',fontsize=11);ax.xaxis.grid(True,color='#dfe6ef',zorder=0);ax.set_axisbelow(True)
 ax.set_title(title+'\n'+sub,loc='left',fontsize=14,fontweight='bold',pad=13)
 winner='robin-map leads on single-key calls' if case=='single-key' else 'KVQuery leads on these batched reads'
 ax.text(0,-.27,winner,transform=ax.transAxes,fontsize=10.5,color='#8c5b00' if case=='single-key' else '#174a7e',fontweight='bold')
fig.suptitle('KVQuery vs robin-map: where each approach wins',x=.04,y=.98,ha='left',fontsize=23,fontweight='bold')
fig.text(.04,.925,'Read-only lookup Â· one pinned performance core Â· identical inputs and ordered value views',fontsize=12.5,color='#51657a')
fig.subplots_adjust(left=.16,right=.97,top=.84,bottom=.17,hspace=.73,wspace=.72)
fig.text(.04,.067,'Bars: medians of 3 independent processes. Whiskers: minâ€“max, including all slow rounds. Panel scales differ.',fontsize=11,color='#45576b')
fig.text(.04,.04,'30 s/method for large workloads; 10 s diagnostics. Core Ultra 9 285K Â· Windows/Cygwin Â· GCC 14.4.0 Â· 2026-10-27',fontsize=10.5,color='#64758a')
fig.savefig(out/'comparison.png',dpi=175,facecolor=fig.get_facecolor());fig.savefig(out/'comparison.svg',facecolor=fig.get_facecolor());plt.close(fig)
print(out)

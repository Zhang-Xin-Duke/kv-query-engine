# 服务器评测与基准方法

[项目首页](../README.md) · [设计与实现](ARCHITECTURE.md)

## 1. 设计文档中的服务器实验

参考实现的评测来自 [CIKM 2024 论文](https://arxiv.org/pdf/2409.00400)及其[公开复现仓库](https://github.com/slow-steppers/NeighborHash)。论文 Table 3 对应 16 GB 数据集、90% 命中率和 0.8 负载因子；公开工程记录的 CPU 为 Intel Xeon Gold 5218，运行于 Linux，配置 2 MiB Hugepage。

| Table 3 中的实现 | MOPS | APCL |
|---|---:|---:|
| CoalescedHashing | 19 | 1.72 |
| PerfectCellarHash | 23 | 1.48 |
| NeighborProbing | 30 | 1.34 |
| NeighborHash | 39 | 1.14 |

MOPS 表示每秒百万次查找，APCL 表示平均探测缓存行数。实验逐步引入节点迁移、邻近探测和内联链编码，展示了索引布局与读取效率之间的关系。本文将其作为设计依据；本节表格的测试对象是论文参考实现。

## 2. 当前引擎的比较方式

`kvquery_bench` 使用相同的 Key 批次比较十一个配置：

- `std::unordered_map`，使用与索引初始构建相同的 64 位混合函数，目标最大负载因子 0.8。
- 紧凑索引 Scalar。
- Prefetch，窗口 8、32、64。
- AMAC，窗口 8、32、64。
- SIMD，窗口 8、32、64；JSON 的 `simd_backend` 记录编译后端。

各路径都分配按输入排序的可选 Value 视图。基准在计时前比较完整 Value，在计时中消费长度、首字节和尾字节，并核对各策略校验和。计时包含结果分配、查找、视图返回、生命周期处理和结果销毁；衡量的是内存查询与视图访问路径。

每批调用使用 `steady_clock` 记录墙钟时间。吞吐按查询 Key 总数除以累计调用时间计算，P50/P95/P99 是整批调用延迟。查询准备、完整字节复制、文件加载和网络传输在这项测量范围之外。

## 3. 工作负载

| 名称 | 记录数 | 每批 Key 数 | Value 字节 | 目标命中率 | 分布 |
|---|---:|---:|---:|---:|---|
| small-uniform | 10,000 | 256 | 32 | 90% | 均匀 |
| large-single | 1,000,000 | 1 | 32 | 90% | 均匀 |
| large-batch32 | 1,000,000 | 32 | 32 | 90% | 均匀 |
| large-batch256 | 1,000,000 | 256 | 32 | 90% | 均匀 |
| large-batch1024 | 1,000,000 | 1024 | 32 | 90% | 均匀 |
| large-low-hit | 1,000,000 | 256 | 32 | 30% | 均匀 |
| large-hot | 1,000,000 | 256 | 32 | 90% | 热点 |
| wide-values | 250,000 | 256 | 256 | 90% | 均匀 |

数据 Key 为偶数，命中请求从已有记录中选择，缺失请求使用奇数，因此能控制缺失语义。热点分布中，90% 的 Key 选择来自前 1% 的记录。报告保存实际命中率。

标准套件每项负载启动三个独立进程，每个进程执行三轮。进程之间使用相邻固定种子；一轮内所有策略处理相同输入，策略顺序逐轮轮换。完整校验和预热在计时前完成。

## 4. 服务器复现

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure

# 单次实验：结果写入 JSON。
mkdir -p bench/results
build/release/kvquery_bench \
  --records 1000000 --batch 256 --queries 262144 \
  --value-bytes 32 --hit-rate 0.9 --distribution uniform \
  --seed 20261021 --rounds 3 > bench/results/server-single.json

# 完整实验与结果表。
python3 bench/run_suite.py --output bench/results/server-suite.json
python3 bench/summarize.py \
  --input bench/results/server-suite.json \
  --output bench/results/server-report.md
```

服务器评测应固定 CPU 集合与 NUMA 内存节点，并记录 CPU 型号、内存、内核、编译器、编译参数及进程绑定策略。若使用 `taskset` 或 `numactl`，在实际分配的 CPU/节点上运行上述命令。Hugepage、硬件预取设置与并发负载也应随报告记录，便于解释不同实验的差异。

`run_suite.py` 保存命令、种子、硬件与编译器信息、每进程各策略结果，以及源码和二进制 SHA256。源码或二进制在实验期间变化会导致记录校验失败。

论文参考工程使用独立的 Bazel/Google Benchmark 复现流程。当前引擎的 Value 字节与有序视图接口采用本节协议，跨实现比较时应对齐这些测量边界。

## 5. 结果阅读

`bench/summarize.py` 按每个负载的进程中位数汇总 Mkeys/s，固定展示 Scalar、Prefetch/32、AMAC/32 和 SIMD/32，其他窗口保留在 JSON 中。报告同时列出进程间吞吐范围与批次 P99 范围。

比较结果时，先看批大小和工作集，再看命中率与热点分布。紧凑布局改变索引占用与访问位置，预取和 AMAC 改变访问顺序，Value 大小则影响实际数据访问。基准让这些因素可以在一致的返回语义下分别观察。

## 6. 数据记录

第一节引用服务器参考实验。SIMD 新增后的原始结果与分析见 [SIMD 实测](SIMD_EVALUATION.md)。运行当前引擎的服务器套件后，原始 JSON 与汇总表保存在 `bench/results/`，使用结果中的环境和源码哈希标识具体实验。

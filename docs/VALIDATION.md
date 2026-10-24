# 测试与验证

## 自动化检查

Linux CI 使用四组配置运行完整 CTest：GCC Release、Clang Debug、Clang ASan/UBSan、强制可移植 SIMD 的 GCC Release。每组包含十一个测试组，当前工作流见 [.github/workflows/ci.yml](../.github/workflows/ci.yml)。

| 测试组 | 覆盖内容 |
|---|---|
| edges | Key 为 0、最大整数、二进制/空/大 Value、无效配置、缺失和重复查询 |
| collisions | 确定性同桶冲突、寄居节点迁移、极小表和链偏移边界 |
| randomized | 随机二进制数据、三种负载因子、四种执行器和多窗口差分 |
| concurrent | 共享快照并发读取、结果持有期与最终资源释放 |
| persistence | 截断、错误头部、校验和、长度、预算、重复记录与竞争保存 |
| retry | 短偏移邻域耗尽、受预算约束的扩容换种子重建 |
| format | 独立小端字节样本与保存结果的精确比对 |
| simd | 两个 32 位半字的完整 Key 比较、碰撞未命中、奇数批次、补位与排空、视图持有期 |
| inline | 0–6 字节二进制值、链修改与搬移、混合布局、长度边界、跨布局文件加载、视图复制/移动和预算 |
| cli_roundtrip | 构建、检查、四种查询方式、数字边界和错误输入 |
| benchmark_smoke | JSON 输出、结果校验和、样本数量和百分位顺序 |

测试使用显式检查，Release 构建同样执行断言逻辑。随机差分以独立映射为参考，确定性样本确保冲突和迁移分支被触发。

## 运行

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
ctest --test-dir build/debug --output-on-failure

cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DKVQUERY_SANITIZERS=ON
cmake --build build/asan -j
ctest --test-dir build/asan --output-on-failure

cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DKVQUERY_TSAN=ON
cmake --build build/tsan -j
ctest --test-dir build/tsan -R 'concurrent|persistence' --output-on-failure
```

ASan/UBSan 和 TSan 使用独立构建目录。CLI 集成测试需要 Python 3。格式、生命周期和查询结果属于正确性检查；吞吐和延迟由[服务器基准](BENCHMARKS.md)单独测量。

## SIMD backend validation

Run the same eleven CTest groups in a separate forced-fallback build:

```sh
cmake -S . -B build/portable -DCMAKE_BUILD_TYPE=Release -DKVQUERY_FORCE_PORTABLE_SIMD=ON
cmake --build build/portable -j
ctest --test-dir build/portable --output-on-failure
```

The `simd` test checks the reported backend against the compile target, lane masks,
false matches with equal low/high 32-bit halves, long collision chains, missing and
duplicate keys, odd windows, short batches and retained results. Random differential,
file-roundtrip and concurrent-reader tests include the SIMD executor. Linux CI adds
an explicit portable Release build alongside GCC Release, Clang Debug and ASan/UBSan.

# picoflate 测试验证与性能基准报告

> 编译标准：`-std=c++26 -O3`（无缝兼容 `-std=c++23`）

---

## 1. 测试环境与方法论

- **测试平台**：Apple M4（`arm64-apple-darwin25.4.0`，Apple Clang 21.0.0，`QOS_CLASS_USER_INTERACTIVE` 绑定性能核）及 AWS `c7i`（Intel Xeon 4th Gen Sapphire Rapids `x86-64-v3` 基线对照）。
- **基准协议**：
  - 每次运行先热身 `20,000` 次，再连续测量 `7` 轮 × `100,000` 次解压（合计 `700,000` 次测量调用），记录中位数、最小值、均值与吞吐率（原始逐轮数据见 [`rounds.csv`](rounds.csv)，汇总表见 [`summary.csv`](summary.csv)）。
  - 输入指针与长度经 `volatile` 修饰，输出首字节与返回值通过内联汇编内存屏障锚定，防止跨迭代常量折叠或死代码消除。
- **对比对象**：
  1. `zlib`（系统 `inflateReset2(-15)` + `inflate(Z_FINISH)`，零堆分配复用 `z_stream`）
  2. `picoflate_v0`（初始基线版：16-bit 双层表 + `lens[320]` 两趟扫描 + 标量解码循环）
  3. `libdeflate v1.22`（官方 `v1.22` Release 源码 `-O2 -DNDEBUG` 编译复用 `libdeflate_decompressor`）
  4. `picoflate`（当前版本：`include/picoflate/picoflate.h`）

---

## 2. 实测性能数据

### 2.1 实测汇总（`100,000 iters × 7 rounds`）

| 用例 ID | 样本特征 | 压缩 → 解压大小 | 实现 | Median (ns) | Min (ns) | Mean (ns) | 吞吐 (MiB/s) | 相对 `v0` | 相对 `libdeflate v1.22` |
| :--- | :--- | :---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| `ws_json_v1_dyn` | WebSocket JSON 增量帧 `v1` (`btype=2`) | `207 B -> 563 B` | `zlib` | 1216.73 | 1201.08 | 1226.98 | 441.3 | +9.7% | -21.0% |
| `ws_json_v1_dyn` | WebSocket JSON 增量帧 `v1` (`btype=2`) | `207 B -> 563 B` | `picoflate_v0` | 1109.55 | 1087.18 | 1112.90 | 483.9 | baseline | -28.0% |
| `ws_json_v1_dyn` | WebSocket JSON 增量帧 `v1` (`btype=2`) | `207 B -> 563 B` | `libdeflate v1.22` | 1540.30 | 1534.15 | 1568.33 | 348.6 | +38.8% | baseline |
| **`ws_json_v1_dyn`** | **WebSocket JSON 增量帧 `v1` (`btype=2`)** | **`207 B -> 563 B`** | **`picoflate`** | **651.99** | **643.44** | **655.17** | **823.5** | **-41.2% (1.70×)** | **-57.7% (2.36×)** |
| `ws_json_v2_dyn` | WebSocket JSON 增量帧 `v2` (`btype=2`) | `148 B -> 323 B` | `zlib` | 1063.87 | 1045.36 | 1071.38 | 289.5 | +23.8% | -24.0% |
| `ws_json_v2_dyn` | WebSocket JSON 增量帧 `v2` (`btype=2`) | `148 B -> 323 B` | `picoflate_v0` | 859.46 | 856.97 | 865.40 | 358.4 | baseline | -38.6% |
| `ws_json_v2_dyn` | WebSocket JSON 增量帧 `v2` (`btype=2`) | `148 B -> 323 B` | `libdeflate v1.22` | 1399.56 | 1396.06 | 1407.57 | 220.1 | +62.8% | baseline |
| **`ws_json_v2_dyn`** | **WebSocket JSON 增量帧 `v2` (`btype=2`)** | **`148 B -> 323 B`** | **`picoflate`** | **509.26** | **501.88** | **508.74** | **604.9** | **-40.7% (1.69×)** | **-63.6% (2.75×)** |
| `json_batch40_dyn` | 40 项结构化 JSON 批次 (`btype=2`) | `316 B -> 1345 B` | `zlib` | 1474.48 | 1463.23 | 1475.57 | 869.9 | -8.0% | -18.6% |
| `json_batch40_dyn` | 40 项结构化 JSON 批次 (`btype=2`) | `316 B -> 1345 B` | `picoflate_v0` | 1602.32 | 1591.24 | 1608.24 | 800.5 | baseline | -11.6% |
| `json_batch40_dyn` | 40 项结构化 JSON 批次 (`btype=2`) | `316 B -> 1345 B` | `libdeflate v1.22` | 1812.29 | 1805.34 | 1816.88 | 707.8 | +13.1% | baseline |
| **`json_batch40_dyn`** | **40 项结构化 JSON 批次 (`btype=2`)** | **`316 B -> 1345 B`** | **`picoflate`** | **901.52** | **894.00** | **901.86** | **1422.8** | **-43.7% (1.78×)** | **-50.3% (2.01×)** |
| `ws_json_v1_fixed` | WebSocket 帧 `v1` 纯解码循环 (`btype=1`) | `251 B -> 563 B` | `zlib` | 457.95 | 452.09 | 457.26 | 1172.4 | -23.7% | +16.3% |
| `ws_json_v1_fixed` | WebSocket 帧 `v1` 纯解码循环 (`btype=1`) | `251 B -> 563 B` | `picoflate_v0` | 600.24 | 597.51 | 605.55 | 894.5 | baseline | +52.4% |
| `ws_json_v1_fixed` | WebSocket 帧 `v1` 纯解码循环 (`btype=1`) | `251 B -> 563 B` | `libdeflate v1.22` | 393.90 | 391.56 | 397.51 | 1363.1 | -34.4% | baseline |
| **`ws_json_v1_fixed`** | **WebSocket 帧 `v1` 纯解码循环 (`btype=1`)** | **`251 B -> 563 B`** | **`picoflate`** | **361.16** | **357.85** | **361.26** | **1486.6** | **-39.8% (1.66×)** | **-8.3% (1.09×)** |

### 2.2 阶段耗时拆解（`v1`: `207 B -> 563 B`）

通过对比 `btype=2`（动态树 + 解码循环）与 `btype=1`（编译期静态树，纯解码循环），精确分离建表与解码两阶段开销：

| 阶段 | `picoflate_v0` | `libdeflate v1.22` | **`picoflate`** | 核心收益来源 |
| :--- | ---: | ---: | ---: | :--- |
| **阶段 A：动态树头 + RLE + 建表** | ~509 ns | ~1146 ns | **~291 ns** | 预烘焙 RLE 消除 `lens[320]` 缓冲与 233 个零符号 `offsets[0]++` 转发停顿；32B 内联翻倍消除 17 次外部 `memcpy`/`memset` 调用 |
| **阶段 B：LZ77 + Huffman 解码循环** | ~600 ns | ~394 ns | **~361 ns** | 32-bit 预烘焙表项（消除 4 张 Base/Extra 表查询）+ 边界外提 `FastLoop` + 下一符号 Preload 重叠 `Copy8` 匹配拷贝 |
| **总计** | **1110 ns** | **1540 ns** | **652 ns** | **相比 `libdeflate v1.22` 提速 2.36×，相比 `v0` 降低 41.2%** |

### 2.3 AWS `c7i`（`x86-64-v3`）基线对照与预期

- 优化前在 AWS `c7i` 上针对同一样本 `v1`（`207 B -> 563 B`，10 万次）实测：
  - 系统 `zlib 1.2.11`：`2.71 µs`
  - `picoflate_v0`：`2.63 µs`
  - `libdeflate v1.22`：`2.18 µs`
- 经 `-arch x86_64 -march=x86-64-v3 -O3` 汇编级核验，`picoflate` 完全消除了基线版中的 13 次 `call _memset`、3 次 `call BuildTable`、AVX2 过度向量化导致的多次 `vzeroupper` 以及栈 spill，并显式启用 BMI2 `_bzhi_u64` 指令；按同比例 `-41.2%` 关键路径缩减折算，在 `c7i` 上预期延迟约为 **`1.55–1.70 µs`**。

---

## 3. 安全性与正确性测试结果

`tests/picoflate_test.cc` 在 `-std=c++26 -O2 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined` 下全部通过：

- **真实线上 WebSocket 压缩帧向量（2 组）**：与 `zlib` 解压结果逐字节完全一致。
- **RFC 1951 全模式矩阵（1,072 组精确堆红区边界用例）**：
  - 覆盖压缩等级 `0`（`btype=0` Stored 未压缩块）、`1`、`6`、`9`；
  - 覆盖策略 `Z_DEFAULT_STRATEGY`、`Z_FIXED`（`btype=1`）、`Z_HUFFMAN_ONLY`（0 距离码动态树）、`Z_RLE`（`dist=1` 游程至最大匹配长度 258）；
  - 覆盖多块 `Z_SYNC_FLUSH` 连续块拼接、短距离重叠回溯（`dist = 2..20`）、Fibonacci 词频分布强制 15-bit 子表分配；
  - 每组均使用 `malloc(exact_len)` 输入缓冲与 `malloc(exact_out + 16)` 输出缓冲（ASan 红区紧贴边界），并对 **`0 .. comp_len - 1` 所有严格前缀截断**逐一验证内存安全（零 1B 越界读、零超 `dst_cap + 16` 写）。
- **手工构造畸形码流拦截**：`btype=3` 保留块、固定树非法符号 `286`、非法距离符号 `30`、首字节前越界回溯 `dist > out - dst`、容量不足 `dst_cap = expected - 1` 全部准确拒绝返回 `false`。
- **差分 Fuzz 压测（25,000 次）**：随机比特翻转与随机字节流在 ASan + UBSan 下零崩溃、零未定义行为，所有接受样本与 `zlib` 100% 逐字节匹配。

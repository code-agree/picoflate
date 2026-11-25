# picoflate 测试验证与性能基准报告

> 编译标准：`-std=c++26 -O3`（无缝兼容 `-std=c++23`）

---

## 1. 测试环境与方法论

- **测试平台**：Apple M4（`arm64-apple-darwin25.4.0`，Apple Clang 21.0.0，`QOS_CLASS_USER_INTERACTIVE` 绑定性能核）。
- **可复现负载设计**（全部在 [`bench/picoflate_bench.cc`](../picoflate_bench.cc) 中由明文确定性压缩生成，覆盖 `280 B – 7.1 KB` 规模阶梯）：
  1. `json_event_320b_dyn`（`173 B -> 280 B`）：单条轻量 WebSocket / IoT 节点遥测事件（`btype=2` 动态树）；
  2. `json_rpc_560b_dyn`（`203 B -> 520 B`）：典型微服务 JSON-RPC 2.0 集群状态响应（`btype=2` 动态树）；
  3. `json_batch_1k4_dyn`（`279 B -> 1142 B`）：多实例网关指标同步批次（`btype=2` 动态树）；
  4. `json_page_8k_dyn`（`1278 B -> 7136 B`）：64 条结构化 HTTP 访问日志分页（`btype=2` 动态树）；
  5. `json_rpc_560b_fixed`（`233 B -> 520 B`）：对同一 `520 B` JSON-RPC 负载使用 `Z_FIXED`（`btype=1` 静态树），用于剥离动态建表、单独测量纯 LZ77 + Huffman 解码主循环。
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

| 用例 ID | 负载特征 | 压缩 → 解压大小 | 实现 | Median (ns) | Min (ns) | Mean (ns) | 吞吐 (MiB/s) | 相对 `v0` | 相对 `libdeflate v1.22` |
| :--- | :--- | :---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| `json_event_320b_dyn` | Tiny JSON Event (`btype=2`) | `173 B -> 280 B` | `zlib` | 1150.11 | 1138.35 | 1150.93 | 232.2 | +24.3% | -19.7% |
| `json_event_320b_dyn` | Tiny JSON Event (`btype=2`) | `173 B -> 280 B` | `picoflate_v0` | 925.17 | 920.89 | 927.69 | 288.6 | baseline | -35.4% |
| `json_event_320b_dyn` | Tiny JSON Event (`btype=2`) | `173 B -> 280 B` | `libdeflate v1.22` | 1431.82 | 1425.22 | 1445.64 | 186.5 | +54.8% | baseline |
| **`json_event_320b_dyn`** | **Tiny JSON Event (`btype=2`)** | **`173 B -> 280 B`** | **`picoflate`** | **593.79** | **578.83** | **599.82** | **449.7** | **-35.8% (1.56×)** | **-58.5% (2.41×)** |
| `json_rpc_560b_dyn` | Small JSON-RPC Message (`btype=2`) | `203 B -> 520 B` | `zlib` | 1226.10 | 1208.94 | 1225.91 | 404.5 | +18.3% | -21.2% |
| `json_rpc_560b_dyn` | Small JSON-RPC Message (`btype=2`) | `203 B -> 520 B` | `picoflate_v0` | 1036.71 | 1021.93 | 1038.04 | 478.4 | baseline | -33.4% |
| `json_rpc_560b_dyn` | Small JSON-RPC Message (`btype=2`) | `203 B -> 520 B` | `libdeflate v1.22` | 1555.72 | 1553.59 | 1561.75 | 318.8 | +50.1% | baseline |
| **`json_rpc_560b_dyn`** | **Small JSON-RPC Message (`btype=2`)** | **`203 B -> 520 B`** | **`picoflate`** | **667.24** | **656.57** | **665.31** | **743.2** | **-35.6% (1.55×)** | **-57.1% (2.33×)** |
| `json_batch_1k4_dyn` | Structured JSON Batch (`btype=2`) | `279 B -> 1142 B` | `zlib` | 1368.45 | 1361.20 | 1372.69 | 795.9 | +1.6% | -25.3% |
| `json_batch_1k4_dyn` | Structured JSON Batch (`btype=2`) | `279 B -> 1142 B` | `picoflate_v0` | 1346.93 | 1333.22 | 1363.68 | 808.6 | baseline | -26.5% |
| `json_batch_1k4_dyn` | Structured JSON Batch (`btype=2`) | `279 B -> 1142 B` | `libdeflate v1.22` | 1832.51 | 1741.92 | 1823.65 | 594.3 | +36.1% | baseline |
| **`json_batch_1k4_dyn`** | **Structured JSON Batch (`btype=2`)** | **`279 B -> 1142 B`** | **`picoflate`** | **833.65** | **816.28** | **834.22** | **1306.5** | **-38.1% (1.62×)** | **-54.5% (2.20×)** |
| `json_page_8k_dyn` | 8 KB Log / API Page (`btype=2`) | `1278 B -> 7136 B` | `zlib` | 3266.51 | 3238.36 | 3269.67 | 2083.4 | -32.9% | -15.1% |
| `json_page_8k_dyn` | 8 KB Log / API Page (`btype=2`) | `1278 B -> 7136 B` | `picoflate_v0` | 4865.39 | 4817.42 | 5083.47 | 1398.7 | baseline | +26.5% |
| `json_page_8k_dyn` | 8 KB Log / API Page (`btype=2`) | `1278 B -> 7136 B` | `libdeflate v1.22` | 3846.94 | 3538.97 | 3989.20 | 1769.1 | -20.9% | baseline |
| **`json_page_8k_dyn`** | **8 KB Log / API Page (`btype=2`)** | **`1278 B -> 7136 B`** | **`picoflate`** | **2670.09** | **2649.28** | **2695.97** | **2548.7** | **-45.1% (1.82×)** | **-30.6% (1.44×)** |
| `json_rpc_560b_fixed` | Fixed-Tree Decode Loop (`btype=1`) | `233 B -> 520 B` | `zlib` | 433.06 | 430.73 | 435.88 | 1145.2 | -24.0% | +16.1% |
| `json_rpc_560b_fixed` | Fixed-Tree Decode Loop (`btype=1`) | `233 B -> 520 B` | `picoflate_v0` | 569.77 | 551.41 | 594.33 | 870.4 | baseline | +52.7% |
| `json_rpc_560b_fixed` | Fixed-Tree Decode Loop (`btype=1`) | `233 B -> 520 B` | `libdeflate v1.22` | 373.08 | 355.88 | 379.35 | 1329.3 | -34.5% | baseline |
| **`json_rpc_560b_fixed`** | **Fixed-Tree Decode Loop (`btype=1`)** | **`233 B -> 520 B`** | **`picoflate`** | **344.27** | **341.31** | **343.89** | **1440.5** | **-39.6% (1.66×)** | **-7.7% (1.08×)** |

### 2.2 阶段耗时拆解（`Small JSON-RPC Message`: `203 B -> 520 B`）

通过对比同一 `520 B` 明文的 `btype=2`（动态树 + 解码循环）与 `btype=1`（编译期静态树，纯解码循环），精确分离建表与解码两阶段开销：

| 阶段 | `picoflate_v0` | `libdeflate v1.22` | **`picoflate`** | 核心收益来源 |
| :--- | ---: | ---: | ---: | :--- |
| **阶段 A：动态树头 + RLE + 建表** | ~467 ns | ~1183 ns | **~323 ns** | 预烘焙 RLE 消除 `lens[320]` 缓冲与 222 个零符号 `offsets[0]++` 转发停顿；32B 内联翻倍消除 17 次外部 `memcpy`/`memset` 调用 |
| **阶段 B：LZ77 + Huffman 解码循环** | ~570 ns | ~373 ns | **~344 ns** | 32-bit 预烘焙表项（消除 4 张 Base/Extra 表查询）+ 边界外提 `FastLoop` + 下一符号 Preload 重叠 `Copy8` 匹配拷贝 |
| **总计** | **1037 ns** | **1556 ns** | **667 ns** | **相比 `libdeflate v1.22` 提速 2.33×，动态建表快 3.66×** |

---

## 3. 安全性与正确性测试结果

`tests/picoflate_test.cc` 在 `-std=c++26 -O2 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined` 下全部通过：

- **通用 JSON 事件与 JSON-RPC 基准向量（2 组）**：精确红区边界与全前缀截断逐字节匹配 `zlib`。
- **RFC 1951 全模式矩阵（1,072 组精确堆红区边界用例）**：
  - 覆盖压缩等级 `0`（`btype=0` Stored 未压缩块）、`1`、`6`、`9`；
  - 覆盖策略 `Z_DEFAULT_STRATEGY`、`Z_FIXED`（`btype=1`）、`Z_HUFFMAN_ONLY`（0 距离码动态树）、`Z_RLE`（`dist=1` 游程至最大匹配长度 258）；
  - 覆盖多块 `Z_SYNC_FLUSH` 连续块拼接、短距离重叠回溯（`dist = 2..20`）、Fibonacci 词频分布强制 15-bit 子表分配；
  - 每组均使用 `malloc(exact_len)` 输入缓冲与 `malloc(exact_out + 16)` 输出缓冲（ASan 红区紧贴边界），并对 **`0 .. comp_len - 1` 所有严格前缀截断**逐一验证内存安全（零 1B 越界读、零超 `dst_cap + 16` 写）。
- **手工构造畸形码流拦截**：`btype=3` 保留块、固定树非法符号 `286`、非法距离符号 `30`、首字节前越界回溯 `dist > out - dst`、容量不足 `dst_cap = expected - 1` 全部准确拒绝返回 `false`。
- **差分 Fuzz 压测（25,000 次）**：随机比特翻转与随机字节流在 ASan + UBSan 下零崩溃、零未定义行为，所有接受样本与 `zlib` 100% 逐字节匹配。

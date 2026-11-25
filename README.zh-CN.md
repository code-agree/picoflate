# picoflate

**从第一性原理构建的亚微秒级、零堆分配、单头文件 C++23/26 RFC 1951 Raw Deflate（`wbits = -15`）解压引擎——专为 `100 B – 16 KB` 高频实时小包流设计。**

[English](README.md) | [简体中文](README.zh-CN.md)

---

## 第一性原理：为什么 `libdeflate` 与 `zlib` 在亚 KB 级实时小包上撞上性能墙？

主流工业级 DEFLATE 库（`zlib`、`zlib-ng`、`libdeflate`）的目标函数都是**渐近大块吞吐率**（针对 $\ge 64\text{ KB}$ 文件的 $\text{MB/s}$）：

$$T_{\text{total}}(N) = T_{\text{tree}} + N \cdot c_{\text{token}}$$

当负载 $N \ge 64\text{ KB}$ 时，约 `1.2 µs` 的动态 Huffman 建表开销 $T_{\text{tree}}$ 被摊薄到不足 `1%`。

但在低延迟实时流场景——无状态 WebSocket `permessage-deflate`（`server_no_context_takeover`）、JSON-RPC 微服务响应、IoT 遥测与结构化日志微批次（$N \in [100\text{ B}, 8\text{ KB}]$）中，**物理约束发生了根本反转**：

1. **每一帧都携带独立的 `btype = 2` 动态 Huffman 树头**：解剖 [`bench/picoflate_bench.cc`](bench/picoflate_bench.cc) 中的 `Small JSON-RPC Message` 样本（压缩态 `203 B` $\to$ 解压态 `520 B` JSON-RPC 响应）：
   - 动态 Huffman 树头占 **`333 bits`（`41.6 B`，占整包压缩字节的 `20.5%`）**；
   - 实际数据负载仅有 **`161` 个字面量 + `33` 次短距离 LZ77 匹配**（纯解码耗时仅 `~344 ns`）。
2. **超过 80% 的 RFC 1951 字母表是全零死区**：在典型 JSON/ASCII 微帧中，`272` 个字面量/长度符号里仅有 **`50` 个活跃非零符号**（占 `18.4%`，`max_len = 8`），其余 **`222` 个符号码长全为 0**。
3. **`libdeflate v1.22` 在解码第 1 个字节前就花掉了 76% 的总时间**：
   - 在 `203 B -> 520 B` 样本上，`libdeflate v1.22` 耗时 `1556 ns`，其中 **`~1183 ns`（`76.0%`）消耗在动态树建表阶段**；
   - 它先分配并写出 `lens[320]` 数组，随即重扫全部 `272` 个符号并无条件执行 `sorted_syms[offsets[lens[sym]]++] = sym`——对同一个栈地址 `offsets[0]` 连续执行 **222 次读写递增**，触发严重的 CPU **Store-to-Load Forwarding（存储转发）流水线停顿**，随后又无视实际信息熵强行填充固定宽度大表。

**`picoflate` 从第一性原理出发同时击穿 $T_{\text{tree}}$ 与 $c_{\text{token}}$**：将动态树建表耗时压缩 **`3.66×`**（`323 ns` vs `1183 ns`），同时在纯解码循环上继续领先 `libdeflate v1.22` `8.4%`（`344 ns` vs `373 ns`），在 `280 B – 7.1 KB` 全梯度负载上以零外部依赖单头文件实现 **`1.44×–2.41×` 的端到端加速**。

---

## 核心技术亮点

### 1. 零物化预烘焙 RLE（`lens[]`-Free 动态树解码）
将前导码（Precode）中代表零游程的三个符号 `{0, 17, 18}` 在编译期预烘焙为最高位置 1 的 32-bit 表项（`kLiteralFlag | (base_rep << 16) | extra_bits`）。RLE 展开时，所有零码长区间直接通过单条无分支移位加法推进符号游标，**全程不分配、不写入、不重扫 `lens[320]` 缓冲区**，仅就地收集约 35–50 个非零活跃符号，彻底根除 `offsets[0]++` 存储转发停顿与外部 `memset` 调用。

### 2. 信息熵自适应主表宽度 + 寄存器内联成倍复制
根据帧内真实最大码长动态收缩主表宽度至 $\min(10, L_{\max})$（如 `max_len = 8` 时主表仅 `256` 项 = `1 KB`，驻留 L1D），配合 `std::bit_floor`（`[[assume]]`）反序码字自增与 32 字节内联块翻倍（`DoubleTable`），零 PLT `memcpy` 调用完成建表。

### 3. 单次移位 32-bit 预烘焙解码项 + 预取重叠 FastLoop
- **单表项折叠全部解码元数据**：将字面量符号位判定（`int32_t < 0`）、匹配基数、`codeword_bits` 与 `codeword_bits + extra_bits` 打包进单个 `uint32_t`，彻底省去 4 张独立的 Base/Extra 查表与二次分支。
- **访存与拷贝指令级重叠**：外提边界检查的 `FastLoop` 内置 3× 连续字面量展开与无分支 64-bit 比特缓冲补水（`RefillBranchless`），并在执行 16 字节非对齐 LZ77 匹配拷贝（`Copy8 × 2`）**之前**提前发出下一符号的 `ll_tab` 预取与比特补水，用内存存储吞吐完全掩盖 L1 查表延迟。
- **微架构指令集克制**：在 `x86-64` 下显式调用 BMI2 `_bzhi_u64` 提取变长比特，并通过 `NOVECTOR` 编译指示抑制短匹配尾循环过度自动向量化带来的 AVX2 `vzeroupper` 频率切换惩罚。

### 4. 零开销 C++26 就绪与零热路径指令安全陷阱
- **C++26 `[[indeterminate]]`（P2795R5）**：显式标注栈上临时建表缓冲，消除 `-std=c++26` 默认栈变量错误值清零带来的 ~1.7 KB/帧隐式开销。
- **零热路径开销的 RFC 1951 边界拦截**：将非法保留符号（`286..287`、距离码 `30..31`）与不完全单符号空槽统一预编码为 `kExceptional`，正常热路径增加 **0 条额外检查指令**，配合精确到比特的 `overread_count` 截断校验，完整通过 **1,074 组 ASan 精确堆红区边界矩阵（含全部严格前缀截断）+ 25,000 次对拍 `zlib` 差分 Fuzz**。

---

## 实测性能基准

全部测试负载均由 [`bench/picoflate_bench.cc`](bench/picoflate_bench.cc) 中的可读明文确定性生成，经 `100,000 次迭代 × 7 轮`（`-std=c++26 -O3`，绑定性能大核）实测；原始逐轮 CSV 见 [`bench/results/rounds.csv`](bench/results/rounds.csv)，汇总见 [`bench/results/summary.csv`](bench/results/summary.csv)，详细拆解见 [`bench/results/benchmark_report.md`](bench/results/benchmark_report.md)：

| 测试负载（按尺寸梯度） | 压缩 $\to$ 解压 | `zlib` | `libdeflate v1.22` | **`picoflate`** | 相比 `libdeflate v1.22` | 相比 `zlib` |
| :--- | :---: | ---: | ---: | ---: | ---: | ---: |
| **Tiny JSON Event (`btype=2` Dynamic)** | `173 B -> 280 B` | 1150 ns | 1432 ns | **594 ns** *(450 MiB/s)* | **快 2.41× (-58.5%)** | **快 1.94× (-48.4%)** |
| **Small JSON-RPC Message (`btype=2` Dynamic)** | `203 B -> 520 B` | 1226 ns | 1556 ns | **667 ns** *(743 MiB/s)* | **快 2.33× (-57.1%)** | **快 1.84× (-45.6%)** |
| **Structured JSON Batch (`btype=2` Dynamic)** | `279 B -> 1142 B` | 1368 ns | 1833 ns | **834 ns** *(1307 MiB/s)* | **快 2.20× (-54.5%)** | **快 1.64× (-39.1%)** |
| **8 KB Log / API Page (`btype=2` Dynamic)** | `1278 B -> 7136 B` | 3267 ns | 3847 ns | **2670 ns** *(2549 MiB/s)* | **快 1.44× (-30.6%)** | **快 1.22× (-18.3%)** |
| **Fixed-Tree Decode Loop (`btype=1` Fixed)** | `233 B -> 520 B` | 433 ns | 373 ns | **344 ns** *(1441 MiB/s)* | **快 1.08× (-7.7%)** | **快 1.26× (-20.5%)** |

---

## 适用边界（选型指南）

- **推荐使用 `picoflate`**：`100 B – 16 KB` 低延迟 Raw Deflate 实时流（无状态 WebSocket `permessage-deflate` 帧、JSON-RPC 微服务消息、IoT 遥测与日志微批次），或追求单头文件、零运行时依赖、零堆分配的现代 C++23/26 工程。
- **推荐使用 `libdeflate` / `zlib-ng`**：`>= 64 KB` 的大文件或归档批处理、`gzip`/`zlib` 容器校验（CRC32/Adler32）以及数据压缩（Deflate 编码）。

---

## 快速集成

```cpp
#include "picoflate/picoflate.h"
#include <vector>

picoflate::Inflater inflater;  // ~7.6 KB 常驻 L1D 缓存，Inflate() 热路径零堆分配

constexpr std::size_t kMaxPayload = 65536;
// 注意：输出缓冲区须在 dst_cap 之外额外预留至少 kRequiredOutputSlack (16) 字节可写冗余空间
std::vector<char> out_buf(kMaxPayload + picoflate::Inflater::kRequiredOutputSlack);

std::size_t out_len = 0;
if (inflater.Inflate(compressed_ptr, compressed_len, out_buf.data(), kMaxPayload, out_len)) {
    // 解压结果位于 out_buf[0 .. out_len)
}
```

一键运行 ASan + UBSan 边界测试、25,000 次差分 Fuzz 与 7×100k 微基准测试：

```bash
./bench/run_bench.sh
```

## 开源协议

[MIT License](LICENSE)

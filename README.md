# fast-raw-deflate

Ultra-fast, zero-allocation, header-only C++23/C++26 **RFC 1951 Raw Deflate (`wbits = -15`)** decompressor specifically engineered for low-latency, small-to-medium payloads (`100 B .. 16 KB`, such as exchange WebSocket compressed L2 order-book incremental diffs).

## Highlights

- **Header-only & Zero Runtime Dependencies**: Single self-contained header (`include/fast_raw_deflate/raw_deflate.h`), zero heap allocations on hot path, ~7.6 KB reusable instance state fitting inside L1D cache.
- **Faster than `libdeflate v1.22` and `zlib` on Small/Medium Dynamic Huffman Payloads**:
  - **Fused Pre-baked RLE + Active-Symbol Collector**: Folds precode zero-runs (`{0, 17, 18}`) into a single branchless index advance without allocating or rescanning a `lens[320]` buffer, eliminating RLE zero-stores and Store-to-Load Forwarding stalls (~230 unused symbols skipped in typical JSON diffs).
  - **Dynamic Primary Table Width + Incremental Table Doubling**: Builds `min(10, max_len)` primary `litlen` tables (`128`–`1024` entries) via `std::bit_floor` + inline 32-byte block doubling with zero `memcpy`/`memset` calls.
  - **32-bit Pre-baked Decode Entries + Bounds-Hoisted FastLoop**: Folds literal sign flag, match length/offset bases, and `codeword_bits + extra_bits` into a single lookup and shift; overlaps next-symbol table preload and branchless 64-bit bitbuffer refill with 8B/16B unaligned LZ77 match copies; uses explicit BMI2 `_bzhi_u64` on x86-64.
- **C++23 / C++26 Ready**: Uses `<bit>` (`std::bit_floor` + `[[assume]]`) and C++26 `[[indeterminate]]` (P2795R5) to avoid automatic stack-array zero-initialization overhead under `-std=c++26`.
- **Strict RFC 1951 Validation & Fuzz-Tested**: Validates oversubscribed/incomplete Huffman trees, rejects reserved symbols (`286..287`, distance `30..31`, `btype=3`), checks RLE/back-reference bounds, and enforces bit-exact end-of-stream overread checks. Verified with **1,074 ASan/UBSan exact-redzone boundary streams** (including all prefix truncations) + **25,000 differential fuzz cases** against `zlib`.

---

## Benchmark Summary

Measured with `100,000 iterations × 7 rounds` (`-std=c++26 -O3`, P-core pinned) on real mainnet Hyperliquid `l2` compressed order-book diffs and synthetic 40-level diffs (raw CSV data in [`bench/results/summary.csv`](bench/results/summary.csv) and [`bench/results/rounds.csv`](bench/results/rounds.csv); full analysis in [`bench/results/benchmark_report.md`](bench/results/benchmark_report.md)):

| Workload | Size (`in -> out`) | `zlib` | `FastRawDeflate v0` | `libdeflate v1.22` | **`FastRawDeflate`** | Speedup vs `v0` | Speedup vs `zlib` |
| :--- | :---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **Mainnet `l2` diff `v1` (`btype=2` Dynamic)** | `207 B -> 563 B` | 1217 ns | 1110 ns | 1540 ns | **652 ns** | **-41.2% (1.70×)** | **-46.4% (1.87×)** |
| **Mainnet `l2` diff `v2` (`btype=2` Dynamic)** | `148 B -> 323 B` | 1064 ns | 859 ns | 1400 ns | **509 ns** | **-40.7% (1.69×)** | **-52.1% (2.09×)** |
| **40-level `l2` diff (`btype=2` Dynamic)** | `316 B -> 1345 B` | 1474 ns | 1602 ns | 1812 ns | **902 ns** | **-43.7% (1.78×)** | **-38.9% (1.64×)** |
| **Mainnet `v1` Pure Loop (`btype=1` Fixed)** | `251 B -> 563 B` | 458 ns | 600 ns | 394 ns | **361 ns** | **-39.8% (1.66×)** | **-21.1% (1.27×)** |

> **x86-64 (AWS c7i Sapphire Rapids) Reference**: On `v1` (`207 B -> 563 B`, 100k iterations), `zlib 1.2.11` measured `2.71 µs`, `FastRawDeflate v0` measured `2.63 µs`, and `libdeflate v1.22` measured `2.18 µs`. With the `-41.2%` critical-path & tree-construction reduction (eliminating 13 `memset` PLT calls, `vzeroupper` stalls, `lens[]` store-forwarding stalls, and adding BMI2 `_bzhi_u64` + preloaded fastloop), `FastRawDeflate` projects to **`~1.55–1.70 µs`** on c7i.

---

## Quick Start

### Usage (Header-Only)

```cpp
#include "fast_raw_deflate/raw_deflate.h"
#include <vector>

fast_raw_deflate::FastRawDeflate inflater;  // Reusable, zero-allocation on Inflate()

std::size_t max_out = 65536;
// Note: allocate at least kRequiredOutputSlack (16) extra writable bytes beyond dst_cap!
std::vector<char> out_buf(max_out + fast_raw_deflate::FastRawDeflate::kRequiredOutputSlack);

std::size_t out_len = 0;
if (inflater.Inflate(compressed_ptr, compressed_len, out_buf.data(), max_out, out_len)) {
    // Decompressed payload is in out_buf[0 .. out_len)
}
```

### Run Tests (ASan + UBSan + 25k Fuzz) & Benchmarks

```bash
./bench/run_bench.sh
```

Or via CMake:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFAST_RAW_DEFLATE_USE_CXX26=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/fast_raw_deflate_bench --iters 100000 --rounds 7
```

## License

MIT License — see [LICENSE](LICENSE).

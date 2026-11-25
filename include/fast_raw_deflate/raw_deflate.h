// SPDX-License-Identifier: MIT
// Author: yuwenjun | Date: 2026-11-25
#pragma once

#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__GNUC__) || defined(__clang__)
#define FAST_RAW_DEFLATE_FORCE_INLINE inline __attribute__((always_inline))
#define FAST_RAW_DEFLATE_NOINLINE     __attribute__((noinline))
#define FAST_RAW_DEFLATE_HOT          __attribute__((hot))
#define FAST_RAW_DEFLATE_LIKELY(x)    __builtin_expect(!!(x), 1)
#define FAST_RAW_DEFLATE_UNLIKELY(x)  __builtin_expect(!!(x), 0)
#define FAST_RAW_DEFLATE_RESTRICT     __restrict__
#elif defined(_MSC_VER)
#define FAST_RAW_DEFLATE_FORCE_INLINE __forceinline
#define FAST_RAW_DEFLATE_NOINLINE     __declspec(noinline)
#define FAST_RAW_DEFLATE_HOT
#define FAST_RAW_DEFLATE_LIKELY(x)    (x)
#define FAST_RAW_DEFLATE_UNLIKELY(x)  (x)
#define FAST_RAW_DEFLATE_RESTRICT     __restrict
#else
#define FAST_RAW_DEFLATE_FORCE_INLINE inline
#define FAST_RAW_DEFLATE_NOINLINE
#define FAST_RAW_DEFLATE_HOT
#define FAST_RAW_DEFLATE_LIKELY(x)    (x)
#define FAST_RAW_DEFLATE_UNLIKELY(x)  (x)
#define FAST_RAW_DEFLATE_RESTRICT
#endif

#if defined(__clang__)
#define FAST_RAW_DEFLATE_NOVECTOR \
    _Pragma("clang loop vectorize(disable) interleave(disable)")
#elif defined(__GNUC__)
#define FAST_RAW_DEFLATE_NOVECTOR _Pragma("GCC novector")
#else
#define FAST_RAW_DEFLATE_NOVECTOR
#endif

// C++26 P2795R5 opt-out from automatic erroneous-value zeroing on scratch arrays
#if __has_cpp_attribute(indeterminate)
#define FAST_RAW_DEFLATE_INDETERMINATE [[indeterminate]]
#else
#define FAST_RAW_DEFLATE_INDETERMINATE
#endif

namespace fast_raw_deflate {

// Ultra-fast, zero-allocation, header-only one-shot RFC 1951 raw-deflate (wbits = -15)
// decoder tuned for small-to-medium low-latency payloads (100 B .. 16 KB, such as
// exchange WebSocket compressed order-book diffs).
//
// Key design choices:
// 1. Fused pre-baked RLE + active-symbol collector: precode symbols {0, 17, 18} fold
//    zero-runs into a single branchless index advance without allocating or scanning
//    a `lens[320]` buffer (eliminating RLE zero-stores and store-forwarding stalls).
// 2. Dynamic primary litlen table width `min(10, max_len)` + power-of-2 incremental
//    table doubling + 32-bit pre-baked decode entries folding literal flags, match
//    bases, and extra-bit counts into a single lookup and shift.
// 3. Bounds-hoisted fastloop with branchless 64-bit bitbuffer refills, entry
//    preloading overlapped with LZ77 match copies, 3x fast-literal unrolling, and
//    BMI2 `_bzhi_u64` bitfield extraction on x86-64.
// 4. C++23/C++26 clean (`std::bit_floor` + `[[assume]]`, C++26 `[[indeterminate]]`)
//    with strict RFC 1951 validation (oversubscribed/incomplete trees, invalid symbols
//    286..287 & dist 30..31, RLE/offset bounds, and exact bit-level overread checks).
class FastRawDeflate {
public:
    static constexpr unsigned kPrecodeBits = 7;
    static constexpr std::size_t kPrecodeTableSize = 1U << kPrecodeBits;  // 128

    static constexpr unsigned kLitLenMaxBits = 10;
    static constexpr std::size_t kLitLenTableCap = 1408;  // > ENOUGH(288, 10, 15) = 1334

    static constexpr unsigned kDistBits = 7;
    static constexpr std::size_t kDistTableCap = 512;     // > ENOUGH(32, 7, 15) = 402
    static constexpr std::uint64_t kDistMask = (1ULL << kDistBits) - 1ULL;

    static constexpr std::uint32_t kLiteralFlag = 0x80000000U;
    static constexpr std::uint32_t kExceptional = 0x00008000U;
    static constexpr std::uint32_t kSubtablePtr = 0x00004000U;
    static constexpr std::uint32_t kEndOfBlock  = 0x00002000U;

    // Required writable slack bytes beyond `dst + dst_cap` for branchless 8B/16B match copies.
    static constexpr std::size_t kRequiredOutputSlack = 16;

    // One FastLoop iteration starting with `in_end - in >= 16` performs at most two
    // 8-byte branchless refills (advancing `in` by <= 7 before the 2nd load), so it
    // never reads past `in_0 + 15`. Output overruns `out_next` by <= 14 bytes, within
    // the caller's 16-byte writable slack contract beyond `dst_cap`.
    static constexpr std::size_t kFastLoopInMargin = 15;
    static constexpr std::size_t kFastLoopPreRefillMargin = kFastLoopInMargin + 7;  // 22
    static constexpr std::size_t kFastLoopMaxBytesWritten = 2 + 258;

    FastRawDeflate() noexcept = default;

    // Decompresses raw-deflate `src[0..src_len)` into `dst[0..dst_cap)`.
    // Caller MUST ensure `dst` has at least `kRequiredOutputSlack` (16) extra writable
    // slack bytes beyond `dst_cap` (i.e. buffer allocation >= `dst_cap + 16`).
    [[nodiscard]] FAST_RAW_DEFLATE_HOT bool Inflate(
        const std::uint8_t* FAST_RAW_DEFLATE_RESTRICT src, std::size_t src_len,
        char* FAST_RAW_DEFLATE_RESTRICT dst, std::size_t dst_cap,
        std::size_t& out_len) noexcept {
        if (FAST_RAW_DEFLATE_UNLIKELY(src_len == 0 || dst_cap == 0)) return false;

        const std::uint8_t* in = src;
        const std::uint8_t* const in_end = src + src_len;
        const std::uint8_t* const in_fast_end =
            src_len > kFastLoopInMargin ? (in_end - kFastLoopInMargin) : src;

        char* out = dst;
        char* const out_end = dst + dst_cap;
        char* const out_fast_end =
            dst_cap > kFastLoopMaxBytesWritten ? (out_end - kFastLoopMaxBytesWritten) : dst;

        std::uint64_t bitbuf = 0;
        std::uint32_t bitsleft = 0;
        std::size_t overread_count = 0;

        bool is_final = false;
        do {
            if (FAST_RAW_DEFLATE_UNLIKELY(
                    !RefillSafe(in, in_end, bitbuf, bitsleft, overread_count))) {
                return false;
            }
            is_final = (bitbuf & 1U) != 0;
            const unsigned btype = static_cast<unsigned>((bitbuf >> 1) & 3U);

            const std::uint32_t* ll_tab = nullptr;
            const std::uint32_t* d_tab = nullptr;
            std::uint64_t ll_mask = 0;

            if (FAST_RAW_DEFLATE_LIKELY(btype == 2U)) {
                const DynHeaderResult dyn =
                    DecodeDynamicTrees(in, in_end, bitbuf, bitsleft, overread_count);
                const unsigned ll_bits = static_cast<unsigned>(dyn.packed >> 48);
                if (FAST_RAW_DEFLATE_UNLIKELY(ll_bits == 0)) return false;
                in += static_cast<std::uint32_t>(dyn.packed);
                bitbuf = dyn.bitbuf;
                bitsleft = static_cast<std::uint8_t>(dyn.packed >> 32);
                overread_count = static_cast<std::uint8_t>(dyn.packed >> 40);
                ll_tab = dyn_litlen_.data();
                d_tab = dyn_dist_.data();
                ll_mask = (1ULL << ll_bits) - 1ULL;
            } else if (btype == 1U) {
                bitbuf >>= 3;
                bitsleft -= 3U;
                ll_tab = kFixedLitLenTable.data();
                d_tab = kFixedDistTable.data();
                ll_mask = (1ULL << 9U) - 1ULL;
            } else if (btype == 0U) {
                bitsleft = static_cast<std::uint8_t>(bitsleft - 3U);
                if (FAST_RAW_DEFLATE_UNLIKELY(overread_count > (bitsleft >> 3))) return false;
                in -= (bitsleft >> 3) - overread_count;
                overread_count = 0;
                bitbuf = 0;
                bitsleft = 0;
                if (FAST_RAW_DEFLATE_UNLIKELY(in_end - in < 4)) return false;
                std::uint16_t len = 0;
                std::uint16_t nlen = 0;
                std::memcpy(&len, in, 2);
                std::memcpy(&nlen, in + 2, 2);
                in += 4;
                if (FAST_RAW_DEFLATE_UNLIKELY(len != static_cast<std::uint16_t>(~nlen))) {
                    return false;
                }
                if (FAST_RAW_DEFLATE_UNLIKELY(len > static_cast<std::size_t>(in_end - in) ||
                                              len > static_cast<std::size_t>(out_end - out))) {
                    return false;
                }
                std::memcpy(out, in, len);
                in += len;
                out += len;
                continue;
            } else {
                return false;
            }

            // Fast decode loop: bounds checks hoisted to loop condition, all state in registers.
            if (static_cast<std::size_t>(in_end - in) > kFastLoopPreRefillMargin &&
                out < out_fast_end) {
                RefillBranchless(in, bitbuf, bitsleft);
                std::uint32_t entry = ll_tab[bitbuf & ll_mask];
                do {
                    std::uint64_t saved_bitbuf = bitbuf;
                    bitbuf >>= static_cast<std::uint8_t>(entry);
                    bitsleft -= entry;

                    if (static_cast<std::int32_t>(entry) < 0) {
                        *out++ = static_cast<char>(entry >> 16);
                        entry = ll_tab[bitbuf & ll_mask];
                        saved_bitbuf = bitbuf;
                        bitbuf >>= static_cast<std::uint8_t>(entry);
                        bitsleft -= entry;
                        if (static_cast<std::int32_t>(entry) < 0) {
                            *out++ = static_cast<char>(entry >> 16);
                            entry = ll_tab[bitbuf & ll_mask];
                            saved_bitbuf = bitbuf;
                            bitbuf >>= static_cast<std::uint8_t>(entry);
                            bitsleft -= entry;
                            if (static_cast<std::int32_t>(entry) < 0) {
                                *out++ = static_cast<char>(entry >> 16);
                                entry = ll_tab[bitbuf & ll_mask];
                                RefillBranchless(in, bitbuf, bitsleft);
                                continue;
                            }
                        }
                    }

                    if (FAST_RAW_DEFLATE_UNLIKELY((entry & kExceptional) != 0)) {
                        if ((entry & kEndOfBlock) != 0) goto block_done;
                        if (FAST_RAW_DEFLATE_UNLIKELY((entry & kSubtablePtr) == 0)) return false;
                        entry = ll_tab[(entry >> 16) +
                                       ExtractVarBits(bitbuf, (entry >> 8) & 0x3FU)];
                        saved_bitbuf = bitbuf;
                        bitbuf >>= static_cast<std::uint8_t>(entry);
                        bitsleft -= entry;
                        if (static_cast<std::int32_t>(entry) < 0) {
                            *out++ = static_cast<char>(entry >> 16);
                            entry = ll_tab[bitbuf & ll_mask];
                            RefillBranchless(in, bitbuf, bitsleft);
                            continue;
                        }
                        if ((entry & kExceptional) != 0) {
                            if ((entry & kEndOfBlock) != 0) goto block_done;
                            return false;
                        }
                    }

                    const std::uint32_t length =
                        (entry >> 16) +
                        static_cast<std::uint32_t>(ExtractVarBits8(saved_bitbuf, entry) >>
                                                   static_cast<std::uint8_t>(entry >> 8));

                    std::uint32_t d_entry = d_tab[bitbuf & kDistMask];
                    if (FAST_RAW_DEFLATE_UNLIKELY((d_entry & kExceptional) != 0)) {
                        if (FAST_RAW_DEFLATE_UNLIKELY((d_entry & kSubtablePtr) == 0)) return false;
                        if (FAST_RAW_DEFLATE_UNLIKELY(
                                static_cast<std::uint8_t>(bitsleft) < 28U + kLitLenMaxBits)) {
                            RefillBranchless(in, bitbuf, bitsleft);
                        }
                        bitbuf >>= kDistBits;
                        bitsleft -= kDistBits;
                        d_entry = d_tab[(d_entry >> 16) +
                                        ExtractVarBits(bitbuf, (d_entry >> 8) & 0x3FU)];
                        if (FAST_RAW_DEFLATE_UNLIKELY((d_entry & kExceptional) != 0)) return false;
                    } else if (FAST_RAW_DEFLATE_UNLIKELY(
                                   static_cast<std::uint8_t>(bitsleft) <
                                   (kDistBits + 13U) + kLitLenMaxBits)) {
                        RefillBranchless(in, bitbuf, bitsleft);
                    }

                    saved_bitbuf = bitbuf;
                    bitbuf >>= static_cast<std::uint8_t>(d_entry);
                    bitsleft -= d_entry;
                    const std::size_t dist =
                        (d_entry >> 16) +
                        static_cast<std::size_t>(ExtractVarBits8(saved_bitbuf, d_entry) >>
                                                 static_cast<std::uint8_t>(d_entry >> 8));

                    if (FAST_RAW_DEFLATE_UNLIKELY(dist > static_cast<std::size_t>(out - dst))) {
                        return false;
                    }
                    const char* match = out - dist;
                    char* copy_dst = out;
                    out += length;

                    // Preload next litlen entry and refill bitbuf before copying match bytes.
                    entry = ll_tab[bitbuf & ll_mask];
                    RefillBranchless(in, bitbuf, bitsleft);

                    CopyMatchFast(copy_dst, match, out, dist);
                } while (in < in_fast_end && out < out_fast_end);
            }

            // Safe tail loop near end of input/output buffers.
            for (;;) {
                if (FAST_RAW_DEFLATE_UNLIKELY(
                        !RefillSafe(in, in_end, bitbuf, bitsleft, overread_count))) {
                    return false;
                }
                std::uint32_t entry = ll_tab[bitbuf & ll_mask];
                std::uint64_t saved_bitbuf = bitbuf;
                bitbuf >>= static_cast<std::uint8_t>(entry);
                bitsleft -= entry;

                if (FAST_RAW_DEFLATE_UNLIKELY((entry & kSubtablePtr) != 0)) {
                    entry = ll_tab[(entry >> 16) + ExtractVarBits(bitbuf, (entry >> 8) & 0x3FU)];
                    saved_bitbuf = bitbuf;
                    bitbuf >>= static_cast<std::uint8_t>(entry);
                    bitsleft -= entry;
                }
                if (static_cast<std::int32_t>(entry) < 0) {
                    if (FAST_RAW_DEFLATE_UNLIKELY(out == out_end)) return false;
                    *out++ = static_cast<char>(entry >> 16);
                    continue;
                }
                if (FAST_RAW_DEFLATE_UNLIKELY((entry & kExceptional) != 0)) {
                    if ((entry & kEndOfBlock) != 0) goto block_done;
                    return false;
                }

                const std::size_t length =
                    (entry >> 16) +
                    static_cast<std::size_t>(ExtractVarBits8(saved_bitbuf, entry) >>
                                             static_cast<std::uint8_t>(entry >> 8));
                if (FAST_RAW_DEFLATE_UNLIKELY(length > static_cast<std::size_t>(out_end - out))) {
                    return false;
                }

                std::uint32_t d_entry = d_tab[bitbuf & kDistMask];
                if (FAST_RAW_DEFLATE_UNLIKELY((d_entry & kExceptional) != 0)) {
                    if (FAST_RAW_DEFLATE_UNLIKELY((d_entry & kSubtablePtr) == 0)) return false;
                    bitbuf >>= kDistBits;
                    bitsleft -= kDistBits;
                    d_entry = d_tab[(d_entry >> 16) +
                                    ExtractVarBits(bitbuf, (d_entry >> 8) & 0x3FU)];
                    if (FAST_RAW_DEFLATE_UNLIKELY((d_entry & kExceptional) != 0)) return false;
                }
                const std::size_t dist =
                    (d_entry >> 16) +
                    static_cast<std::size_t>(ExtractVarBits8(bitbuf, d_entry) >>
                                             static_cast<std::uint8_t>(d_entry >> 8));
                bitbuf >>= static_cast<std::uint8_t>(d_entry);
                bitsleft -= d_entry;

                if (FAST_RAW_DEFLATE_UNLIKELY(dist > static_cast<std::size_t>(out - dst))) {
                    return false;
                }
                const char* match = out - dist;
                char* copy_dst = out;
                out += length;
                CopyMatchTail(copy_dst, match, out, dist);
            }

        block_done:;
        } while (!is_final);

        bitsleft = static_cast<std::uint8_t>(bitsleft);
        if (FAST_RAW_DEFLATE_UNLIKELY(overread_count > (bitsleft >> 3))) return false;
        out_len = static_cast<std::size_t>(out - dst);
        return true;
    }

private:
    // 16-byte struct returned in two registers (%rax:%rdx on x86-64, x0:x1 on ARM64).
    struct DynHeaderResult {
        std::uint64_t bitbuf;
        std::uint64_t packed;  // [0:31]=in_adv, [32:39]=bitsleft, [40:47]=overread, [48:55]=ll_bits
    };

    FAST_RAW_DEFLATE_FORCE_INLINE static unsigned HighestBit(unsigned x) noexcept {
#if __has_cpp_attribute(assume)
        [[assume(x != 0U)]];
#endif
        return std::bit_floor(x);
    }

    FAST_RAW_DEFLATE_FORCE_INLINE static void RefillBranchless(
        const std::uint8_t*& in, std::uint64_t& bitbuf, std::uint32_t& bitsleft) noexcept {
        std::uint64_t w;
        std::memcpy(&w, in, sizeof(w));
        bitbuf |= w << static_cast<std::uint8_t>(bitsleft);
        in += 7U - ((bitsleft >> 3) & 7U);
        bitsleft |= 56U;
    }

    FAST_RAW_DEFLATE_FORCE_INLINE static bool RefillSafe(
        const std::uint8_t*& in, const std::uint8_t* in_end,
        std::uint64_t& bitbuf, std::uint32_t& bitsleft,
        std::size_t& overread_count) noexcept {
        if (FAST_RAW_DEFLATE_LIKELY(in_end - in >= 8)) {
            RefillBranchless(in, bitbuf, bitsleft);
            return true;
        }
        while (static_cast<std::uint8_t>(bitsleft) < 56U) {
            if (FAST_RAW_DEFLATE_LIKELY(in < in_end)) {
                bitbuf |= static_cast<std::uint64_t>(*in++) << static_cast<std::uint8_t>(bitsleft);
            } else {
                if (FAST_RAW_DEFLATE_UNLIKELY(++overread_count > 8U)) return false;
            }
            bitsleft += 8U;
        }
        return true;
    }

    FAST_RAW_DEFLATE_FORCE_INLINE static std::uint64_t ExtractVarBits(
        std::uint64_t word, unsigned count) noexcept {
#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
        return _bzhi_u64(word, count);
#else
        return word & ((std::uint64_t{1} << (count & 63U)) - 1ULL);
#endif
    }

    FAST_RAW_DEFLATE_FORCE_INLINE static std::uint64_t ExtractVarBits8(
        std::uint64_t word, std::uint32_t entry) noexcept {
#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
        return _bzhi_u64(word, entry);
#else
        return word & ((std::uint64_t{1} << (entry & 63U)) - 1ULL);
#endif
    }

    FAST_RAW_DEFLATE_FORCE_INLINE static void Copy8(char* dst, const char* src) noexcept {
        std::uint64_t v;
        std::memcpy(&v, src, 8);
        std::memcpy(dst, &v, 8);
    }

    FAST_RAW_DEFLATE_FORCE_INLINE static void CopyMatchFast(
        char* dst, const char* src, const char* out_next, std::size_t dist) noexcept {
        if (FAST_RAW_DEFLATE_LIKELY(dist >= 8)) {
            Copy8(dst, src);
            Copy8(dst + 8, src + 8);
            dst += 16;
            if (FAST_RAW_DEFLATE_UNLIKELY(dst < out_next)) {
                src += 16;
                FAST_RAW_DEFLATE_NOVECTOR
                do {
                    Copy8(dst, src);
                    Copy8(dst + 8, src + 8);
                    dst += 16;
                    src += 16;
                } while (dst < out_next);
            }
        } else if (dist == 1) {
            const std::uint64_t v =
                static_cast<std::uint64_t>(static_cast<std::uint8_t>(src[0])) *
                0x0101010101010101ULL;
            FAST_RAW_DEFLATE_NOVECTOR
            do {
                std::memcpy(dst, &v, 8);
                std::memcpy(dst + 8, &v, 8);
                dst += 16;
            } while (dst < out_next);
        } else {
            FAST_RAW_DEFLATE_NOVECTOR
            do {
                Copy8(dst, src);
                dst += dist;
                src += dist;
            } while (dst < out_next);
        }
    }

    FAST_RAW_DEFLATE_FORCE_INLINE static void CopyMatchTail(
        char* dst, const char* src, const char* out_next, std::size_t dist) noexcept {
        if (FAST_RAW_DEFLATE_LIKELY(dist >= 8)) {
            Copy8(dst, src);
            dst += 8;
            if (FAST_RAW_DEFLATE_UNLIKELY(dst < out_next)) {
                src += 8;
                FAST_RAW_DEFLATE_NOVECTOR
                do {
                    Copy8(dst, src);
                    dst += 8;
                    src += 8;
                } while (dst < out_next);
            }
        } else {
            FAST_RAW_DEFLATE_NOVECTOR
            do {
                *dst++ = *src++;
            } while (dst < out_next);
        }
    }

    FAST_RAW_DEFLATE_FORCE_INLINE static void DoubleTable(
        std::uint32_t* table, unsigned cur_end) noexcept {
        if (cur_end < 8U) {
            if (cur_end == 4U) {
                std::memcpy(table + 4, table, 16);
            } else if (cur_end == 2U) {
                std::memcpy(table + 2, table, 8);
            } else {
                table[1] = table[0];
            }
            return;
        }
        const char* s = reinterpret_cast<const char*>(table);
        char* d = reinterpret_cast<char*>(table + cur_end);
        const char* const s_end = d;
        FAST_RAW_DEFLATE_NOVECTOR
        do {
            std::uint64_t w0, w1, w2, w3;
            std::memcpy(&w0, s, 8);
            std::memcpy(&w1, s + 8, 8);
            std::memcpy(&w2, s + 16, 8);
            std::memcpy(&w3, s + 24, 8);
            std::memcpy(d, &w0, 8);
            std::memcpy(d + 8, &w1, 8);
            std::memcpy(d + 16, &w2, 8);
            std::memcpy(d + 24, &w3, 8);
            s += 32;
            d += 32;
#if defined(__GNUC__) || defined(__clang__)
            __asm__ volatile("" : "+r"(s));
#endif
        } while (s < s_end);
    }

    // Pre-baked precode results:
    // - sym 1..15 (active codeword length): (sym << 16)
    // - sym 0, 17, 18 (zero-length run):    kLiteralFlag | (base_rep << 16) | extra_bits
    // - sym 16 (repeat prev 3..6):          kExceptional | kSubtablePtr | 2
    static constexpr auto kPrecodeResults = []() constexpr {
        std::array<std::uint32_t, 19> r{};
        r[0] = kLiteralFlag | (1U << 16);
        for (unsigned s = 1; s <= 15; ++s) {
            r[s] = s << 16;
        }
        r[16] = kExceptional | kSubtablePtr | 2U;
        r[17] = kLiteralFlag | (3U << 16) | 3U;
        r[18] = kLiteralFlag | (11U << 16) | 7U;
        return r;
    }();

    FAST_RAW_DEFLATE_NOINLINE FAST_RAW_DEFLATE_HOT DynHeaderResult DecodeDynamicTrees(
        const std::uint8_t* const in_start, const std::uint8_t* in_end,
        std::uint64_t bitbuf, std::uint32_t bitsleft,
        std::size_t overread_count) noexcept {
        const std::uint8_t* in = in_start;
        constexpr DynHeaderResult bad{0, 0};

        const unsigned hlit = 257U + static_cast<unsigned>((bitbuf >> 3) & 31U);
        const unsigned hdist = 1U + static_cast<unsigned>((bitbuf >> 8) & 31U);
        const unsigned hclen = 4U + static_cast<unsigned>((bitbuf >> 13) & 15U);
        if (FAST_RAW_DEFLATE_UNLIKELY(hlit > 286U || hdist > 30U)) return bad;

        static constexpr std::uint8_t kClOrder[19] = {
            16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        alignas(16) std::uint8_t cl_lens[24]{};
        cl_lens[kClOrder[0]] = static_cast<std::uint8_t>((bitbuf >> 17) & 7U);
        bitbuf >>= 20;
        bitsleft -= 20U;
        if (FAST_RAW_DEFLATE_UNLIKELY(!RefillSafe(in, in_end, bitbuf, bitsleft, overread_count))) {
            return bad;
        }

        FAST_RAW_DEFLATE_NOVECTOR
        for (unsigned i = 1; i < hclen; ++i) {
            cl_lens[kClOrder[i]] = static_cast<std::uint8_t>(bitbuf & 7U);
            bitbuf >>= 3;
            bitsleft -= 3U;
        }

        alignas(32) std::uint32_t cl_tab[kPrecodeTableSize] FAST_RAW_DEFLATE_INDETERMINATE;
        if (FAST_RAW_DEFLATE_UNLIKELY(!BuildPrecodeTable(cl_lens, cl_tab))) return bad;

        alignas(16) std::uint16_t ll_count[16]{};
        alignas(16) std::uint16_t d_count[16]{};
        std::uint16_t ll_active[288] FAST_RAW_DEFLATE_INDETERMINATE;
        std::uint16_t d_active[32] FAST_RAW_DEFLATE_INDETERMINATE;
        unsigned n_ll = 0;
        unsigned n_d = 0;
        unsigned prev_len = 0;

        const unsigned total_codes = hlit + hdist;
        unsigned idx = 0;

        auto step_rle = [&]
#if defined(__GNUC__) || defined(__clang__)
            [[gnu::always_inline]]
#endif
            () noexcept -> bool {
            const std::uint32_t e = cl_tab[bitbuf & 127U];
            const std::uint64_t saved_bb = bitbuf;
            bitbuf >>= static_cast<std::uint8_t>(e);
            bitsleft -= e;

            if (static_cast<std::int32_t>(e) < 0) {
                prev_len = 0;
                idx += static_cast<std::uint8_t>(e >> 16) +
                       static_cast<unsigned>(ExtractVarBits8(saved_bb, e) >>
                                             static_cast<std::uint8_t>(e >> 8));
                return true;
            }
            if (FAST_RAW_DEFLATE_LIKELY((e & kExceptional) == 0)) {
                const unsigned l = e >> 16;
                prev_len = l;
                if (FAST_RAW_DEFLATE_LIKELY(idx < hlit)) {
                    ++ll_count[l];
                    ll_active[n_ll++] = static_cast<std::uint16_t>((l << 9) | idx);
                } else {
                    ++d_count[l];
                    d_active[n_d++] = static_cast<std::uint16_t>((l << 9) | (idx - hlit));
                }
                ++idx;
                return true;
            }
            if (FAST_RAW_DEFLATE_UNLIKELY((e & kSubtablePtr) == 0 || idx == 0U)) return false;
            const unsigned rep =
                3U + static_cast<unsigned>(ExtractVarBits8(saved_bb, e) >>
                                           ((e >> 8) & 0xFU));
            if (FAST_RAW_DEFLATE_UNLIKELY(idx + rep > total_codes)) return false;
            if (prev_len != 0U) {
                const unsigned end_idx = idx + rep;
                const unsigned hi = prev_len << 9;
                if (FAST_RAW_DEFLATE_LIKELY(end_idx <= hlit)) {
                    ll_count[prev_len] = static_cast<std::uint16_t>(ll_count[prev_len] + rep);
                    for (unsigned s = idx; s < end_idx; ++s) {
                        ll_active[n_ll++] = static_cast<std::uint16_t>(hi | s);
                    }
                } else {
                    for (unsigned s = idx; s < end_idx; ++s) {
                        if (s < hlit) {
                            ++ll_count[prev_len];
                            ll_active[n_ll++] = static_cast<std::uint16_t>(hi | s);
                        } else {
                            ++d_count[prev_len];
                            d_active[n_d++] = static_cast<std::uint16_t>(hi | (s - hlit));
                        }
                    }
                }
            }
            idx += rep;
            return true;
        };

        while (idx < total_codes && in_end - in >= 8) {
            if (static_cast<std::uint8_t>(bitsleft) < 14U) {
                RefillBranchless(in, bitbuf, bitsleft);
            }
            if (FAST_RAW_DEFLATE_UNLIKELY(!step_rle())) return bad;
        }
        while (idx < total_codes) {
            if (static_cast<std::uint8_t>(bitsleft) < 14U) {
                if (FAST_RAW_DEFLATE_UNLIKELY(
                        !RefillSafe(in, in_end, bitbuf, bitsleft, overread_count))) {
                    return bad;
                }
            }
            if (FAST_RAW_DEFLATE_UNLIKELY(!step_rle())) return bad;
        }

        if (FAST_RAW_DEFLATE_UNLIKELY(idx != total_codes)) return bad;

        unsigned ll_bits = kLitLenMaxBits;
        if (FAST_RAW_DEFLATE_UNLIKELY(!BuildFromActive<true>(
                dyn_litlen_.data(), kLitLenTableCap, ll_count, ll_active, n_ll,
                kLitLenResults.data(), kLitLenMaxBits, &ll_bits))) {
            return bad;
        }
        if (FAST_RAW_DEFLATE_UNLIKELY(!BuildFromActive<false>(
                dyn_dist_.data(), kDistTableCap, d_count, d_active, n_d,
                kDistResults.data(), kDistBits, nullptr))) {
            return bad;
        }

        const std::uint64_t adv = static_cast<std::uint32_t>(in - in_start);
        const std::uint64_t packed =
            adv |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(bitsleft)) << 32) |
            (static_cast<std::uint64_t>(overread_count) << 40) |
            (static_cast<std::uint64_t>(ll_bits) << 48);
        return DynHeaderResult{bitbuf, packed};
    }

    static constexpr auto kLitLenResults = []() constexpr {
        std::array<std::uint32_t, 288> r{};
        for (unsigned i = 0; i < 256; ++i) {
            r[i] = kLiteralFlag | (i << 16);
        }
        r[256] = kExceptional | kEndOfBlock;
        constexpr std::uint16_t kBase[29] = {
            3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
            35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr std::uint8_t kExtra[29] = {
            0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
            3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        for (unsigned i = 0; i < 29; ++i) {
            r[257 + i] = (static_cast<std::uint32_t>(kBase[i]) << 16) | kExtra[i];
        }
        r[286] = kExceptional;  // RFC 1951 §3.2.6 invalid symbol
        r[287] = kExceptional;  // RFC 1951 §3.2.6 invalid symbol
        return r;
    }();

    static constexpr auto kDistResults = []() constexpr {
        std::array<std::uint32_t, 32> r{};
        constexpr std::uint16_t kBase[30] = {
            1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
            257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
            8193, 12289, 16385, 24577};
        constexpr std::uint8_t kExtra[30] = {
            0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
            7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (unsigned i = 0; i < 30; ++i) {
            r[i] = (static_cast<std::uint32_t>(kBase[i]) << 16) | kExtra[i];
        }
        r[30] = kExceptional;  // RFC 1951 §3.2.6 invalid distance symbol
        r[31] = kExceptional;  // RFC 1951 §3.2.6 invalid distance symbol
        return r;
    }();

    FAST_RAW_DEFLATE_FORCE_INLINE static bool BuildPrecodeTable(
        const std::uint8_t* lens, std::uint32_t* table) noexcept {
        std::uint16_t count[8]{};
        unsigned n_active = 0;
        std::uint16_t active[19] FAST_RAW_DEFLATE_INDETERMINATE;
        FAST_RAW_DEFLATE_NOVECTOR
        for (unsigned sym = 0; sym < 19; ++sym) {
            const unsigned l = lens[sym];
            if (l != 0) {
                ++count[l];
                active[n_active++] = static_cast<std::uint16_t>((l << 5) | sym);
            }
        }
        unsigned max_len = 7;
        while (max_len > 0 && count[max_len] == 0) --max_len;
        if (FAST_RAW_DEFLATE_UNLIKELY(max_len == 0)) return false;

        std::uint16_t offs[8] FAST_RAW_DEFLATE_INDETERMINATE;
        std::uint16_t cur = 0;
        unsigned space = 0;
        FAST_RAW_DEFLATE_NOVECTOR
        for (unsigned l = 1; l <= max_len; ++l) {
            offs[l] = cur;
            cur = static_cast<std::uint16_t>(cur + count[l]);
            space = (space << 1) + count[l];
        }
        if (FAST_RAW_DEFLATE_UNLIKELY(space != (1U << max_len))) {
            if (space > (1U << max_len) || n_active != 1 || max_len != 1) return false;
            const unsigned sym = active[0] & 31U;
            table[0] = kPrecodeResults[sym] + 0x0101U;
            table[1] = kExceptional | 0x0101U;
            for (unsigned sz = 2; sz < kPrecodeTableSize; sz <<= 1) {
                DoubleTable(table, sz);
            }
            return true;
        }

        std::uint8_t sorted[19] FAST_RAW_DEFLATE_INDETERMINATE;
        FAST_RAW_DEFLATE_NOVECTOR
        for (unsigned i = 0; i < n_active; ++i) {
            const unsigned p = active[i];
            sorted[offs[p >> 5]++] = static_cast<std::uint8_t>(p & 31U);
        }

        unsigned codeword = 0;
        unsigned len = 1;
        while (count[len] == 0) ++len;
        unsigned cur_end = 1U << len;
        unsigned s_idx = 0;
        for (;;) {
            unsigned rem = count[len];
            const std::uint32_t len_add = (len << 8) + len;
            do {
                table[codeword] = kPrecodeResults[sorted[s_idx++]] + len_add;
                if (codeword == cur_end - 1U) {
                    for (; len < kPrecodeBits; ++len) {
                        DoubleTable(table, cur_end);
                        cur_end <<= 1;
                    }
                    return true;
                }
                const unsigned bit = HighestBit(codeword ^ (cur_end - 1U));
                codeword = (codeword & (bit - 1U)) | bit;
            } while (--rem != 0);

            do {
                ++len;
                DoubleTable(table, cur_end);
                cur_end <<= 1;
            } while (count[len] == 0);
        }
    }

    template <bool RequireEob>
    FAST_RAW_DEFLATE_FORCE_INLINE static bool BuildFromActive(
        std::uint32_t* table, std::size_t table_cap,
        const std::uint16_t* count, const std::uint16_t* active, unsigned n_active,
        const std::uint32_t* base_results,
        unsigned table_bits, unsigned* table_bits_out) noexcept {
        unsigned max_len = 15;
        while (max_len > 1 && count[max_len] == 0) --max_len;
        if constexpr (RequireEob) {
            table_bits = std::min(table_bits, max_len);
            *table_bits_out = table_bits;
        }

        std::uint16_t offs[16] FAST_RAW_DEFLATE_INDETERMINATE;
        std::uint16_t cur = 0;
        std::uint32_t space = 0;
        FAST_RAW_DEFLATE_NOVECTOR
        for (unsigned l = 1; l <= max_len; ++l) {
            offs[l] = cur;
            cur = static_cast<std::uint16_t>(cur + count[l]);
            space = (space << 1) + count[l];
        }

        if (FAST_RAW_DEFLATE_UNLIKELY(space != (1U << max_len))) {
            if (space > (1U << max_len)) return false;
            if (space == 0) {
                if constexpr (RequireEob) return false;
                table[0] = kExceptional | 0x0101U;
                for (unsigned sz = 1, end = (1U << table_bits); sz < end; sz <<= 1) {
                    DoubleTable(table, sz);
                }
                return true;
            }
            if (n_active != 1 || max_len != 1) return false;
            const unsigned s0 = active[0] & 0x1FFU;
            if constexpr (RequireEob) {
                if (s0 != 256U) return false;
            }
            table[0] = base_results[s0] + 0x0101U;
            table[1] = kExceptional | 0x0101U;
            for (unsigned sz = 2, end = (1U << table_bits); sz < end; sz <<= 1) {
                DoubleTable(table, sz);
            }
            return true;
        }

        constexpr std::size_t kMaxSyms = RequireEob ? 288 : 32;
        std::uint16_t sorted[kMaxSyms] FAST_RAW_DEFLATE_INDETERMINATE;
        FAST_RAW_DEFLATE_NOVECTOR
        for (unsigned i = 0; i < n_active; ++i) {
            const unsigned p = active[i];
            sorted[offs[p >> 9]++] = static_cast<std::uint16_t>(p & 0x1FFU);
        }

        std::uint32_t eob_or = 0;
        unsigned codeword = 0;
        unsigned len = 1;
        while (count[len] == 0) ++len;
        unsigned cur_end = 1U << len;
        unsigned s_idx = 0;

        while (len <= table_bits) {
            unsigned rem = count[len];
            const std::uint32_t len_add = (len << 8) + len;
            do {
                const std::uint32_t base = base_results[sorted[s_idx++]];
                if constexpr (RequireEob) eob_or |= base;
                table[codeword] = base + len_add;
                if (codeword == cur_end - 1U) {
                    if constexpr (RequireEob) {
                        if (FAST_RAW_DEFLATE_UNLIKELY((eob_or & kEndOfBlock) == 0)) return false;
                    }
                    for (; len < table_bits; ++len) {
                        DoubleTable(table, cur_end);
                        cur_end <<= 1;
                    }
                    return true;
                }
                const unsigned bit = HighestBit(codeword ^ (cur_end - 1U));
                codeword = (codeword & (bit - 1U)) | bit;
            } while (--rem != 0);

            do {
                if (++len <= table_bits) {
                    DoubleTable(table, cur_end);
                    cur_end <<= 1;
                }
            } while (count[len] == 0);
        }

        cur_end = 1U << table_bits;
        unsigned sub_prefix = ~0U;
        unsigned sub_start = 0;
        unsigned rem = count[len];
        for (;;) {
            if ((codeword & ((1U << table_bits) - 1U)) != sub_prefix) {
                sub_prefix = codeword & ((1U << table_bits) - 1U);
                sub_start = cur_end;
                unsigned sub_bits = len - table_bits;
                unsigned sub_space = rem;
                while (sub_space < (1U << sub_bits)) {
                    ++sub_bits;
                    sub_space = (sub_space << 1) + count[table_bits + sub_bits];
                }
                cur_end = sub_start + (1U << sub_bits);
                if (FAST_RAW_DEFLATE_UNLIKELY(cur_end > table_cap)) return false;
                table[sub_prefix] = (static_cast<std::uint32_t>(sub_start) << 16) |
                                    kExceptional | kSubtablePtr |
                                    (sub_bits << 8) | table_bits;
            }

            const unsigned extra_len = len - table_bits;
            const std::uint32_t base = base_results[sorted[s_idx++]];
            if constexpr (RequireEob) eob_or |= base;
            const std::uint32_t entry = base + (extra_len << 8) + extra_len;
            unsigned i = sub_start + (codeword >> table_bits);
            const unsigned stride = 1U << extra_len;
            FAST_RAW_DEFLATE_NOVECTOR
            do {
                table[i] = entry;
                i += stride;
            } while (i < cur_end);

            if (codeword == (1U << len) - 1U) {
                if constexpr (RequireEob) {
                    if (FAST_RAW_DEFLATE_UNLIKELY((eob_or & kEndOfBlock) == 0)) return false;
                }
                return true;
            }
            const unsigned bit = HighestBit(codeword ^ ((1U << len) - 1U));
            codeword = (codeword & (bit - 1U)) | bit;
            if (--rem == 0) {
                do { ++len; } while (count[len] == 0);
                rem = count[len];
            }
        }
    }

    static constexpr auto kFixedLitLenTable = []() constexpr {
        auto rev_bits = [](unsigned c, unsigned len) constexpr {
            unsigned r = 0;
            for (unsigned i = 0; i < len; ++i) {
                r = (r << 1) | (c & 1U);
                c >>= 1;
            }
            return r;
        };
        std::array<std::uint32_t, 512> tab{};
        for (unsigned sym = 256; sym <= 279; ++sym) {
            const unsigned rev = rev_bits(sym - 256, 7);
            const std::uint32_t e = kLitLenResults[sym] + (7U << 8) + 7U;
            for (unsigned idx = rev; idx < 512; idx += 128) tab[idx] = e;
        }
        for (unsigned sym = 0; sym <= 143; ++sym) {
            const unsigned rev = rev_bits(48 + sym, 8);
            const std::uint32_t e = kLitLenResults[sym] + (8U << 8) + 8U;
            tab[rev] = e;
            tab[rev + 256] = e;
        }
        for (unsigned sym = 280; sym <= 287; ++sym) {
            const unsigned rev = rev_bits(192 + (sym - 280), 8);
            const std::uint32_t e = kLitLenResults[sym] + (8U << 8) + 8U;
            tab[rev] = e;
            tab[rev + 256] = e;
        }
        for (unsigned sym = 144; sym <= 255; ++sym) {
            const unsigned rev = rev_bits(400 + (sym - 144), 9);
            tab[rev] = kLitLenResults[sym] + (9U << 8) + 9U;
        }
        return tab;
    }();

    static constexpr auto kFixedDistTable = []() constexpr {
        auto rev_bits = [](unsigned c, unsigned len) constexpr {
            unsigned r = 0;
            for (unsigned i = 0; i < len; ++i) {
                r = (r << 1) | (c & 1U);
                c >>= 1;
            }
            return r;
        };
        std::array<std::uint32_t, 128> tab{};
        for (unsigned sym = 0; sym < 32; ++sym) {
            const unsigned rev = rev_bits(sym, 5);
            const std::uint32_t e = kDistResults[sym] + (5U << 8) + 5U;
            for (unsigned idx = rev; idx < 128; idx += 32) tab[idx] = e;
        }
        return tab;
    }();

    alignas(32) std::array<std::uint32_t, kLitLenTableCap> dyn_litlen_{};
    alignas(32) std::array<std::uint32_t, kDistTableCap> dyn_dist_{};
};

}  // namespace fast_raw_deflate

#undef FAST_RAW_DEFLATE_INDETERMINATE
#undef FAST_RAW_DEFLATE_NOVECTOR
#undef FAST_RAW_DEFLATE_RESTRICT
#undef FAST_RAW_DEFLATE_UNLIKELY
#undef FAST_RAW_DEFLATE_LIKELY
#undef FAST_RAW_DEFLATE_HOT
#undef FAST_RAW_DEFLATE_NOINLINE
#undef FAST_RAW_DEFLATE_FORCE_INLINE

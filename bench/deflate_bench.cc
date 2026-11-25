// SPDX-License-Identifier: MIT
#include "fast_raw_deflate/raw_deflate.h"

#include <zlib.h>
#ifdef FAST_RAW_DEFLATE_HAS_LIBDEFLATE
#include <libdeflate.h>
#endif

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Reference v0 baseline (16-bit two-level strided table + scalar bitbuf loop)
// kept in benchmark binary to reproduce exact before-vs-after speedup numbers.
class FastRawDeflateBaselineV0 {
public:
    static constexpr unsigned kLitLenPrimaryBits = 8;
    static constexpr unsigned kLitLenPrimarySize = 1U << kLitLenPrimaryBits;
    static constexpr unsigned kLitLenPrimaryMask = kLitLenPrimarySize - 1U;
    static constexpr unsigned kDistPrimaryBits = 6;
    static constexpr unsigned kDistPrimarySize = 1U << kDistPrimaryBits;
    static constexpr unsigned kDistPrimaryMask = kDistPrimarySize - 1U;
    static constexpr std::size_t kLitLenTableCap = 1024;
    static constexpr std::size_t kDistTableCap = 512;
    static constexpr std::uint16_t kSubtableFlag = 0x8000U;

    FastRawDeflateBaselineV0() noexcept { BuildFixedTables(); }

    bool Inflate(const std::uint8_t* src, std::size_t src_len,
                 char* dst, std::size_t dst_cap, std::size_t& out_len) noexcept {
        if (src_len == 0 || dst_cap == 0) return false;
        const std::uint8_t* in = src;
        const std::uint8_t* const in_end = src + src_len;
        char* out = dst;
        char* const out_end = dst + dst_cap;
        std::uint64_t bitbuf = 0;
        int bits = 0;
        auto refill = [&]() noexcept {
            if (in_end - in >= 8) {
                std::uint64_t w;
                std::memcpy(&w, in, sizeof(w));
                bitbuf |= w << bits;
                const int adv = (63 - bits) >> 3;
                in += adv;
                bits += adv << 3;
            } else {
                while (bits <= 56 && in < in_end) {
                    bitbuf |= static_cast<std::uint64_t>(*in++) << bits;
                    bits += 8;
                }
            }
        };
        auto consume = [&](int n) noexcept { bitbuf >>= n; bits -= n; };
        bool bfinal = false;
        while (!bfinal) {
            if (bits < 3) refill();
            if (bits < 3) return false;
            bfinal = (bitbuf & 1U) != 0;
            const unsigned btype = static_cast<unsigned>((bitbuf >> 1) & 3U);
            consume(3);
            const std::uint16_t* ll_tab = nullptr;
            const std::uint16_t* d_tab = nullptr;
            if (btype == 1) {
                ll_tab = fixed_litlen_.data();
                d_tab = fixed_dist_.data();
            } else if (btype == 2) {
                if (bits < 14) refill();
                if (bits < 14) return false;
                const unsigned hlit = static_cast<unsigned>(bitbuf & 31U) + 257U;
                const unsigned hdist = static_cast<unsigned>((bitbuf >> 5) & 31U) + 1U;
                const unsigned hclen = static_cast<unsigned>((bitbuf >> 10) & 15U) + 4U;
                consume(14);
                if (hlit > 286U || hdist > 30U) return false;
                static constexpr std::array<std::uint8_t, 19> kClOrder = {
                    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                std::array<std::uint8_t, 19> cl_lens{};
                for (unsigned i = 0; i < hclen; ++i) {
                    if (bits < 3) refill();
                    if (bits < 3) return false;
                    cl_lens[kClOrder[i]] = static_cast<std::uint8_t>(bitbuf & 7U);
                    consume(3);
                }
                std::uint16_t cl_tab[128];
                if (!BuildTable(cl_lens.data(), 19, 7, cl_tab, 128)) return false;
                const unsigned total_codes = hlit + hdist;
                std::uint8_t lens[320];
                unsigned idx = 0;
                while (idx < total_codes) {
                    if (bits < 14) refill();
                    const std::uint16_t e = cl_tab[bitbuf & 127U];
                    const int cl_bits = static_cast<int>(e & 0xFU);
                    if (cl_bits == 0 || bits < cl_bits) return false;
                    const unsigned sym = e >> 4;
                    consume(cl_bits);
                    if (sym < 16U) {
                        lens[idx++] = static_cast<std::uint8_t>(sym);
                    } else {
                        unsigned rep = 0;
                        std::uint8_t val = 0;
                        if (sym == 16U) {
                            if (idx == 0 || bits < 2) return false;
                            val = lens[idx - 1];
                            rep = 3U + static_cast<unsigned>(bitbuf & 3U);
                            consume(2);
                        } else if (sym == 17U) {
                            if (bits < 3) return false;
                            rep = 3U + static_cast<unsigned>(bitbuf & 7U);
                            consume(3);
                        } else {
                            if (bits < 7) return false;
                            rep = 11U + static_cast<unsigned>(bitbuf & 127U);
                            consume(7);
                        }
                        if (idx + rep > total_codes) return false;
                        std::memset(lens + idx, val, rep);
                        idx += rep;
                    }
                }
                if (lens[256] == 0) return false;
                if (!BuildTable(lens, hlit, kLitLenPrimaryBits, dyn_litlen_.data(), kLitLenTableCap))
                    return false;
                if (!BuildTable(lens + hlit, hdist, kDistPrimaryBits, dyn_dist_.data(), kDistTableCap))
                    return false;
                ll_tab = dyn_litlen_.data();
                d_tab = dyn_dist_.data();
            } else {
                return false;
            }
            static constexpr std::array<std::uint16_t, 29> kLenBase = {
                3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
            static constexpr std::array<std::uint8_t, 29> kLenExtra = {
                0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
            static constexpr std::array<std::uint16_t, 30> kDistBase = {
                1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                8193, 12289, 16385, 24577};
            static constexpr std::array<std::uint8_t, 30> kDistExtra = {
                0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
            while (true) {
                refill();
                std::uint16_t e = ll_tab[bitbuf & kLitLenPrimaryMask];
                if ((e & kSubtableFlag) != 0) {
                    const unsigned sub_bits = e & 0xFU, sub_off = (e >> 4) & 0x7FFU;
                    consume(kLitLenPrimaryBits);
                    e = ll_tab[sub_off + (bitbuf & ((1U << sub_bits) - 1U))];
                }
                const int ll_bits = static_cast<int>(e & 0xFU);
                if (ll_bits == 0 || bits < ll_bits) return false;
                unsigned sym = e >> 4;
                consume(ll_bits);
                if (sym < 256U) {
                    if (out >= out_end) return false;
                    *out++ = static_cast<char>(sym);
                    while (bits >= 15) {
                        const std::uint16_t e2 = ll_tab[bitbuf & kLitLenPrimaryMask];
                        if ((e2 & kSubtableFlag) != 0) break;
                        const unsigned sym2 = e2 >> 4;
                        if (sym2 >= 256U) break;
                        const int b2 = static_cast<int>(e2 & 0xFU);
                        if (b2 == 0 || out >= out_end) break;
                        consume(b2);
                        *out++ = static_cast<char>(sym2);
                    }
                    continue;
                }
                if (sym == 256U) break;
                sym -= 257U;
                if (sym >= 29U) return false;
                const int lextra = kLenExtra[sym];
                if (bits < lextra) return false;
                const std::size_t length =
                    kLenBase[sym] + static_cast<std::size_t>(bitbuf & ((1U << lextra) - 1U));
                consume(lextra);
                if (bits < 28) refill();
                std::uint16_t de = d_tab[bitbuf & kDistPrimaryMask];
                if ((de & kSubtableFlag) != 0) {
                    const unsigned sub_bits = de & 0xFU, sub_off = (de >> 4) & 0x7FFU;
                    consume(kDistPrimaryBits);
                    de = d_tab[sub_off + (bitbuf & ((1U << sub_bits) - 1U))];
                }
                const int d_bits = static_cast<int>(de & 0xFU);
                if (d_bits == 0 || bits < d_bits) return false;
                const unsigned dsym = de >> 4;
                consume(d_bits);
                if (dsym >= 30U) return false;
                const int dextra = kDistExtra[dsym];
                if (bits < dextra) return false;
                const std::size_t dist =
                    kDistBase[dsym] + static_cast<std::size_t>(bitbuf & ((1U << dextra) - 1U));
                consume(dextra);
                if (dist > static_cast<std::size_t>(out - dst) ||
                    length > static_cast<std::size_t>(out_end - out))
                    return false;
                const char* match = out - dist;
                if (dist >= 8) {
                    std::size_t rem = length;
                    char* dst_p = out;
                    const char* src_p = match;
                    do {
                        std::memcpy(dst_p, src_p, 8);
                        dst_p += 8;
                        src_p += 8;
                    } while (rem > 8 && (rem -= 8, true));
                    out += length;
                } else if (dist == 1) {
                    std::memset(out, *match, length);
                    out += length;
                } else {
                    for (std::size_t i = 0; i < length; ++i) out[i] = match[i];
                    out += length;
                }
            }
        }
        out_len = static_cast<std::size_t>(out - dst);
        return true;
    }

private:
    static constexpr auto kBitRev8 = []() constexpr {
        std::array<std::uint8_t, 256> t{};
        for (unsigned i = 0; i < 256; ++i) {
            unsigned r = 0, x = i;
            for (int b = 0; b < 8; ++b) {
                r = (r << 1) | (x & 1U);
                x >>= 1;
            }
            t[i] = static_cast<std::uint8_t>(r);
        }
        return t;
    }();
    static inline std::uint16_t ReverseBits(std::uint16_t code, unsigned len) noexcept {
        const std::uint16_t rev16 = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(kBitRev8[code & 0xFFU]) << 8) | kBitRev8[code >> 8]);
        return static_cast<std::uint16_t>(rev16 >> (16U - len));
    }
    static bool BuildTable(const std::uint8_t* lens, unsigned num_syms,
                           unsigned primary_bits, std::uint16_t* table,
                           std::size_t table_cap) noexcept {
        std::array<std::uint16_t, 16> count{};
        unsigned max_len = 0;
        for (unsigned i = 0; i < num_syms; ++i) {
            const unsigned l = lens[i];
            if (l != 0) {
                if (l > 15U) return false;
                ++count[l];
                if (l > max_len) max_len = l;
            }
        }
        const unsigned primary_size = 1U << primary_bits;
        if (max_len == 0) {
            std::memset(table, 0, primary_size * 2);
            return true;
        }
        std::array<std::uint16_t, 16> offs{}, next_code{};
        std::uint16_t total_active = 0;
        unsigned code = 0;
        for (unsigned bits = 1; bits <= max_len; ++bits) {
            offs[bits] = total_active;
            total_active = static_cast<std::uint16_t>(total_active + count[bits]);
            code = (code + count[bits - 1]) << 1;
            next_code[bits] = static_cast<std::uint16_t>(code);
        }
        if ((code + count[max_len]) > (1U << max_len)) return false;
        std::uint16_t sorted[288];
        for (unsigned sym = 0; sym < num_syms; ++sym) {
            const unsigned l = lens[sym];
            if (l != 0) sorted[offs[l]++] = static_cast<std::uint16_t>(sym);
        }
        const bool complete = (code + count[max_len]) == (1U << max_len);
        if (!complete) std::memset(table, 0, primary_size * 2);
        unsigned s_idx = 0;
        const unsigned direct_max = max_len < primary_bits ? max_len : primary_bits;
        for (unsigned l = 1; l <= direct_max; ++l) {
            const unsigned cnt = count[l];
            if (!cnt) continue;
            std::uint16_t c = next_code[l];
            const unsigned step = 1U << l;
            for (unsigned k = 0; k < cnt; ++k, ++c) {
                const unsigned sym = sorted[s_idx++];
                const std::uint16_t entry = static_cast<std::uint16_t>((sym << 4) | l);
                for (unsigned idx = ReverseBits(c, l); idx < primary_size; idx += step)
                    table[idx] = entry;
            }
        }
        if (max_len > primary_bits) {
            std::uint8_t sub_max_len[256]{};
            for (unsigned l = primary_bits + 1; l <= max_len; ++l) {
                std::uint16_t c = next_code[l];
                const unsigned extra = l - primary_bits;
                for (unsigned k = 0; k < count[l]; ++k, ++c) {
                    const unsigned p = ReverseBits(c, l) & (primary_size - 1U);
                    if (extra > sub_max_len[p]) sub_max_len[p] = static_cast<std::uint8_t>(extra);
                }
            }
            std::size_t next_sub = primary_size;
            for (unsigned p = 0; p < primary_size; ++p) {
                const unsigned sb = sub_max_len[p];
                if (!sb) continue;
                const std::size_t ss = std::size_t{1} << sb;
                if (next_sub + ss > table_cap) return false;
                if (!complete) std::memset(table + next_sub, 0, ss * 2);
                table[p] = static_cast<std::uint16_t>(kSubtableFlag | (next_sub << 4) | sb);
                next_sub += ss;
            }
            for (unsigned l = primary_bits + 1; l <= max_len; ++l) {
                std::uint16_t c = next_code[l];
                const unsigned extra = l - primary_bits, step = 1U << extra;
                for (unsigned k = 0; k < count[l]; ++k, ++c) {
                    const unsigned sym = sorted[s_idx++], rev = ReverseBits(c, l);
                    const std::uint16_t ptr = table[rev & (primary_size - 1U)];
                    const unsigned sb = ptr & 0xFU, so = (ptr >> 4) & 0x7FFU, ss = 1U << sb;
                    const std::uint16_t entry = static_cast<std::uint16_t>((sym << 4) | extra);
                    for (unsigned idx = (rev >> primary_bits); idx < ss; idx += step)
                        table[so + idx] = entry;
                }
            }
        }
        return true;
    }
    void BuildFixedTables() noexcept {
        std::array<std::uint8_t, 288> ll{};
        for (unsigned i = 0; i <= 143; ++i) ll[i] = 8;
        for (unsigned i = 144; i <= 255; ++i) ll[i] = 9;
        for (unsigned i = 256; i <= 279; ++i) ll[i] = 7;
        for (unsigned i = 280; i <= 287; ++i) ll[i] = 8;
        (void)BuildTable(ll.data(), 288, kLitLenPrimaryBits, fixed_litlen_.data(), fixed_litlen_.size());
        std::array<std::uint8_t, 32> dd{};
        dd.fill(5);
        (void)BuildTable(dd.data(), 32, kDistPrimaryBits, fixed_dist_.data(), fixed_dist_.size());
    }
    std::array<std::uint16_t, kLitLenTableCap> fixed_litlen_{}, dyn_litlen_{};
    std::array<std::uint16_t, kDistTableCap> fixed_dist_{}, dyn_dist_{};
};

std::vector<std::uint8_t> DecodeBase64(std::string_view in) {
    static constexpr const char* kChars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int tab[256];
    std::memset(tab, -1, sizeof(tab));
    for (int i = 0; i < 64; ++i) tab[static_cast<unsigned char>(kChars[i])] = i;
    std::vector<std::uint8_t> out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (tab[c] == -1) break;
        val = (val << 6) + tab[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<std::uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

struct BenchStats {
    double min_ns = 0.0;
    double median_ns = 0.0;
    double mean_ns = 0.0;
    double max_ns = 0.0;
};

template <typename Fn>
BenchStats RunBench(const char* impl_name, const char* case_id,
                    int rounds, int iters, std::ofstream* rounds_csv, Fn&& fn) {
    for (int i = 0; i < 20000; ++i) fn();
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(rounds));
    for (int r = 0; r < rounds; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) fn();
        const auto t1 = std::chrono::steady_clock::now();
        const double ns =
            std::chrono::duration<double, std::nano>(t1 - t0).count() /
            static_cast<double>(iters);
        samples.push_back(ns);
        if (rounds_csv && rounds_csv->is_open()) {
            *rounds_csv << case_id << "," << impl_name << "," << r << "," << ns << "\n";
        }
    }
    std::sort(samples.begin(), samples.end());
    BenchStats st{};
    st.min_ns = samples.front();
    st.median_ns = samples[samples.size() / 2];
    st.mean_ns = std::accumulate(samples.begin(), samples.end(), 0.0) /
                 static_cast<double>(samples.size());
    st.max_ns = samples.back();
    std::printf("  %-28s : median %7.2f ns (min %7.2f ns, mean %7.2f ns)\n",
                impl_name, st.median_ns, st.min_ns, st.mean_ns);
    return st;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    int iters = 100000;
    int rounds = 7;
    std::string summary_path;
    std::string rounds_path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--iters" && i + 1 < argc) iters = std::stoi(argv[++i]);
        else if (arg == "--rounds" && i + 1 < argc) rounds = std::stoi(argv[++i]);
        else if (arg == "--csv" && i + 1 < argc) summary_path = argv[++i];
        else if (arg == "--rounds-csv" && i + 1 < argc) rounds_path = argv[++i];
    }

    const std::string_view v1_b64 =
        "XZI9TgUxDAbv4jqybH/5sbeEK9A9bUVLgYDuae9OaLwJ5Wp2nImVJ73TQS9vr1Tohw4d0dpQMY/uhT7oeDye9Dl/"
        "aQE1Fir0Pb+ExWs1uspNdaHNW2xUalJjuEJXV5BUGe3fZFkn1+66u5Ku8UzfqIUnBZtsyRYjYWXT6r5EWbSkxrMY"
        "baP3hVS5Gnps2FKeNKqArrMsu7y7tLGje190GFK3uRAZ63DY3W3cLLajAUkX7D7WbGDtwoTbsRipdlY3m5s+z0Jf"
        "f+9AY17gPK9f";
    const std::string_view v2_b64 =
        "VdA7EgIhDAbgu6RmMoS8yJZ6BTuHytbCUbudvbvYwFIyH/nz2OEBG1xuV0jwhY08VJ1yCQlK8ITtft/h1b9olAjM"
        "kODTX4IiYQZHmupDGZ2k1kXLUEJ3DzhaGtFMdbJiZVc5VXOhwRmtqvCiMtTQsjMtOqMVtRAvtTyTidDM+Tw2s47o"
        "Unpn8T5Xawne/8v0DVo7fg==";

    const auto raw1 = DecodeBase64(v1_b64);
    const auto raw2 = DecodeBase64(v2_b64);

    // 40-level order-book diff (~316 B -> 1345 B)
    std::string big_json = R"({"c":"BTC","t":1795571028968,"l":[[)";
    for (int i = 0; i < 20; ++i) {
        if (i) big_json += ",";
        big_json += R"({"p":")" + std::to_string(96500 - i * 7) + R"(.50","s":")" +
                    std::to_string(i * 19 + 3) + R"(.1250"})";
    }
    big_json += R"(],[)";
    for (int i = 0; i < 20; ++i) {
        if (i) big_json += ",";
        big_json += R"({"p":")" + std::to_string(96510 + i * 7) + R"(.00","s":")" +
                    std::to_string(i * 23 + 1) + R"(.8750"})";
    }
    big_json += R"(]],"r":[[0,2,5,8,12,17,19],[1,4,9,14,18]]})";

    z_stream zc{};
    deflateInit2(&zc, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    std::vector<std::uint8_t> raw_big(4096);
    zc.next_in = reinterpret_cast<Bytef*>(big_json.data());
    zc.avail_in = static_cast<uInt>(big_json.size());
    zc.next_out = raw_big.data();
    zc.avail_out = static_cast<uInt>(raw_big.size());
    deflate(&zc, Z_FINISH);
    raw_big.resize(raw_big.size() - zc.avail_out);
    deflateEnd(&zc);

    FastRawDeflateBaselineV0 fast_v0;
    fast_raw_deflate::FastRawDeflate fast_opt;
    z_stream zs{};
    inflateInit2(&zs, -MAX_WBITS);
#ifdef FAST_RAW_DEFLATE_HAS_LIBDEFLATE
    libdeflate_decompressor* libdef = libdeflate_alloc_decompressor();
#endif

    std::vector<char> out(65536 + 64, '\0');
    std::size_t len1 = 0, len2 = 0, len_big = 0;
    (void)fast_opt.Inflate(raw1.data(), raw1.size(), out.data(), 65536, len1);
    const std::string plain1(out.data(), len1);
    (void)fast_opt.Inflate(raw2.data(), raw2.size(), out.data(), 65536, len2);
    (void)fast_opt.Inflate(raw_big.data(), raw_big.size(), out.data(), 65536, len_big);

    deflateInit2(&zc, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_FIXED);
    std::vector<std::uint8_t> raw_fixed(1024);
    zc.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(plain1.data()));
    zc.avail_in = static_cast<uInt>(plain1.size());
    zc.next_out = raw_fixed.data();
    zc.avail_out = static_cast<uInt>(raw_fixed.size());
    deflate(&zc, Z_FINISH);
    raw_fixed.resize(raw_fixed.size() - zc.avail_out);
    deflateEnd(&zc);

    std::ofstream summary_csv;
    if (!summary_path.empty()) {
        summary_csv.open(summary_path);
        summary_csv << "case_id,in_bytes,out_bytes,impl,median_ns,min_ns,mean_ns,max_ns,mib_per_s\n";
    }
    std::ofstream rounds_csv;
    if (!rounds_path.empty()) {
        rounds_csv.open(rounds_path);
        rounds_csv << "case_id,impl,round,ns_per_op\n";
    }

    struct Case {
        const char* id;
        const char* desc;
        const std::vector<std::uint8_t>* raw;
        std::size_t out_bytes;
    };
    const std::array<Case, 4> cases = {{
        {"mainnet_l2_v1_dyn", "Mainnet l2 diff v1 (Dynamic Huffman btype=2)", &raw1, len1},
        {"mainnet_l2_v2_dyn", "Mainnet l2 diff v2 (Dynamic Huffman btype=2)", &raw2, len2},
        {"book40_diff_dyn", "40-level l2 diff (Dynamic Huffman btype=2)", &raw_big, len_big},
        {"mainnet_l2_v1_fixed", "Mainnet v1 pure decode loop (Fixed Huffman btype=1)", &raw_fixed, len1},
    }};

    for (const auto& c : cases) {
        std::printf("\n=== %s [%zu B -> %zu B, %d iters x %d rounds] ===\n",
                    c.desc, c.raw->size(), c.out_bytes, iters, rounds);
        volatile const std::uint8_t* vs = c.raw->data();
        volatile std::size_t vl = c.raw->size();

        auto emit = [&](const char* impl, const BenchStats& st) {
            if (summary_csv.is_open()) {
                const double mib_s =
                    (static_cast<double>(c.out_bytes) * 1e9 / st.median_ns) / (1024.0 * 1024.0);
                summary_csv << c.id << "," << c.raw->size() << "," << c.out_bytes << ","
                            << impl << "," << st.median_ns << "," << st.min_ns << ","
                            << st.mean_ns << "," << st.max_ns << "," << mib_s << "\n";
            }
        };

        const auto st_zlib = RunBench("zlib", c.id, rounds, iters, &rounds_csv, [&]() {
            inflateReset2(&zs, -MAX_WBITS);
            zs.next_in = const_cast<Bytef*>(const_cast<std::uint8_t*>(vs));
            zs.avail_in = static_cast<uInt>(vl);
            zs.next_out = reinterpret_cast<Bytef*>(out.data());
            zs.avail_out = 65536;
            const int rc = inflate(&zs, Z_FINISH);
#if defined(__GNUC__) || defined(__clang__)
            __asm__ volatile("" : : "r"(rc), "m"(*out.data()) : "memory");
#else
            (void)rc;
#endif
        });
        emit("zlib", st_zlib);

        const auto st_v0 = RunBench("FastRawDeflate_v0", c.id, rounds, iters, &rounds_csv, [&]() {
            std::size_t olen = 0;
            const bool ok = fast_v0.Inflate(const_cast<const std::uint8_t*>(vs), vl,
                                            out.data(), 65536, olen);
#if defined(__GNUC__) || defined(__clang__)
            __asm__ volatile("" : : "r"(ok), "r"(olen), "m"(*out.data()) : "memory");
#else
            (void)ok;
#endif
        });
        emit("FastRawDeflate_v0", st_v0);

#ifdef FAST_RAW_DEFLATE_HAS_LIBDEFLATE
        const auto st_lib = RunBench("libdeflate_v1.22", c.id, rounds, iters, &rounds_csv, [&]() {
            std::size_t olen = 0;
            const auto rc = libdeflate_deflate_decompress(
                libdef, const_cast<const std::uint8_t*>(vs), vl, out.data(), 65536, &olen);
#if defined(__GNUC__) || defined(__clang__)
            __asm__ volatile("" : : "r"(rc), "r"(olen), "m"(*out.data()) : "memory");
#else
            (void)rc;
#endif
        });
        emit("libdeflate_v1.22", st_lib);
#endif

        const auto st_opt = RunBench("FastRawDeflate", c.id, rounds, iters, &rounds_csv, [&]() {
            std::size_t olen = 0;
            const bool ok = fast_opt.Inflate(const_cast<const std::uint8_t*>(vs), vl,
                                             out.data(), 65536, olen);
#if defined(__GNUC__) || defined(__clang__)
            __asm__ volatile("" : : "r"(ok), "r"(olen), "m"(*out.data()) : "memory");
#else
            (void)ok;
#endif
        });
        emit("FastRawDeflate", st_opt);
    }

#ifdef FAST_RAW_DEFLATE_HAS_LIBDEFLATE
    libdeflate_free_decompressor(libdef);
#endif
    inflateEnd(&zs);
    return 0;
}

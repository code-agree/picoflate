// SPDX-License-Identifier: MIT
#include "picoflate/picoflate.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            throw std::runtime_error(std::string("CHECK failed at line ") +    \
                                     std::to_string(__LINE__) + ": " #expr);   \
        }                                                                      \
    } while (false)

namespace {

using FastRawDeflate = picoflate::Inflater;

std::vector<std::uint8_t> CompressZlib(
    std::string_view data, int level, int strategy, int flush_chunk = 0) {
    z_stream zs{};
    CHECK(deflateInit2(&zs, level, Z_DEFLATED, -MAX_WBITS, 8, strategy) == Z_OK);
    std::vector<std::uint8_t> out(deflateBound(&zs, static_cast<uLong>(data.size())) + 256);
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());
    if (flush_chunk > 0 && data.size() > static_cast<std::size_t>(flush_chunk)) {
        std::size_t pos = 0;
        while (pos < data.size()) {
            const std::size_t n = std::min<std::size_t>(flush_chunk, data.size() - pos);
            zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data() + pos));
            zs.avail_in = static_cast<uInt>(n);
            const bool last = (pos + n == data.size());
            const int rc = deflate(&zs, last ? Z_FINISH : Z_SYNC_FLUSH);
            CHECK(rc == Z_OK || rc == Z_STREAM_END);
            pos += n;
        }
    } else {
        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        zs.avail_in = static_cast<uInt>(data.size());
        CHECK(deflate(&zs, Z_FINISH) == Z_STREAM_END);
    }
    out.resize(out.size() - zs.avail_out);
    deflateEnd(&zs);
    return out;
}

// Uses exact-size heap buffers so AddressSanitizer detects any 1-byte overread on
// `src` or any write past `dst_cap + FastRawDeflate::kRequiredOutputSlack` (16B).
void VerifyExactBounds(FastRawDeflate& fast, const std::vector<std::uint8_t>& comp,
                       std::string_view expected) {
    auto* src_exact =
        static_cast<std::uint8_t*>(std::malloc(std::max<std::size_t>(1, comp.size())));
    std::memcpy(src_exact, comp.data(), comp.size());

    // 1. Exact output capacity (+16 slack bytes)
    const std::size_t cap_exact = std::max<std::size_t>(1, expected.size());
    auto* dst_exact =
        static_cast<char*>(std::malloc(cap_exact + FastRawDeflate::kRequiredOutputSlack));
    std::memset(dst_exact, 0xA5, cap_exact + FastRawDeflate::kRequiredOutputSlack);
    std::size_t out_len = 0;
    CHECK(fast.Inflate(src_exact, comp.size(), dst_exact, cap_exact, out_len));
    CHECK(out_len == expected.size());
    CHECK(std::memcmp(dst_exact, expected.data(), out_len) == 0);

    // 2. Large output capacity (exercises FastLoop across the full payload)
    const std::size_t cap_large = expected.size() + 1024;
    auto* dst_large =
        static_cast<char*>(std::malloc(cap_large + FastRawDeflate::kRequiredOutputSlack));
    std::memset(dst_large, 0x5A, cap_large + FastRawDeflate::kRequiredOutputSlack);
    out_len = 0;
    CHECK(fast.Inflate(src_exact, comp.size(), dst_large, cap_large, out_len));
    CHECK(out_len == expected.size());
    CHECK(std::memcmp(dst_large, expected.data(), out_len) == 0);

    // 3. Insufficient output capacity (`expected.size() - 1`) must fail cleanly
    if (expected.size() >= 2) {
        const std::size_t cap_short = expected.size() - 1;
        auto* dst_short =
            static_cast<char*>(std::malloc(cap_short + FastRawDeflate::kRequiredOutputSlack));
        std::size_t dummy = 0;
        CHECK(!fast.Inflate(src_exact, comp.size(), dst_short, cap_short, dummy));
        std::free(dst_short);
    }

    // 4. Every strict prefix truncation `0 .. comp.size() - 1` must be memory-safe
    for (std::size_t trunc = 0; trunc < comp.size(); ++trunc) {
        auto* s_trunc =
            static_cast<std::uint8_t*>(std::malloc(std::max<std::size_t>(1, trunc)));
        if (trunc > 0) std::memcpy(s_trunc, comp.data(), trunc);
        auto* d_trunc =
            static_cast<char*>(std::malloc(cap_large + FastRawDeflate::kRequiredOutputSlack));
        std::size_t olen = 0;
        if (fast.Inflate(s_trunc, trunc, d_trunc, cap_large, olen)) {
            CHECK(olen == expected.size());
            CHECK(std::memcmp(d_trunc, expected.data(), olen) == 0);
        }
        std::free(d_trunc);
        std::free(s_trunc);
    }

    std::free(dst_large);
    std::free(dst_exact);
    std::free(src_exact);
}

void TestCapturedWsVectors(FastRawDeflate& fast) {
    // Representative stateless WebSocket JSON event (~320 B) and JSON-RPC response (~560 B)
    const std::string tiny_event =
        R"({"type":"node.metrics","seq":104829,"ts":1732512000412,"host":"edge-04.prod",)"
        R"("cpu":{"user":14.2,"sys":3.1,"idle":82.7},"net":{"rx_kbps":94210,"tx_kbps":88140},)"
        R"("peers":[{"id":"n-01","rtt_us":184,"ok":true},{"id":"n-02","rtt_us":219,"ok":true},)"
        R"({"id":"n-03","rtt_us":196,"ok":true}]})";
    const std::string small_rpc =
        R"({"jsonrpc":"2.0","id":98412,"result":{"cluster":"us-east-realtime","revision":491820,)"
        R"("endpoints":[{"node":"gw-01","zone":"az1","conns":1420,"p50_us":142,"p99_us":410,"healthy":true},)"
        R"({"node":"gw-02","zone":"az1","conns":1395,"p50_us":138,"p99_us":395,"healthy":true},)"
        R"({"node":"gw-03","zone":"az2","conns":1448,"p50_us":151,"p99_us":432,"healthy":true},)"
        R"({"node":"gw-04","zone":"az2","conns":1402,"p50_us":145,"p99_us":418,"healthy":true},)"
        R"({"node":"gw-05","zone":"az3","conns":1389,"p50_us":149,"p99_us":425,"healthy":true}]}})";

    for (std::string_view plain : {std::string_view(tiny_event), std::string_view(small_rpc)}) {
        const auto raw = CompressZlib(plain, Z_DEFAULT_COMPRESSION, Z_DEFAULT_STRATEGY, 0);
        VerifyExactBounds(fast, raw, plain);
    }
}

std::size_t TestRfc1951EdgeCases(FastRawDeflate& fast, std::vector<std::uint8_t>& out_fuzz_seed) {
    std::vector<std::string> corpora;
    corpora.emplace_back("A");
    corpora.emplace_back(std::string(520, 'X'));  // dist=1 RLE up to max match 258
    for (int period = 2; period <= 20; ++period) {
        std::string s;
        for (int i = 0; i < 600; ++i) s.push_back(static_cast<char>('a' + (i % period)));
        corpora.push_back(std::move(s));
    }

    // Fibonacci symbol frequencies -> forces 15-bit codeword lengths & subtables
    {
        std::string fib;
        unsigned f0 = 1, f1 = 1;
        for (int sym = 0; sym < 22; ++sym) {
            fib.append(f0, static_cast<char>(33 + sym));
            fib.push_back(' ');
            const unsigned fn = std::min(f0 + f1, 4000U);
            f0 = f1;
            f1 = fn;
        }
        std::mt19937 rng(42);
        std::string mixed;
        for (int i = 0; i < 200; ++i) {
            const std::size_t off = rng() % (fib.size() / 2);
            const std::size_t len = 3 + (rng() % 30);
            mixed.append(fib.data() + off, std::min(len, fib.size() - off));
        }
        corpora.push_back(std::move(fib));
        corpora.push_back(std::move(mixed));
    }

    // Incompressible random binary payloads (stored btype=0 & 0-distance dynamic blocks)
    {
        std::mt19937 rng(12345);
        for (std::size_t sz : {5, 17, 64, 255, 1024, 4096}) {
            std::string rnd(sz, '\0');
            for (char& c : rnd) c = static_cast<char>(rng() & 0xFF);
            corpora.push_back(std::move(rnd));
        }
    }

    // Structured nested JSON incremental payloads (1..40 items per group)
    for (int levels : {1, 2, 5, 12, 20, 40}) {
        std::string j = R"({"c":"app","t":1795571028968,"l":[[)";
        for (int i = 0; i < levels; ++i) {
            if (i) j += ",";
            j += R"({"p":")" + std::to_string(96500 - i * 3) + R"(.50","s":")" +
                 std::to_string(i * 17 + 1) + R"(.0025"})";
        }
        j += R"(],[)";
        for (int i = 0; i < levels; ++i) {
            if (i) j += ",";
            j += R"({"p":")" + std::to_string(96505 + i * 3) + R"(.00","s":")" +
                 std::to_string(i * 13 + 2) + R"(.5000"})";
        }
        j += R"(]],"r":[[0,1,4,9,15],[2,6,11,19]]})";
        corpora.push_back(std::move(j));
    }

    out_fuzz_seed = CompressZlib(corpora.back(), 6, Z_DEFAULT_STRATEGY, 0);

    std::size_t verified = 0;
    for (const auto& plain : corpora) {
        for (int level : {0, 1, 6, 9}) {
            for (int strat : {Z_DEFAULT_STRATEGY, Z_FIXED, Z_HUFFMAN_ONLY, Z_RLE}) {
                VerifyExactBounds(fast, CompressZlib(plain, level, strat, 0), plain);
                ++verified;
                if (plain.size() > 40) {
                    VerifyExactBounds(
                        fast,
                        CompressZlib(plain, level, strat, static_cast<int>(plain.size() / 3)),
                        plain);
                    ++verified;
                }
            }
        }
    }

    // Hand-crafted invalid RFC 1951 streams
    char buf[256 + 16]{};
    std::size_t olen = 0;
    CHECK(!fast.Inflate(nullptr, 0, buf, 256, olen));
    const std::uint8_t bad_btype3[] = {0x07, 0x00};
    CHECK(!fast.Inflate(bad_btype3, sizeof(bad_btype3), buf, 256, olen));
    const std::uint8_t bad_sym286[] = {0x1B, 0x03, 0x00, 0x00};
    CHECK(!fast.Inflate(bad_sym286, sizeof(bad_sym286), buf, 256, olen));
    const std::uint64_t bad_dsym30_bits =
        0x3ULL | (0x8EULL << 3) | (0x40ULL << 11) | (0x0FULL << 18);
    std::uint8_t bad_dsym30[8]{};
    std::memcpy(bad_dsym30, &bad_dsym30_bits, 8);
    CHECK(!fast.Inflate(bad_dsym30, sizeof(bad_dsym30), buf, 256, olen));
    const std::uint64_t bad_dist_oob_bits =
        0x3ULL | (0x8EULL << 3) | (0x40ULL << 11) | (0x10ULL << 18);
    std::uint8_t bad_dist_oob[8]{};
    std::memcpy(bad_dist_oob, &bad_dist_oob_bits, 8);
    CHECK(!fast.Inflate(bad_dist_oob, sizeof(bad_dist_oob), buf, 256, olen));

    return verified;
}

void TestDifferentialFuzzAgainstZlib(FastRawDeflate& fast,
                                     const std::vector<std::uint8_t>& base_comp,
                                     std::size_t fuzz_runs) {
    std::mt19937 rng(0xC0FFEE);
    for (std::size_t i = 0; i < fuzz_runs; ++i) {
        std::vector<std::uint8_t> mut;
        if (i % 3 == 0) {
            mut.resize(1 + (rng() % 256));
            for (auto& b : mut) b = static_cast<std::uint8_t>(rng() & 0xFF);
        } else {
            mut = base_comp;
            const int flips = 1 + static_cast<int>(rng() % 4);
            for (int f = 0; f < flips; ++f) {
                mut[rng() % mut.size()] ^= static_cast<std::uint8_t>(1U << (rng() % 8));
            }
        }
        auto* s = static_cast<std::uint8_t*>(std::malloc(mut.size()));
        std::memcpy(s, mut.data(), mut.size());
        constexpr std::size_t kCap = 8192;
        auto* d = static_cast<char*>(std::malloc(kCap + FastRawDeflate::kRequiredOutputSlack));
        std::size_t olen = 0;
        const bool ok = fast.Inflate(s, mut.size(), d, kCap, olen);
        if (ok) {
            z_stream zs{};
            CHECK(inflateInit2(&zs, -MAX_WBITS) == Z_OK);
            std::vector<char> dz(kCap + 16);
            zs.next_in = s;
            zs.avail_in = static_cast<uInt>(mut.size());
            zs.next_out = reinterpret_cast<Bytef*>(dz.data());
            zs.avail_out = static_cast<uInt>(kCap);
            const int zrc = inflate(&zs, Z_FINISH);
            const std::size_t oz = kCap - zs.avail_out;
            inflateEnd(&zs);
            if (zrc == Z_STREAM_END) {
                CHECK(olen == oz);
                CHECK(std::memcmp(d, dz.data(), olen) == 0);
            }
        }
        std::free(d);
        std::free(s);
    }
}

}  // namespace

int main() {
    FastRawDeflate fast;
    TestCapturedWsVectors(fast);
    std::vector<std::uint8_t> seed;
    const std::size_t structured = TestRfc1951EdgeCases(fast, seed);
    constexpr std::size_t kFuzzRuns = 25000;
    TestDifferentialFuzzAgainstZlib(fast, seed, kFuzzRuns);
    std::printf("picoflate_tests passed (%zu structured vectors + %zu fuzz cases)\n",
                structured + 2, kFuzzRuns);
    return 0;
}

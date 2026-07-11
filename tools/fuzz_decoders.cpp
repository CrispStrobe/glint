// Fuzz harness for the MP3 + AAC decoders (PLAN § D robustness). Feeds
// pure-random buffers, bit-flipped valid streams, and random truncations
// through each decoder; a real decoder must never crash, read/write out
// of bounds, or hang on malformed input. Build with
// -fsanitize=address,undefined to catch memory/UB errors; the built-in
// per-decode work is bounded so a hang shows up as a wall-clock timeout.
//
// usage: fuzz_decoders <valid.mp3> <valid.aac> [iters]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "aac_decoder.hpp"
#include "mp3_decoder.hpp"

static uint32_t g_rng = 0x9e3779b9u;
static uint32_t xr() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static std::vector<uint8_t> readf(const char* p) {
    FILE* f = std::fopen(p, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> b(static_cast<size_t>(n));
    if (std::fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
    std::fclose(f);
    return b;
}

template <class Dec, class Info,
          int (*info_fn)(const uint8_t*, int, Info*)>
long fuzz(const std::vector<uint8_t>& valid, int iters, int min_hdr) {
    std::vector<float> pcm(2 * 1152);
    long calls = 0;
    auto walk = [&](const std::vector<uint8_t>& buf) {
        Dec dec;
        dec.init();
        size_t off = 0;
        while (off + static_cast<size_t>(min_hdr) <= buf.size()) {
            Info fi;
            if (info_fn(buf.data() + off,
                        static_cast<int>(buf.size() - off), &fi) < 0) {
                off++;
                continue;
            }
            if (fi.frame_bytes <= 0 ||
                off + static_cast<size_t>(fi.frame_bytes) > buf.size())
                break;
            dec.decode_frame(buf.data() + off,
                             static_cast<int>(buf.size() - off),
                             pcm.data(), &fi);
            calls++;
            off += fi.frame_bytes;
        }
    };
    // 1) pure-random buffers.
    for (int it = 0; it < iters; it++) {
        int len = 8 + static_cast<int>(xr() % 2000);
        std::vector<uint8_t> buf(len);
        for (auto& b : buf) b = static_cast<uint8_t>(xr());
        walk(buf);
    }
    // 2) bit-flipped + truncated valid streams.
    if (!valid.empty()) {
        for (int it = 0; it < iters; it++) {
            std::vector<uint8_t> buf = valid;
            int nf = 1 + static_cast<int>(xr() % 8);
            for (int k = 0; k < nf; k++) {
                size_t p = xr() % buf.size();
                buf[p] ^= static_cast<uint8_t>(1u << (xr() & 7));
            }
            if (xr() & 1) buf.resize(1 + xr() % buf.size());
            walk(buf);
        }
    }
    return calls;
}

int main(int argc, char** argv) {
    std::vector<uint8_t> mp3 = argc > 1 ? readf(argv[1])
                                        : std::vector<uint8_t>();
    std::vector<uint8_t> aac = argc > 2 ? readf(argv[2])
                                        : std::vector<uint8_t>();
    int iters = argc > 3 ? std::atoi(argv[3]) : 3000;

    long m = fuzz<glint::mp3::Mp3Decoder, glint::mp3::Mp3FrameInfo,
                  glint::mp3::mp3_frame_info>(mp3, iters, 4);
    std::printf("mp3: %ld decode calls survived, no crash\n", m);
    long a = fuzz<glint::aac::AacDecoder, glint::aac::AacFrameInfo,
                  glint::aac::aac_frame_info>(aac, iters, 7);
    std::printf("aac: %ld decode calls survived, no crash\n", a);
    return 0;
}

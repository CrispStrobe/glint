// Memory-safety fuzz for the top-level Opus decoder (packet parse + mode
// orchestration + PLC/FEC + multistream). Random and bit-flipped/
// truncated valid packets must never crash or read/write out of bounds.
// Build with -fsanitize=address (the SILK layer has benign,
// reference-inherited signed-shift UBs on its bit-exact path, so this is
// ASan-only; UBSan is used for the MP3/AAC harness).
//
// usage: fuzz_opus_decoder <valid.bit> [iters]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "opus_decoder.hpp"

static uint32_t g = 0x243f6a88u;
static uint32_t xr() {
    g ^= g << 13;
    g ^= g >> 17;
    g ^= g << 5;
    return g;
}

int main(int argc, char** argv) {
    std::vector<std::vector<uint8_t>> valid;
    if (argc > 1) {
        FILE* f = std::fopen(argv[1], "rb");
        uint8_t h[8];
        while (f && std::fread(h, 1, 8, f) == 8) {
            uint32_t n = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 |
                         (uint32_t)h[2] << 8 | h[3];
            std::vector<uint8_t> p(n);
            if (n && std::fread(p.data(), 1, n, f) != n) break;
            valid.push_back(std::move(p));
        }
        if (f) std::fclose(f);
    }
    int iters = argc > 2 ? std::atoi(argv[2]) : 20000;
    std::vector<float> pcm(2 * 5760);
    long calls = 0;

    for (int ch = 1; ch <= 2; ch++) {
        for (int it = 0; it < iters; it++) {
            int len = 1 + static_cast<int>(xr() % 1200);
            std::vector<uint8_t> b(len);
            for (auto& x : b) x = static_cast<uint8_t>(xr());
            glint::opus::OpusDecoder d;
            d.init(ch);
            d.decode(b.data(), len, pcm.data(), 5760);
            d.decode_fec(b.data(), len, pcm.data(), 960);
            d.decode_fec(nullptr, 0, pcm.data(), 960);
            calls += 3;
        }
        if (!valid.empty()) {
            for (int it = 0; it < iters / 2; it++) {
                glint::opus::OpusDecoder d;
                d.init(ch);
                for (size_t k = 0; k < valid.size() && k < 20; k++) {
                    std::vector<uint8_t> b = valid[k];
                    if (!b.empty()) {
                        int nf = 1 + static_cast<int>(xr() % 4);
                        for (int j = 0; j < nf; j++)
                            b[xr() % b.size()] ^=
                                static_cast<uint8_t>(1u << (xr() & 7));
                        if (xr() % 3 == 0) b.resize(1 + xr() % b.size());
                    }
                    d.decode(b.data(), static_cast<int>(b.size()),
                             pcm.data(), 5760);
                    calls++;
                }
            }
        }
    }
    std::printf("opus: %ld decode calls survived, no crash\n", calls);
    return 0;
}

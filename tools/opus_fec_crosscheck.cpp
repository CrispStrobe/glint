// Cross-check driver for SILK in-band FEC decode (PLAN § O5). Compiled
// twice by tools/crosscheck_opus_fec.py: once against libopus's public
// opus_decode(..., decode_fec) and once against glint's
// OpusDecoder::decode_fec. A deterministic xorshift loss pattern is
// applied to a FEC-enabled .bit stream; lost frames are recovered from
// the NEXT packet's LBRR data (PLC when that one is lost too). SILK is
// exact integer, so int16 PCM and final ranges must be BYTE-IDENTICAL.
//
// usage: opus_fec_crosscheck <in.bit> <channels> <frame_size> [fs]
// frame_size and the output PCM are in fs-rate samples (default 48000).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef USE_LIBOPUS
extern "C" {
#include "opus.h"
}
struct Dec {
    OpusDecoder* d = nullptr;
    void init(int ch, int fs) {
        int err = 0;
        d = opus_decoder_create(fs, ch, &err);
    }
    int decode(const uint8_t* p, int len, int16_t* pcm, int frame,
               int fec) {
        return opus_decode(d, p, len, pcm, frame, fec);
    }
    uint32_t range() {
        opus_uint32 r = 0;
        opus_decoder_ctl(d, OPUS_GET_FINAL_RANGE(&r));
        return r;
    }
};
#else
#include "opus_decoder.hpp"
struct Dec {
    glint::opus::OpusDecoder d;
    float buf[2 * 5760];
    float clip_mem[2] = { 0, 0 };
    int ch = 1;
    void init(int channels, int fs) {
        ch = channels;
        d.init(channels, fs);
    }
    int decode(const uint8_t* p, int len, int16_t* pcm, int frame,
               int fec) {
        int n = fec || p == nullptr ? d.decode_fec(p, len, buf, frame)
                                    : d.decode(p, len, buf, 5760);
        if (n < 0) return n;
        // Reference int16 API semantics: soft clip, then FLOAT2INT16
        // (float multiply + lrintf — the float rounding must match).
        glint::opus::pcm_soft_clip(buf, n, ch, clip_mem);
        for (int i = 0; i < n * ch; i++) {
            float x = buf[i] * 32768.0f;
            x = x > 32767.f ? 32767.f : (x < -32768.f ? -32768.f : x);
            pcm[i] = static_cast<int16_t>(std::lrintf(x));
        }
        return n;
    }
    uint32_t range() { return d.final_range(); }
};
#endif

static uint32_t rng_state = 12345;
static uint32_t xrand() {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) return 2;
    FILE* in = std::fopen(argv[1], "rb");
    if (!in) return 2;
    int channels = std::atoi(argv[2]);
    int frame = std::atoi(argv[3]);
    int fs = argc == 5 ? std::atoi(argv[4]) : 48000;

    std::vector<std::vector<uint8_t>> pkts;
    uint8_t hdr[8];
    while (std::fread(hdr, 1, 8, in) == 8) {
        uint32_t len = be32(hdr);
        std::vector<uint8_t> p(len);
        if (len && std::fread(p.data(), 1, len, in) != len) return 3;
        pkts.push_back(std::move(p));
    }
    std::fclose(in);

    Dec dec;
    dec.init(channels, fs);
    std::vector<int16_t> pcm(2 * 5760);

    int npkt = static_cast<int>(pkts.size());
    std::vector<int> lost(npkt);
    for (int i = 0; i < npkt; i++) lost[i] = (xrand() % 100) < 20;
    lost[0] = 0;  // prime the decoder before the first loss

    for (int i = 0; i < npkt; i++) {
        int n;
        const char* kind;
        if (!lost[i]) {
            n = dec.decode(pkts[i].data(),
                           static_cast<int>(pkts[i].size()), pcm.data(),
                           frame, 0);
            kind = "norm";
        } else if (i + 1 < npkt && !lost[i + 1]) {
            // Recover from the next packet's LBRR copy.
            n = dec.decode(pkts[i + 1].data(),
                           static_cast<int>(pkts[i + 1].size()),
                           pcm.data(), frame, 1);
            kind = "fec";
        } else {
            n = dec.decode(nullptr, 0, pcm.data(), frame, 0);
            kind = "plc";
        }
        std::printf("%d %s n %d rng %08x pcm", i, kind, n,
                    n >= 0 ? dec.range() : 0);
        for (int k = 0; k < n * channels; k += 37)
            std::printf(" %d", pcm[k]);
        std::printf("\n");
    }
    return 0;
}

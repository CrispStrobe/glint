// Cross-check driver for the full CELT frame decoder (PLAN § O1).
// Compiled twice by tools/crosscheck_opus_celt_dec.py (reference vs glint).
//
// Fuzz oracle at the celt_decode level: sequences of random packets into a
// persistent decoder instance (state carryover: MDCT overlap, energy
// history, postfilter parameters, LCG seed), PCM compared per frame.

#include <cstdint>
#include <cstdio>

#ifdef USE_LIBOPUS
extern "C" {
#include "opus_custom.h"
#include "celt.h"
}
struct DecA {
    OpusCustomDecoder* st;
    void init(int channels) {
        int err = 0;
        OpusCustomMode* mode = opus_custom_mode_create(48000, 960, &err);
        st = opus_custom_decoder_create(mode, channels, &err);
        opus_custom_decoder_ctl(st, CELT_SET_SIGNALLING(0));
    }
    int frame(const uint8_t* data, int len, float* pcm, int fsize) {
        return opus_custom_decode_float(st, data, len, pcm, fsize);
    }
};
#else
#include "opus_celt_decoder.hpp"
struct DecA {
    glint::opus::CeltDecoder d;
    void init(int channels) { d.init(channels); }
    int frame(const uint8_t* data, int len, float* pcm, int fsize) {
        glint::opus::RangeDecoder dec;
        dec.init(data, static_cast<uint32_t>(len));
        return d.decode_frame(dec, static_cast<uint32_t>(len), pcm, fsize);
    }
};
#endif

static uint32_t rng_state;
static uint32_t xrand() {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

int main() {
    static uint8_t buf[512];
    static float pcm[2 * 960];
    for (uint32_t seed = 1; seed <= 60; seed++) {
        rng_state = seed;
        int channels = 1 + (int)(xrand() & 1);
        int lm = (int)(xrand() % 4);
        int fsize = 120 << lm;
        DecA dec;
        dec.init(channels);
        std::printf("seed %u ch %d lm %d\n", seed, channels, lm);
        for (int f = 0; f < 6; f++) {
            uint32_t len = 2 + xrand() % 300;
            for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)xrand();
            int ret = dec.frame(buf, (int)len, pcm, fsize);
            std::printf("frame %d len %u ret %d pcm", f, len, ret);
            for (int i = 0; i < channels * fsize; i += 13)
                std::printf(" %.5f", (double)pcm[i]);
            std::printf("\n");
        }
    }
    return 0;
}

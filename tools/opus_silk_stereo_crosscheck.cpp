// Cross-check driver for SILK stereo decode + unmix (PLAN § O2).
// Compiled twice by tools/crosscheck_opus_silk_stereo.py. Byte-identical
// fuzz oracle with state chained across frames.

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef USE_LIBOPUS
extern "C" {
#include "main.h"
#include "entdec.h"
}
struct SideA {
    stereo_dec_state st;
    ec_dec d;
    void init() { memset(&st, 0, sizeof(st)); }
    void stream(uint8_t* b, uint32_t n) { ec_dec_init(&d, b, n); }
    void pred(opus_int32* p) { silk_stereo_decode_pred(&d, p); }
    int mid_only() {
        opus_int m = 0;
        silk_stereo_decode_mid_only(&d, &m);
        return m;
    }
    void unmix(int16_t* x1, int16_t* x2, const opus_int32* p, int fs,
               int len) {
        silk_stereo_MS_to_LR(&st, x1, x2, p, fs, len);
    }
    uint32_t tell() { return (uint32_t)ec_tell(&d); }
};
#else
#include "opus_silk_stereo.hpp"
using namespace glint::opus::silk;
struct SideA {
    StereoDecState st;
    glint::opus::RangeDecoder d;
    void init() { st = StereoDecState(); }
    void stream(const uint8_t* b, uint32_t n) { d.init(b, n); }
    void pred(int32_t* p) { stereo_decode_pred(d, p); }
    int mid_only() { return stereo_decode_mid_only(d); }
    void unmix(int16_t* x1, int16_t* x2, const int32_t* p, int fs,
               int len) {
        stereo_ms_to_lr(&st, x1, x2, p, fs, len);
    }
    uint32_t tell() { return d.tell(); }
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
    static uint8_t buf[256];
    static const int kFs[3] = { 8, 12, 16 };
    for (uint32_t seed = 1; seed <= 300; seed++) {
        rng_state = seed;
        int fs = kFs[xrand() % 3];
        int len = fs * ((xrand() & 1) ? 20 : 10);
        SideA a;
        a.init();
        uint32_t blen = 16 + xrand() % 200;
        for (uint32_t i = 0; i < blen; i++) buf[i] = (uint8_t)xrand();
        a.stream(buf, blen);
        std::printf("seed %u fs %d len %d\n", seed, fs, len);
        for (int f = 0; f < 4; f++) {
            int32_t pred[2];
            a.pred(pred);
            int mo = a.mid_only();
            int16_t x1[322], x2[322];
            for (int i = 0; i < len + 2; i++) {
                x1[i] = (int16_t)(xrand() & 0xFFFF);
                x2[i] = (int16_t)(xrand() & 0xFFFF);
            }
            a.unmix(x1, x2, pred, fs, len);
            std::printf("f %d p %d %d mo %d tell %u x", f, pred[0],
                        pred[1], mo, a.tell());
            for (int i = 0; i < len + 2; i += 3)
                std::printf(" %d %d", x1[i], x2[i]);
            std::printf("\n");
        }
    }
    return 0;
}

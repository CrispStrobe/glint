// Cross-check driver for SILK excitation decoding (PLAN § O2).
// Compiled twice by tools/crosscheck_opus_silk_exc.py. Fuzz oracle:
// identical random streams; pulses[] and tells must be BYTE-IDENTICAL
// (SILK is exact integer).

#include <cstdint>
#include <cstdio>

#ifdef USE_LIBOPUS
extern "C" {
#include "main.h"
#include "entdec.h"
}
struct DecA {
    ec_dec d;
    void init(uint8_t* b, uint32_t n) { ec_dec_init(&d, b, n); }
    void pulses(int16_t* p, int st, int qo, int len) {
        silk_decode_pulses(&d, p, st, qo, len);
    }
    uint32_t tell() { return (uint32_t)ec_tell(&d); }
};
#else
#include "opus_silk_excitation.hpp"
struct DecA {
    glint::opus::RangeDecoder d;
    void init(const uint8_t* b, uint32_t n) { d.init(b, n); }
    void pulses(int16_t* p, int st, int qo, int len) {
        glint::opus::silk::decode_pulses(d, p, st, qo, len);
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
    static uint8_t buf[1024];
    static const int kLens[] = { 80, 160, 120, 240, 160, 320 };
    for (uint32_t seed = 1; seed <= 300; seed++) {
        rng_state = seed;
        uint32_t len = 8 + xrand() % 500;
        for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)xrand();
        int flen = kLens[xrand() % 6];
        int st = (int)(xrand() % 3);
        int qo = (int)(xrand() & 1);

        DecA dec;
        dec.init(buf, len);
        int16_t pulses[320 + 16];  // partial-block padding (10ms @ 12kHz)
        dec.pulses(pulses, st, qo, flen);
        std::printf("seed %u flen %d st %d qo %d tell %u p", seed, flen, st,
                    qo, dec.tell());
        int n = (flen + 15) & ~15;  // shell blocks round up
        for (int i = 0; i < n; i++) std::printf(" %d", pulses[i]);
        std::printf("\n");
    }
    return 0;
}

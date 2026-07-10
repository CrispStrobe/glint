// Cross-check driver for full SILK frame decoding (PLAN § O2).
// Compiled twice by tools/crosscheck_opus_silk_frame.py. Fuzz oracle:
// identical random streams through the complete frame decode (indices +
// pulses + parameters + core synthesis + history upkeep), frames chained
// per sequence. SILK is exact integer: xq and tells byte-identical.
// Sequences 1-250 are all-clean (the original pre-PLC gate content);
// sequences 251-500 mix good and LOST frames (random loss pattern incl.
// loss-at-start and consecutive losses), exercising PLC conceal/update,
// CNG estimation/generation and the glue fade against the reference
// silk_decode_frame with FLAG_PACKET_LOST.

#include <cstdint>
#include <cstdio>

#ifdef USE_LIBOPUS
extern "C" {
#include "main.h"
#include "entdec.h"
}
struct SideA {
    silk_decoder_state st;
    ec_dec d;
    void init(int fs_khz, int nb_subfr) {
        silk_init_decoder(&st);
        st.nb_subfr = nb_subfr;
        silk_decoder_set_fs(&st, fs_khz, 48000);
    }
    void stream(uint8_t* b, uint32_t n) { ec_dec_init(&d, b, n); }
    int frame(int vad, int cond, int16_t* xq, int lost = 0) {
        st.VAD_flags[0] = vad;
        opus_int32 n = 0;
        silk_decode_frame(&st, &d, xq, &n,
                          lost ? FLAG_PACKET_LOST : FLAG_DECODE_NORMAL,
                          cond, 0);
        return (int)n;
    }
    uint32_t tell() { return (uint32_t)ec_tell(&d); }
};
#else
#include "opus_silk_frame.hpp"
using namespace glint::opus::silk;
struct SideA {
    DecoderState st;
    glint::opus::RangeDecoder d;
    void init(int fs_khz, int nb_subfr) { st.set_fs(fs_khz, nb_subfr); }
    void stream(const uint8_t* b, uint32_t n) { d.init(b, n); }
    int frame(int vad, int cond, int16_t* xq, int lost = 0) {
        st.vad_flags[0] = vad;
        // Null decoder on loss proves the conceal path never reads it.
        return decode_frame(&st, lost ? nullptr : &d, xq, cond, lost != 0);
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
    static uint8_t buf[2048];
    static const int kFs[3] = { 8, 12, 16 };
    for (uint32_t seed = 1; seed <= 250; seed++) {
        rng_state = seed;
        int fs = kFs[xrand() % 3];
        int nsf = (xrand() & 1) ? 4 : 2;
        SideA a;
        a.init(fs, nsf);
        uint32_t len = 30 + xrand() % 1200;
        for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)xrand();
        a.stream(buf, len);
        std::printf("seed %u fs %d nsf %d len %u\n", seed, fs, nsf, len);
        for (int f = 0; f < 4; f++) {
            int vad = (int)(xrand() & 1);
            // First frame independent (as in real streams); later frames
            // mix conditional and independent coding.
            int cond = f == 0 ? 0 : (int)(xrand() % 3);
            int16_t xq[320] = { 0 };
            int n = a.frame(vad, cond, xq);
            std::printf("f %d vad %d cond %d n %d tell %u xq", f, vad,
                        cond, n, a.tell());
            for (int i = 0; i < n; i++) std::printf(" %d", xq[i]);
            std::printf("\n");
        }
    }
    // Loss-mixing sequences: PLC conceal/update, CNG and glue. Forced
    // patterns guarantee loss-at-start and consecutive-loss coverage on
    // top of the ~1/3 random losses.
    for (uint32_t seed = 251; seed <= 500; seed++) {
        rng_state = seed;
        int fs = kFs[xrand() % 3];
        int nsf = (xrand() & 1) ? 4 : 2;
        SideA a;
        a.init(fs, nsf);
        uint32_t len = 30 + xrand() % 1200;
        for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)xrand();
        a.stream(buf, len);
        std::printf("seed %u fs %d nsf %d len %u (loss mix)\n", seed, fs,
                    nsf, len);
        for (int f = 0; f < 6; f++) {
            int lost = (int)(xrand() % 3 == 0);
            if (seed % 5 == 0 && f == 0) lost = 1;              // at start
            if (seed % 7 == 0 && (f == 2 || f == 3)) lost = 1;  // burst
            int vad = (int)(xrand() & 1);
            int cond = f == 0 ? 0 : (int)(xrand() % 3);
            int16_t xq[320] = { 0 };
            int n = a.frame(vad, cond, xq, lost);
            std::printf("f %d lost %d vad %d cond %d n %d tell %u xq", f,
                        lost, vad, cond, n, a.tell());
            for (int i = 0; i < n; i++) std::printf(" %d", xq[i]);
            std::printf("\n");
        }
    }
    return 0;
}

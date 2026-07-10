// Cross-check driver for CELT bit allocation (PLAN § O1).
// Compiled twice by tools/crosscheck_opus_alloc.py (reference vs glint).
// All quantities are integers, so stdout must be BYTE-IDENTICAL: caps,
// codedBands, per-band pulses/ebits/fine_priority, balance, intensity,
// dual_stereo, and the decoder tell after the embedded skip/intensity/
// dual-stereo symbols.

#include <cstdint>
#include <cstdio>

#ifdef USE_LIBOPUS
extern "C" {
#include "modes.h"
#include "rate.h"
#include "celt.h"
#include "entdec.h"
#include "entenc.h"
}
struct AllocA {
    ec_dec d;
    const CELTMode* mode;
    int caps[21];
    void init(uint8_t* b, uint32_t n, int lm, int c) {
        mode = opus_custom_mode_create(48000, 960, 0);
        init_caps(mode, caps, lm, c);
        ec_dec_init(&d, b, n);
    }
    int alloc(int start, int end, const int* offsets, int trim,
              int* intensity, int* dual, int32_t total, int32_t* balance,
              int* pulses, int* ebits, int* prio, int c, int lm) {
        return clt_compute_allocation(mode, start, end, offsets, caps, trim,
                                      intensity, dual, total, balance,
                                      pulses, ebits, prio, c, lm, &d, 0, 0,
                                      0);
    }
    uint32_t tell() { return (uint32_t)ec_tell(&d); }
};
struct AllocEncA {
    ec_enc e;
    const CELTMode* mode;
    int caps[21];
    uint8_t* buf_;
    void init(uint8_t* b, uint32_t n, int lm, int c) {
        mode = opus_custom_mode_create(48000, 960, 0);
        init_caps(mode, caps, lm, c);
        ec_enc_init(&e, b, n);
        buf_ = b;
    }
    int alloc(int start, int end, const int* offsets, int trim,
              int* intensity, int* dual, int32_t total, int32_t* balance,
              int* pulses, int* ebits, int* prio, int c, int lm, int prev,
              int sbw) {
        return clt_compute_allocation(mode, start, end, offsets, caps, trim,
                                      intensity, dual, total, balance,
                                      pulses, ebits, prio, c, lm,
                                      (ec_ctx*)&e, 1, prev, sbw);
    }
    void done() { ec_enc_done(&e); }
    uint32_t tell() { return (uint32_t)ec_tell(&e); }
};
#else
#include "opus_celt_rate.hpp"
#include "opus_ec.hpp"
struct AllocA {
    glint::opus::RangeDecoder d;
    int caps[21];
    void init(const uint8_t* b, uint32_t n, int lm, int c) {
        glint::opus::init_caps(caps, lm, c);
        d.init(b, n);
    }
    int alloc(int start, int end, const int* offsets, int trim,
              int* intensity, int* dual, int32_t total, int32_t* balance,
              int* pulses, int* ebits, int* prio, int c, int lm) {
        return glint::opus::compute_allocation_dec(
            start, end, offsets, caps, trim, intensity, dual, total,
            balance, pulses, ebits, prio, c, lm, d);
    }
    uint32_t tell() { return d.tell(); }
};
struct AllocEncA {
    glint::opus::RangeEncoder e;
    int caps[21];
    uint8_t* buf_;
    void init(uint8_t* b, uint32_t n, int lm, int c) {
        glint::opus::init_caps(caps, lm, c);
        e.init(b, n);
        buf_ = b;
    }
    int alloc(int start, int end, const int* offsets, int trim,
              int* intensity, int* dual, int32_t total, int32_t* balance,
              int* pulses, int* ebits, int* prio, int c, int lm, int prev,
              int sbw) {
        return glint::opus::compute_allocation_enc(
            start, end, offsets, caps, trim, intensity, dual, total,
            balance, pulses, ebits, prio, c, lm, e, prev, sbw);
    }
    void done() { e.done(); }
    uint32_t tell() { return e.tell(); }
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
    static uint8_t buf[64];
    for (uint32_t seed = 1; seed <= 200; seed++) {
        rng_state = seed;
        for (int i = 0; i < 64; i++) buf[i] = (uint8_t)xrand();
        int lm = (int)(xrand() % 4);
        int c = 1 + (int)(xrand() & 1);
        int start = (xrand() % 4 == 0) ? 17 : 0;  // 17 = hybrid mode start
        int end = start == 17 ? 18 + (int)(xrand() % 4)
                              : 16 + (int)(xrand() % 6);
        int trim = (int)(xrand() % 11);
        int offsets[21];
        for (int j = 0; j < 21; j++)
            offsets[j] = (xrand() % 3 == 0) ? (int)(xrand() % 500) : 0;
        int32_t total = (int32_t)(xrand() % 20000) - 100;

        AllocA a;
        a.init(buf, 64, lm, c);
        int intensity = 0, dual = 0;
        int32_t balance = 0;
        int pulses[21] = { 0 }, ebits[21] = { 0 }, prio[21] = { 0 };
        int coded = a.alloc(start, end, offsets, trim, &intensity, &dual,
                            total, &balance, pulses, ebits, prio, c, lm);

        std::printf("seed %u lm %d c %d start %d end %d trim %d total %d\n",
                    seed, lm, c, start, end, trim, total);
        std::printf("caps");
        for (int j = 0; j < 21; j++) std::printf(" %d", a.caps[j]);
        std::printf("\ncoded %d intensity %d dual %d balance %d tell %u\n",
                    coded, intensity, dual, balance, a.tell());
        std::printf("pulses");
        for (int j = start; j < end; j++) std::printf(" %d", pulses[j]);
        std::printf("\nebits");
        for (int j = start; j < end; j++) std::printf(" %d", ebits[j]);
        std::printf("\nprio");
        for (int j = start; j < end; j++) std::printf(" %d", prio[j]);
        std::printf("\n");
    }
    return 0;
}

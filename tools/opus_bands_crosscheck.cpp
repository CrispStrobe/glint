// Cross-check driver for CELT band decoding (PLAN § O1).
// Compiled twice by tools/crosscheck_opus_bands.py (reference vs glint).
//
// Fuzz oracle: identical random byte streams into both decoders, wired the
// way celt_decoder.c does it — allocator first (already byte-identical),
// its pulses/intensity/dual/balance into quant_all_bands, then the
// anti-collapse bit + pass. Integers (collapse masks, seed, tells) must
// match exactly; normalized spectra within float-vs-double tolerance.

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef USE_LIBOPUS
extern "C" {
#include "modes.h"
#include "rate.h"
#include "celt.h"
#include "bands.h"
#include "entdec.h"
#include "entenc.h"
}
typedef float NormT;
struct SideA {
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
    void bands(int start, int end, NormT* X, NormT* Y, uint8_t* masks,
               const int* pulses, int transient, int spread, int dual,
               int intensity, int* tf_res, int32_t total_bits,
               int32_t balance, int lm, int coded, uint32_t* seed,
               int disable_inv) {
        quant_all_bands(0, mode, start, end, X, Y, masks, 0, (int*)pulses,
                        transient, spread, dual, intensity, tf_res,
                        total_bits, balance, &d, lm, coded, seed, 0, 0,
                        disable_inv);
    }
    void anti(NormT* X, const uint8_t* masks, int lm, int c, int size,
              int start, int end, const NormT* le, const NormT* p1,
              const NormT* p2, const int* pulses, uint32_t seed) {
        anti_collapse(mode, X, (unsigned char*)masks, lm, c, size, start,
                      end, le, p1, p2, (int*)pulses, seed, 0);
    }
    uint32_t tell() { return (uint32_t)ec_tell(&d); }
    uint32_t tellf() { return ec_tell_frac(&d); }
    uint32_t bits(unsigned n) { return ec_dec_bits(&d, n); }
};
struct EncSideA {
    ec_enc e;
    const CELTMode* mode;
    int caps[21];
    void init(uint8_t* b, uint32_t n, int lm, int c) {
        mode = opus_custom_mode_create(48000, 960, 0);
        init_caps(mode, caps, lm, c);
        ec_enc_init(&e, b, n);
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
    void bands(int start, int end, float* X, float* Y, uint8_t* masks,
               const float* bandE, const int* pulses, int transient,
               int spread, int dual, int intensity, int* tf_res,
               int32_t total_bits, int32_t balance, int lm, int coded,
               uint32_t* seed) {
        quant_all_bands(1, mode, start, end, X, Y, masks, bandE,
                        (int*)pulses, transient, spread, dual, intensity,
                        tf_res, total_bits, balance, (ec_ctx*)&e, lm, coded,
                        seed, 0, 0, 0);
    }
    void done() { ec_enc_done(&e); }
    uint32_t tell() { return (uint32_t)ec_tell(&e); }
};
#else
#include "opus_celt_bands.hpp"
#include "opus_celt_rate.hpp"
#include "opus_ec.hpp"
typedef double NormT;
struct SideA {
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
    void bands(int start, int end, NormT* X, NormT* Y, uint8_t* masks,
               const int* pulses, int transient, int spread, int dual,
               int intensity, int* tf_res, int32_t total_bits,
               int32_t balance, int lm, int coded, uint32_t* seed,
               int disable_inv) {
        glint::opus::quant_all_bands_dec(start, end, X, Y, masks, pulses,
                                         transient, spread, dual, intensity,
                                         tf_res, total_bits, balance, d, lm,
                                         coded, seed, disable_inv);
    }
    void anti(NormT* X, const uint8_t* masks, int lm, int c, int size,
              int start, int end, const NormT* le, const NormT* p1,
              const NormT* p2, const int* pulses, uint32_t seed) {
        glint::opus::anti_collapse(X, masks, lm, c, size, start, end, le,
                                   p1, p2, pulses, seed);
    }
    uint32_t tell() { return d.tell(); }
    uint32_t tellf() { return d.tell_frac(); }
    uint32_t bits(unsigned n) { return d.dec_bits(n); }
};
#include "opus_celt_enc_bands.hpp"
struct EncSideA {
    glint::opus::RangeEncoder e;
    int caps[21];
    void init(uint8_t* b, uint32_t n, int lm, int c) {
        glint::opus::init_caps(caps, lm, c);
        e.init(b, n);
    }
    int alloc(int start, int end, const int* offsets, int trim,
              int* intensity, int* dual, int32_t total, int32_t* balance,
              int* pulses, int* ebits, int* prio, int c, int lm, int prev,
              int sbw) {
        return glint::opus::compute_allocation_enc(
            start, end, offsets, caps, trim, intensity, dual, total,
            balance, pulses, ebits, prio, c, lm, e, prev, sbw);
    }
    void bands(int start, int end, float* X, float* Y, uint8_t* masks,
               const float* bandE, const int* pulses, int transient,
               int spread, int dual, int intensity, int* tf_res,
               int32_t total_bits, int32_t balance, int lm, int coded,
               uint32_t* seed) {
        glint::opus::quant_all_bands_enc(start, end, X, Y, masks, bandE,
                                         pulses, transient, spread, dual,
                                         intensity, tf_res, total_bits,
                                         balance, e, lm, coded, seed);
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

// tf_select_table domain (celt.c): legal tf_res values per (LM, transient).
static const signed char kTfSelect[4][8] = {
    { 0, -1, 0, -1, 0, -1, 0, -1 },
    { 0, -1, 0, -2, 1, 0, 1, -1 },
    { 0, -2, 0, -3, 2, 0, 1, -1 },
    { 0, -2, 0, -3, 3, 0, 1, -1 },
};

int main() {
    static uint8_t buf[512];
    static uint8_t mask_store[42];
    static NormT X[2 * 960];
    static NormT logE[42], p1[42], p2[42];

    for (uint32_t seed_i = 1; seed_i <= 150; seed_i++) {
        rng_state = seed_i;
        uint32_t len = 60 + xrand() % 340;
        for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)xrand();
        int lm = (int)(xrand() % 4);
        int c = 1 + (int)(xrand() & 1);
        int start = (xrand() % 5 == 0) ? 17 : 0;
        int end = 21;
        int transient = lm > 0 && (xrand() & 1);
        int spread = (int)(xrand() % 4);
        int trim = (int)(xrand() % 11);
        int disable_inv = (int)(xrand() & 1);
        int tf_sel = (int)(xrand() & 1);
        int tf_res[21];
        for (int i = 0; i < 21; i++)
            tf_res[i] =
                kTfSelect[lm][4 * transient + 2 * tf_sel + (xrand() & 1)];
        int offsets[21];
        for (int j = 0; j < 21; j++)
            offsets[j] = (xrand() % 4 == 0) ? (int)(xrand() % 300) : 0;
        int M = 1 << lm;
        int size = 120 * M;
        for (int i = 0; i < 2 * 960; i++)
            X[i] = (NormT)(((int)(xrand() % 200) - 100) / 100.0);
        for (int i = 0; i < 42; i++) {
            logE[i] = (NormT)(((int)(xrand() % 300) - 100) / 10.0);
            p1[i] = (NormT)(((int)(xrand() % 300) - 100) / 10.0);
            p2[i] = (NormT)(((int)(xrand() % 300) - 100) / 10.0);
        }

        std::memset(mask_store, 0, sizeof(mask_store));
        SideA a;
        a.init(buf, len, lm, c);

        // Mirror celt_decode_with_ec's budget plumbing.
        int32_t total_frame = (int32_t)len * (8 << 3);
        int32_t bits = total_frame - (int32_t)a.tellf() - 1;
        int anti_rsv =
            transient && lm >= 2 && bits >= ((int32_t)lm + 2) << 3 ? 1 << 3
                                                                   : 0;
        bits -= anti_rsv;
        int intensity = 0, dual = 0;
        int32_t balance = 0;
        int pulses[21] = { 0 }, ebits[21] = { 0 }, prio[21] = { 0 };
        int coded = a.alloc(start, end, offsets, trim, &intensity, &dual,
                            bits, &balance, pulses, ebits, prio, c, lm);

        uint32_t rng_seed = xrand();
        uint32_t rng_out = rng_seed;
        a.bands(start, end, X, c == 2 ? X + size : (NormT*)0,
                (uint8_t*)mask_store, pulses, transient, spread, dual,
                intensity, tf_res, total_frame - anti_rsv, balance, lm,
                coded, &rng_out, disable_inv);
        std::printf("seed %u len %u lm %d c %d start %d tr %d sp %d: "
                    "coded %d int %d dual %d tell %u rng %u\n",
                    seed_i, len, lm, c, start, transient, spread, coded,
                    intensity, dual, a.tell(), rng_out);
        std::printf("masks");
        for (int i = 0; i < 21 * c; i++) std::printf(" %u", mask_store[i]);
        std::printf("\n");

        int anti_on = 0;
        if (anti_rsv) anti_on = (int)a.bits(1);
        if (anti_on)
            a.anti(X, mask_store, lm, c, size, start, end, logE, p1, p2,
                   pulses, rng_out);
        std::printf("anti %d tell %u\nX", anti_on, a.tell());
        for (int i = 0; i < c * size; i += 7)
            std::printf(" %.6f", (double)X[i]);
        std::printf("\n");
    }

    // ---- ENCODE pass: allocator-enc + quant_all_bands(encode=1),
    // byte-exact streams (float32 decisions pinned in the enc kernels).
    static float Xf[2 * 960];
    static float bandE[42];
    static uint8_t ebuf[600];
    for (uint32_t seed_i = 1; seed_i <= 150; seed_i++) {
        rng_state = seed_i ^ 0xEC0DE;
        int lm = (int)(xrand() % 4);
        int c = 1 + (int)(xrand() & 1);
        int start = (xrand() % 5 == 0) ? 17 : 0;
        int end = 21;
        int transient = lm > 0 && (xrand() & 1);
        int spread = (int)(xrand() % 4);
        int trim = (int)(xrand() % 11);
        int tf_sel = (int)(xrand() & 1);
        int tf_res[21];
        for (int i = 0; i < 21; i++)
            tf_res[i] =
                kTfSelect[lm][4 * transient + 2 * tf_sel + (xrand() & 1)];
        int offsets[21];
        for (int j = 0; j < 21; j++)
            offsets[j] = (xrand() % 4 == 0) ? (int)(xrand() % 300) : 0;
        int M = 1 << lm;
        // Unit-ish normalized spectra + positive band energies, identical
        // float values on both sides.
        for (int i = 0; i < 2 * 960; i++)
            Xf[i] = (float)(((int)(xrand() % 2001) - 1000) / 1000.0f);
        for (int i = 0; i < 42; i++)
            bandE[i] = (float)((1 + (int)(xrand() % 10000)) / 100.0f);
        uint32_t len = 60 + xrand() % 400;
        int32_t total_frame = (int32_t)len * (8 << 3);
        int32_t bits = total_frame - 1 - 8;  // leave the tell()==1 slack
        int intensity = start + (int)(xrand() % (end + 1 - start));
        int dual = c == 2 ? (int)(xrand() & 1) : 0;
        int prev = (int)(xrand() % 22);
        int sbw = (int)(xrand() % 21);

        EncSideA a;
        a.init(ebuf, len, lm, c);
        int32_t balance = 0;
        int pulses[21] = { 0 }, ebits[21] = { 0 }, prio[21] = { 0 };
        int coded = a.alloc(start, end, offsets, trim, &intensity, &dual,
                            bits, &balance, pulses, ebits, prio, c, lm,
                            prev, sbw);
        uint8_t masks[42] = { 0 };
        uint32_t rng_seed = xrand();
        a.bands(start, end, Xf, c == 2 ? Xf + 120 * M : (float*)0, masks,
                bandE, pulses, transient, spread, dual, intensity, tf_res,
                total_frame, balance, lm, coded, &rng_seed);
        uint32_t tell = a.tell();
        a.done();
        std::printf("encb seed %u lm %d c %d st %d tr %d sp %d: coded %d "
                    "int %d dual %d tell %u\nmasks",
                    seed_i, lm, c, start, transient, spread, coded,
                    intensity, dual, tell);
        for (int i = 0; i < 21 * c; i++) std::printf(" %u", masks[i]);
        std::printf("\nbytes ");
        for (uint32_t i = 0; i < len; i++) std::printf("%02x", ebuf[i]);
        std::printf("\n");
    }
    return 0;
}

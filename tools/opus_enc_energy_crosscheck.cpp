// Cross-check driver for the CELT ENCODER-side energy pipeline (PLAN § O4).
// Compiled twice by tools/crosscheck_opus_enc_energy.py:
//  - reference: this driver + celt/quant_bands.c + celt/bands.c compiled
//    fresh with -ffp-contract=off (IEEE-pinned float semantics; the
//    prebuilt libopus.a fuses FMAs and vectorizes, which is legal for the
//    codec but not byte-reproducible), linked against libopus.a for the
//    range coder / laplace / mode machinery;
//  - glint: opus_celt_enc_energy + opus_ec + opus_laplace +
//    opus_celt_energy (the decoder, for the self-decode consistency leg),
//    also -ffp-contract=off.
//
// Everything printed is exact (tells, %a hex floats, encoded bytes), so
// the gate is byte-identical stdout. Each scenario chains oldEBands /
// delayedIntra state across several frames; small buffers exercise the
// <15/<2/<1-bit coarse fallbacks and the badness clamps.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef USE_LIBOPUS
extern "C" {
#include "modes.h"
#include "bands.h"
#include "quant_bands.h"
#include "entenc.h"
#include "entdec.h"
}

struct Api {
    const CELTMode* mode = nullptr;
    ec_enc enc;
    float dec_old[42];

    void setup() { mode = opus_custom_mode_create(48000, 960, 0); }
    void band_energies(const float* x, float* be, int end, int c, int lm) {
        compute_band_energies(mode, x, be, end, c, lm, 0);
    }
    void log_energies(int eff_end, int end, float* be, float* ble, int c) {
        amp2Log2(mode, eff_end, end, be, ble, c);
    }
    void normalise(const float* freq, float* xn, const float* be, int end,
                   int c, int m) {
        normalise_bands(mode, freq, xn, be, end, c, m);
    }
    void enc_init(uint8_t* buf, uint32_t len) { ec_enc_init(&enc, buf, len); }
    void coarse(int start, int end, int eff_end, const float* eb,
                float* old_e, uint32_t budget, float* err, int c, int lm,
                int nb_avail, int force_intra, float* di, int two_pass,
                int loss_rate, int lfe) {
        quant_coarse_energy(mode, start, end, eff_end, eb, old_e, budget,
                            err, &enc, c, lm, nb_avail, force_intra, di,
                            two_pass, loss_rate, lfe);
    }
    void fine(int start, int end, float* old_e, float* err, int* fq, int c) {
        quant_fine_energy(mode, start, end, old_e, err, fq, &enc, c);
    }
    void finalise(int start, int end, float* old_e, float* err, int* fq,
                  int* fp, int bits_left, int c) {
        quant_energy_finalise(mode, start, end, old_e, err, fq, fp,
                              bits_left, &enc, c);
    }
    uint32_t tell() { return (uint32_t)ec_tell(&enc); }
    uint32_t tellq3() { return ec_tell_frac(&enc); }
    void enc_done() { ec_enc_done(&enc); }
    int enc_error() { return enc.error; }

    void dec_reset(const float* init, int c) {
        for (int i = 0; i < c * 21; i++) dec_old[i] = init[i];
    }
    // Decode the frame with the reference decoder against this side's own
    // chained state; return max |decoded - encoder old_e| over the coded
    // bands and the decoder tell.
    double self_decode(uint8_t* buf, uint32_t len, int start, int end,
                       int c, int lm, int* fq, int* fp, int bits_left,
                       const float* enc_old, uint32_t* dtell) {
        ec_dec dec;
        ec_dec_init(&dec, buf, len);
        int budget = (int)len * 8;
        int intra =
            ec_tell(&dec) + 3 <= budget ? ec_dec_bit_logp(&dec, 3) : 0;
        unquant_coarse_energy(mode, start, end, dec_old, intra, &dec, c, lm);
        unquant_fine_energy(mode, start, end, dec_old, fq, &dec, c);
        unquant_energy_finalise(mode, start, end, dec_old, fq, fp,
                                bits_left, &dec, c);
        *dtell = (uint32_t)ec_tell(&dec);
        double m = 0;
        for (int ch = 0; ch < c; ch++)
            for (int i = start; i < end; i++) {
                double d = std::fabs((double)dec_old[i + ch * 21] -
                                     (double)enc_old[i + ch * 21]);
                if (d > m) m = d;
            }
        return m;
    }
};
#else
#include "opus_celt_enc_energy.hpp"
#include "opus_celt_energy.hpp"
#include "opus_ec.hpp"

struct Api {
    glint::opus::RangeEncoder enc;
    double dec_old[42];

    void setup() {}
    void band_energies(const float* x, float* be, int end, int c, int lm) {
        glint::opus::compute_band_energies(x, be, end, c, lm);
    }
    void log_energies(int eff_end, int end, float* be, float* ble, int c) {
        glint::opus::amp2Log2(eff_end, end, be, ble, c);
    }
    void normalise(const float* freq, float* xn, const float* be, int end,
                   int c, int m) {
        glint::opus::normalise_bands(freq, xn, be, end, c, m);
    }
    void enc_init(uint8_t* buf, uint32_t len) { enc.init(buf, len); }
    void coarse(int start, int end, int eff_end, const float* eb,
                float* old_e, uint32_t budget, float* err, int c, int lm,
                int nb_avail, int force_intra, float* di, int two_pass,
                int loss_rate, int lfe) {
        glint::opus::quant_coarse_energy(start, end, eff_end, eb, old_e,
                                         budget, err, enc, c, lm, nb_avail,
                                         force_intra, di, two_pass,
                                         loss_rate, lfe);
    }
    void fine(int start, int end, float* old_e, float* err, int* fq, int c) {
        glint::opus::quant_fine_energy(start, end, old_e, err, fq, enc, c);
    }
    void finalise(int start, int end, float* old_e, float* err, int* fq,
                  int* fp, int bits_left, int c) {
        glint::opus::quant_energy_finalise(start, end, old_e, err, fq, fp,
                                           bits_left, enc, c);
    }
    uint32_t tell() { return enc.tell(); }
    uint32_t tellq3() { return enc.tell_frac(); }
    void enc_done() { enc.done(); }
    int enc_error() { return enc.error(); }

    void dec_reset(const float* init, int c) {
        for (int i = 0; i < c * 21; i++) dec_old[i] = init[i];
    }
    // Decode with GLINT's (double) energy decoder: internal-consistency
    // leg — the decoded energies must land on the encoder's quantized
    // oldEBands within float-vs-double drift.
    double self_decode(uint8_t* buf, uint32_t len, int start, int end,
                       int c, int lm, int* fq, int* fp, int bits_left,
                       const float* enc_old, uint32_t* dtell) {
        glint::opus::RangeDecoder dec;
        dec.init(buf, len);
        uint32_t budget = len * 8;
        int intra = dec.tell() + 3 <= budget ? dec.dec_bit_logp(3) : 0;
        glint::opus::unquant_coarse_energy(start, end, dec_old, intra != 0,
                                           dec, c, lm);
        glint::opus::unquant_fine_energy(start, end, dec_old, fq, dec, c);
        glint::opus::unquant_energy_finalise(start, end, dec_old, fq, fp,
                                             bits_left, dec, c);
        *dtell = dec.tell();
        double m = 0;
        for (int ch = 0; ch < c; ch++)
            for (int i = start; i < end; i++) {
                double d = std::fabs(dec_old[i + ch * 21] -
                                     (double)enc_old[i + ch * 21]);
                if (d > m) m = d;
            }
        return m;
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

// Band edges of the 48k/960 mode's 120-sample short MDCT (eband5ms).
static const int kEb[22] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12,
                            14, 16, 20, 24, 28, 34, 40, 48, 60, 78, 100};

static void print_f(const char* tag, const float* v, int n) {
    std::printf("%s", tag);
    for (int i = 0; i < n; i++) std::printf(" %a", (double)v[i]);
    std::printf("\n");
}

static uint32_t hash_bits(const float* v, int n) {
    // FNV-1a over the float bit patterns: exact, order-sensitive.
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) {
        uint32_t b;
        std::memcpy(&b, &v[i], 4);
        for (int k = 0; k < 4; k++) {
            h ^= (b >> (8 * k)) & 0xff;
            h *= 16777619u;
        }
    }
    return h;
}

int main() {
    Api api;
    api.setup();
    static float freq[2 * 960], xn[2 * 960];
    float band_e[42], band_log_e[42], old_e[42], err[42];
    static uint8_t buf[256];

    for (uint32_t seed = 1; seed <= 300; seed++) {
        rng_state = seed;
        int c = 1 + (int)(xrand() & 1);
        int lm = (int)(xrand() % 4);
        int start = (xrand() % 8 == 0) ? 17 : 0;
        int end = start + 1 + (int)(xrand() % (unsigned)(21 - start));
        int eff_end = start + 1 + (int)(xrand() % (unsigned)(end - start));
        int nframes = 1 + (int)(xrand() % 3);
        for (int i = 0; i < c * 21; i++)
            old_e[i] = (float)(((int)(xrand() % 560) - 280) / 10.0f);
        float di = (float)(xrand() % 200) * 0.5f;
        api.dec_reset(old_e, c);
        std::printf("seed %u C %d LM %d start %d end %d effEnd %d fr %d\n",
                    seed, c, lm, start, end, eff_end, nframes);

        for (int fr = 0; fr < nframes; fr++) {
            const int n = 120 << lm;
            for (int j = 0; j < c * n; j++)
                freq[j] =
                    (float)(((int)(xrand() % 2000001) - 1000000) / 4000.0f);
            if (xrand() % 3 == 0) {
                // Silence one band in every channel: epsilon-path test.
                int b = (int)(xrand() % 21);
                for (int ch = 0; ch < c; ch++)
                    for (int j = kEb[b] << lm; j < kEb[b + 1] << lm; j++)
                        freq[ch * n + j] = 0.f;
            }
            std::memset(xn, 0, sizeof(xn));
            api.band_energies(freq, band_e, end, c, lm);
            api.log_energies(eff_end, end, band_e, band_log_e, c);
            api.normalise(freq, xn, band_e, end, c, 1 << lm);
            for (int ch = 0; ch < c; ch++) {
                print_f("bandE", &band_e[ch * 21], end);
                print_f("bandLogE", &band_log_e[ch * 21], end);
            }
            std::printf("normhash %08x\n", hash_bits(xn, c * n));

            uint32_t len = 2 + xrand() % 199;
            std::memset(buf, 0, sizeof(buf));
            api.enc_init(buf, len);
            uint32_t budget = len * 8;
            int nb_avail = (int)(xrand() % 256);
            int force_intra = (xrand() % 8) == 0;
            int two_pass = (int)(xrand() & 1);
            int loss_rate = (xrand() & 1) ? 0 : (int)(xrand() % 101);
            int lfe = (xrand() % 16) == 0;
            std::printf(
                "frame %d len %u avail %d fi %d tp %d loss %d lfe %d\n", fr,
                len, nb_avail, force_intra, two_pass, loss_rate, lfe);
            std::memset(err, 0, sizeof(err));

            api.coarse(start, end, eff_end, band_log_e, old_e, budget, err,
                       c, lm, nb_avail, force_intra, &di, two_pass,
                       loss_rate, lfe);
            std::printf("coarse tell %u tf %u dI %a\n", api.tell(),
                        api.tellq3(), (double)di);
            print_f("oldE", old_e, c * 21);
            // error[] is only defined on the coded bands (the reference
            // leaves the rest uninitialized too) — print just that slice.
            for (int ch = 0; ch < c; ch++)
                print_f("err", &err[ch * 21 + start], end - start);

            int fq[21], fp[21];
            int remaining = (int)budget - (int)api.tell();
            if (remaining < 0) remaining = 0;
            for (int i = 0; i < 21; i++) {
                int v = (int)(xrand() % 9);
                if (i >= start && i < end) {
                    int cap = remaining / c;
                    if (v > cap) v = cap;
                    remaining -= c * v;
                }
                fq[i] = v;
                fp[i] = (int)(xrand() & 1);
            }
            api.fine(start, end, old_e, err, fq, c);
            std::printf("fine tell %u\n", api.tell());
            print_f("oldE", old_e, c * 21);
            for (int ch = 0; ch < c; ch++)
                print_f("err", &err[ch * 21 + start], end - start);

            int bits_left = (int)(xrand() % 50);
            int rem2 = (int)budget - (int)api.tell();
            if (rem2 < 0) rem2 = 0;
            if (bits_left > rem2) bits_left = rem2;
            api.finalise(start, end, old_e, err, fq, fp, bits_left, c);
            std::printf("final tell %u bl %d\n", api.tell(), bits_left);
            print_f("oldE", old_e, c * 21);
            for (int ch = 0; ch < c; ch++)
                print_f("err", &err[ch * 21 + start], end - start);

            api.enc_done();
            std::printf("bytes e%d ", api.enc_error());
            for (uint32_t j = 0; j < len; j++) std::printf("%02x", buf[j]);
            std::printf("\n");

            uint32_t dtell = 0;
            double delta = api.self_decode(buf, len, start, end, c, lm, fq,
                                           fp, bits_left, old_e, &dtell);
            std::printf("selfdecode tell %u %s\n", dtell,
                        delta <= 2e-3 ? "OK" : "FAIL");
        }
    }
    return 0;
}

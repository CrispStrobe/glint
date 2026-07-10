// Cross-check driver for CELT energy-envelope decoding (PLAN § O1).
// Compiled twice by tools/crosscheck_opus_energy.py (reference vs glint).
//
// Decode-only components are cross-checked by FUZZING: the range decoder
// accepts any byte stream, so both decoders consume identical pseudo-random
// buffers and must produce identical symbol traces (tells exact; energies
// match within float-vs-double tolerance). Small buffer sizes deliberately
// exercise the <15/<2/<1-bit budget fallback paths.

#include <cstdint>
#include <cstdio>

#ifdef USE_LIBOPUS
extern "C" {
#include "modes.h"
#include "quant_bands.h"
#include "entdec.h"
}
typedef float EnergyT;
struct DecA {
    ec_dec d;
    const CELTMode* mode;
    void init(uint8_t* b, uint32_t n) {
        mode = opus_custom_mode_create(48000, 960, 0);
        ec_dec_init(&d, b, n);
    }
    void coarse(EnergyT* e, int intra, int c, int lm) {
        unquant_coarse_energy(mode, 0, 21, e, intra, &d, c, lm);
    }
    void fine(EnergyT* e, int* fq, int c) {
        unquant_fine_energy(mode, 0, 21, e, fq, &d, c);
    }
    void finalise(EnergyT* e, int* fq, int* fp, int bits_left, int c) {
        unquant_energy_finalise(mode, 0, 21, e, fq, fp, bits_left, &d, c);
    }
    uint32_t tell() { return (uint32_t)ec_tell(&d); }
};
#else
#include "opus_celt_energy.hpp"
#include "opus_ec.hpp"
typedef double EnergyT;
struct DecA {
    glint::opus::RangeDecoder d;
    void init(const uint8_t* b, uint32_t n) { d.init(b, n); }
    void coarse(EnergyT* e, int intra, int c, int lm) {
        glint::opus::unquant_coarse_energy(0, 21, e, intra != 0, d, c, lm);
    }
    void fine(EnergyT* e, int* fq, int c) {
        glint::opus::unquant_fine_energy(0, 21, e, fq, d, c);
    }
    void finalise(EnergyT* e, int* fq, int* fp, int bits_left, int c) {
        glint::opus::unquant_energy_finalise(0, 21, e, fq, fp, bits_left, d,
                                             c);
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

static void print_e(const char* stage, const EnergyT* e, int c,
                    uint32_t tell) {
    std::printf("%s tell %u E", stage, tell);
    for (int i = 0; i < c * 21; i++) std::printf(" %.3f", (double)e[i]);
    std::printf("\n");
}

int main() {
    static uint8_t buf[256];
    for (uint32_t seed = 1; seed <= 40; seed++) {
        rng_state = seed;
        uint32_t len = 3 + xrand() % 198;  // small sizes hit fallback paths
        for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)xrand();
        int intra = (int)(xrand() & 1);
        int lm = (int)(xrand() % 4);
        int c = 1 + (int)(xrand() & 1);
        EnergyT e[42];
        for (int i = 0; i < 42; i++)
            e[i] = (EnergyT)(((int)(xrand() % 400) - 200) / 10.0);
        std::printf("seed %u len %u intra %d lm %d c %d\n", seed, len,
                    intra, lm, c);
        DecA dec;
        dec.init(buf, len);
        dec.coarse(e, intra, c, lm);
        print_e("coarse", e, c, dec.tell());
        int fq[21], fp[21];
        for (int i = 0; i < 21; i++) fq[i] = (int)(xrand() % 9);
        dec.fine(e, fq, c);
        print_e("fine", e, c, dec.tell());
        for (int i = 0; i < 21; i++) fp[i] = (int)(xrand() & 1);
        int bits_left = (int)(xrand() % 50);
        dec.finalise(e, fq, fp, bits_left, c);
        print_e("final", e, c, dec.tell());
    }
    return 0;
}

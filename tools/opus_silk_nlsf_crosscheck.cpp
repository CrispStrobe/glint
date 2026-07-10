// Cross-check driver for the SILK NLSF -> LPC chain (PLAN § O2).
// Compiled twice by tools/crosscheck_opus_silk_nlsf.py. Fuzz oracle:
// identical random codebook indices through NLSF decode + NLSF2A, plus
// direct NLSF2A on random sorted-ish vectors (including degenerate
// near-equal ones for the stabilize / LPC-fit edge paths). SILK is exact
// integer, so all outputs must be BYTE-IDENTICAL.

#include <cstdint>
#include <cstdio>

#ifdef USE_LIBOPUS
extern "C" {
#include "main.h"
}
static void api_nlsf_decode(int16_t* nlsf, int8_t* idx, int wb) {
    silk_NLSF_decode(nlsf, idx, wb ? &silk_NLSF_CB_WB : &silk_NLSF_CB_NB_MB);
}
static void api_nlsf2a(int16_t* a, const int16_t* nlsf, int d) {
    silk_NLSF2A(a, nlsf, d, 0);
}
#else
#include "opus_silk_nlsf.hpp"
static void api_nlsf_decode(int16_t* nlsf, int8_t* idx, int wb) {
    using namespace glint::opus::silk;
    nlsf_decode(nlsf, idx, wb ? kNlsfCbWb : kNlsfCbNbMb);
}
static void api_nlsf2a(int16_t* a, const int16_t* nlsf, int d) {
    glint::opus::silk::nlsf2a(a, nlsf, d);
}
#endif

static uint32_t rng_state;
static uint32_t xrand() {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

static void print_vec(const char* tag, const int16_t* v, int n) {
    std::printf("%s", tag);
    for (int i = 0; i < n; i++) std::printf(" %d", v[i]);
}

int main() {
    // Codebook-index fuzz: stage-1 in [0,32), residuals across the full
    // extension range decode_indices can produce ([-10,10]).
    for (int wb = 0; wb <= 1; wb++) {
        int order = wb ? 16 : 10;
        for (uint32_t seed = 1; seed <= 1200; seed++) {
            rng_state = seed + (wb ? 0x80000000u : 0);
            int8_t idx[17];
            idx[0] = (int8_t)(xrand() % 32);
            for (int i = 1; i <= order; i++)
                idx[i] = (int8_t)((int)(xrand() % 21) - 10);
            int16_t nlsf[16], a[16];
            api_nlsf_decode(nlsf, idx, wb);
            api_nlsf2a(a, nlsf, order);
            std::printf("wb %d seed %u", wb, seed);
            print_vec(" nlsf", nlsf, order);
            print_vec(" a", a, order);
            std::printf("\n");
        }
    }

    // Direct NLSF2A fuzz: sorted ascending vectors in [0,32767] with random
    // gaps; every 3rd seed clusters values (gaps 0..3) to force near-equal
    // LSFs -> huge polynomial coefficients -> the LPC_fit / bandwidth-
    // expansion / inverse-prediction-gain repair paths.
    for (int wb = 0; wb <= 1; wb++) {
        int order = wb ? 16 : 10;
        for (uint32_t seed = 1; seed <= 1200; seed++) {
            rng_state = seed ^ (wb ? 0x55555555u : 0x2AAAAAAAu);
            int16_t nlsf[16];
            if (seed % 3 == 0) {
                int32_t v = (int32_t)(xrand() % 30000);
                for (int i = 0; i < order; i++) {
                    nlsf[i] = (int16_t)(v > 32767 ? 32767 : v);
                    v += (int32_t)(xrand() % 4);
                }
            } else {
                for (int i = 0; i < order; i++)
                    nlsf[i] = (int16_t)(xrand() % 32768);
                for (int i = 1; i < order; i++) {  // insertion sort
                    int16_t value = nlsf[i];
                    int j = i - 1;
                    for (; j >= 0 && value < nlsf[j]; j--) nlsf[j + 1] = nlsf[j];
                    nlsf[j + 1] = value;
                }
            }
            int16_t a[16];
            api_nlsf2a(a, nlsf, order);
            std::printf("n2a wb %d seed %u", wb, seed);
            print_vec(" a", a, order);
            std::printf("\n");
        }
    }
    return 0;
}

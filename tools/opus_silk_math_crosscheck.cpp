// Cross-check driver for the SILK fixed-point kit (PLAN § O2).
// Compiled twice by tools/crosscheck_opus_silk_math.py. SILK is exact
// integer math, so outputs must be BYTE-IDENTICAL over randomized operands
// (biased toward boundary magnitudes).

#include <cstdint>
#include <cstdio>

#ifdef USE_LIBOPUS
extern "C" {
#include "SigProc_FIX.h"
}
#define F_SAT16(a) silk_SAT16(a)
#define F_ADD_SAT32(a, b) silk_ADD_SAT32(a, b)
#define F_SUB_SAT32(a, b) silk_SUB_SAT32(a, b)
#define F_LSHIFT_SAT32(a, s) silk_LSHIFT_SAT32(a, s)
#define F_RSHIFT_ROUND(a, s) silk_RSHIFT_ROUND(a, s)
#define F_SMULWB(a, b) silk_SMULWB(a, b)
#define F_SMLAWB(a, b, c) silk_SMLAWB(a, b, c)
#define F_SMULWT(a, b) silk_SMULWT(a, b)
#define F_SMULWW(a, b) silk_SMULWW(a, b)
#define F_SMLAWW(a, b, c) silk_SMLAWW(a, b, c)
#define F_SMULBB(a, b) silk_SMULBB(a, b)
#define F_SMLABB(a, b, c) silk_SMLABB(a, b, c)
#define F_SMULBT(a, b) silk_SMULBT(a, b)
#define F_SMULTT(a, b) silk_SMULTT(a, b)
#define F_CLZ32(a) silk_CLZ32(a)
#define F_DIV32_varQ(a, b, q) silk_DIV32_varQ(a, b, q)
#define F_INVERSE32_varQ(b, q) silk_INVERSE32_varQ(b, q)
#define F_lin2log(a) silk_lin2log(a)
#define F_log2lin(a) silk_log2lin(a)
#define F_RAND(a) silk_RAND(a)
#define F_ADD32_ovflw(a, b) silk_ADD32_ovflw(a, b)
#else
#include "opus_silk_math.hpp"
using namespace glint::opus::silk;
#define F_SAT16(a) sat16(a)
#define F_ADD_SAT32(a, b) add_sat32(a, b)
#define F_SUB_SAT32(a, b) sub_sat32(a, b)
#define F_LSHIFT_SAT32(a, s) lshift_sat32(a, s)
#define F_RSHIFT_ROUND(a, s) rshift_round(a, s)
#define F_SMULWB(a, b) smulwb(a, b)
#define F_SMLAWB(a, b, c) smlawb(a, b, c)
#define F_SMULWT(a, b) smulwt(a, b)
#define F_SMULWW(a, b) smulww(a, b)
#define F_SMLAWW(a, b, c) smlaww(a, b, c)
#define F_SMULBB(a, b) smulbb(a, b)
#define F_SMLABB(a, b, c) smlabb(a, b, c)
#define F_SMULBT(a, b) smulbt(a, b)
#define F_SMULTT(a, b) smultt(a, b)
#define F_CLZ32(a) clz32((uint32_t)(a))
#define F_DIV32_varQ(a, b, q) div32_varq(a, b, q)
#define F_INVERSE32_varQ(b, q) inverse32_varq(b, q)
#define F_lin2log(a) lin2log(a)
#define F_log2lin(a) log2lin(a)
#define F_RAND(a) silk_rand(a)
#define F_ADD32_ovflw(a, b) add32_ovflw(a, b)
#endif

static uint32_t rng_state = 1;
static uint32_t xrand() {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

// Magnitude-biased operand: exercises boundary widths, not just big values.
static int32_t rnd_op() {
    uint32_t v = xrand();
    int width = 1 + (int)(xrand() % 31);
    int32_t r = (int32_t)(v & ((width == 31) ? 0x7FFFFFFFu
                                             : ((1u << width) - 1)));
    return (xrand() & 1) ? -r : r;
}

int main(int argc, char**) {
    // Default: 2M iterations, one hash line. With any argv: 2000 verbose
    // iterations printing every value (for diffing a mismatch).
    const bool verbose = argc > 1;
    const int iters = verbose ? 2000 : 2000000;
    uint64_t h = 1469598103934665603ull;  // FNV accumulate, printed at end
    auto mix = [&](int32_t v) {
        h ^= (uint32_t)v;
        h *= 1099511628211ull;
        if (verbose) std::printf("%d\n", v);
    };

    for (int i = 0; i < iters; i++) {
        int32_t a = rnd_op(), b = rnd_op(), c = rnd_op();
        int s = 1 + (int)(xrand() % 30);
        mix(F_SAT16(a));
        mix(F_ADD_SAT32(a, b));
        mix(F_SUB_SAT32(a, b));
        mix(F_LSHIFT_SAT32(a, s));
        mix(F_RSHIFT_ROUND(a, s));
        mix(F_SMULWB(a, b));
        mix(F_SMLAWB(a, b, c));
        mix(F_SMULWT(a, b));
        mix(F_SMULWW(a, b));
        mix(F_SMLAWW(a, b, c));
        mix(F_SMULBB(a, b));
        mix(F_SMLABB(a, b, c));
        mix(F_SMULBT(a, b));
        mix(F_SMULTT(a, b));
        mix(F_ADD32_ovflw(a, b));
        if (a) mix(F_CLZ32(a));
        if (b > 0) {
            int q = (int)(xrand() % 48);
            mix(F_INVERSE32_varQ(b, q));
            if (a) mix(F_DIV32_varQ(a, b, q));
        }
        if (a > 0) mix(F_lin2log(a));
        mix(F_log2lin(a & 0xFFF));  // representative log-domain range
        mix(F_RAND(a));
    }
    std::printf("hash %016llx\n", (unsigned long long)h);
    return 0;
}

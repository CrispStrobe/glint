// Cross-check driver for the CELT encoder VQ layer (PLAN § O4 step 4):
// alg_quant (exp_rotation + op_pvq_search + encode_pulses + resynthesis),
// stereo_itheta, exp_rotation, stereo_split, intensity_stereo.
// Compiled twice by tools/crosscheck_opus_enc_vq.py:
//   -DUSE_LIBOPUS : reference libopus 1.5.2 custom-modes float build
//                   (alg_quant / stereo_itheta / exp_rotation are exported)
//   (default)     : glint opus_celt_enc_vq + opus_ec + opus_cwrs
// Both binaries print the same script: every INTEGER line (encoded bytes,
// tells, collapse masks, decoded pulse vectors, itheta, round-trip flags)
// must be byte-identical — the float32 rules in opus_celt_enc_vq.cpp make
// the chosen pulse vectors identical, so the streams must match exactly.
// Lines starting with 'X' carry float spectra; the python wrapper compares
// those with 1e-6 tolerance (residual normalization float noise only).
//
// stereo_split / intensity_stereo are file-static in the reference bands.c,
// so the USE_LIBOPUS side carries a local float-build macro expansion of
// them (fusion semantics verified against the inlined codegen in bands.o);
// the full band-encode crosscheck (PLAN § O4 step 5) revalidates them
// in situ.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef USE_LIBOPUS
extern "C" {
#include "entenc.h"
#include "entdec.h"
#include "cwrs.h"
// vq.h drags in modes.h; declare the three exported functions directly
// (float build: celt_norm == opus_val16 == float).
unsigned alg_quant(float* X, int N, int K, int spread, int B, ec_enc* enc,
                   float gain, int resynth, int arch);
int stereo_itheta(const float* X, const float* Y, int stereo, int N,
                  int arch);
void exp_rotation(float* X, int len, int dir, int stride, int K, int spread);
}

struct Enc {
    ec_enc e;
    void init(uint8_t* b, uint32_t n) { ec_enc_init(&e, b, n); }
    unsigned quant(float* x, int n, int k, int spread, int b, float gain) {
        return alg_quant(x, n, k, spread, b, &e, gain, 1, 0);
    }
    void done() { ec_enc_done(&e); }
    uint32_t tell() { return (uint32_t)ec_tell(&e); }
    uint32_t tell_frac() { return (uint32_t)ec_tell_frac(&e); }
    int error() { return e.error; }
};
struct Dec {
    ec_dec d;
    void init(uint8_t* b, uint32_t n) { ec_dec_init(&d, b, n); }
    int32_t pulses(int* y, int n, int k) {
        return (int32_t)decode_pulses(y, n, k, &d);
    }
    uint32_t tell() { return (uint32_t)ec_tell(&d); }
};

static void rot(float* x, int len, int dir, int stride, int k, int spread) {
    exp_rotation(x, len, dir, stride, k, spread);
}
static int itheta_fn(const float* x, const float* y, int stereo, int n) {
    return stereo_itheta(x, y, stereo, n, 0);
}
// Local float-build expansions of the static bands.c helpers (see top).
static void split_fn(float* x, float* y, int n) {
    for (int j = 0; j < n; j++) {
        float l = 0.70710678f * x[j];
        float r = 0.70710678f * y[j];
        x[j] = l + r;
        y[j] = r - l;
    }
}
static void inten_fn(float* x, const float* y, const float* band_e, int band,
                     int nb_bands, int n) {
    float left = band_e[band];
    float right = band_e[band + nb_bands];
    float t = fmaf(left, left, 1e-15f);
    t = fmaf(right, right, t);
    float norm = 1e-15f + sqrtf(t);
    float a1 = left / norm;
    float a2 = right / norm;
    for (int j = 0; j < n; j++) {
        float l = x[j];
        float r = y[j];
        float u = a2 * r;
        x[j] = fmaf(a1, l, u);
    }
}
#else
#include "opus_celt_enc_vq.hpp"
#include "opus_cwrs.hpp"
#include "opus_ec.hpp"

struct Enc {
    glint::opus::RangeEncoder e;
    void init(uint8_t* b, uint32_t n) { e.init(b, n); }
    unsigned quant(float* x, int n, int k, int spread, int b, float gain) {
        return glint::opus::alg_quant(x, n, k, spread, b, e, gain, true);
    }
    void done() { e.done(); }
    uint32_t tell() { return e.tell(); }
    uint32_t tell_frac() { return e.tell_frac(); }
    int error() { return e.error(); }
};
struct Dec {
    glint::opus::RangeDecoder d;
    void init(const uint8_t* b, uint32_t n) { d.init(b, n); }
    int32_t pulses(int* y, int n, int k) {
        return glint::opus::decode_pulses(y, n, k, d);
    }
    uint32_t tell() { return d.tell(); }
};

static void rot(float* x, int len, int dir, int stride, int k, int spread) {
    glint::opus::exp_rotation(x, len, dir, stride, k, spread);
}
static int itheta_fn(const float* x, const float* y, int stereo, int n) {
    return glint::opus::stereo_itheta(x, y, stereo, n);
}
static void split_fn(float* x, float* y, int n) {
    glint::opus::stereo_split(x, y, n);
}
static void inten_fn(float* x, const float* y, const float* band_e, int band,
                     int nb_bands, int n) {
    glint::opus::intensity_stereo(x, y, band_e, band, nb_bands, n);
}
#endif

// Driver-local reconstruction used by BOTH sides in the decode replay:
// only single-rounded float ops, so it is bit-identical to the inlined
// normalise_residual of either implementation.
static void reconstruct(const int* iy, float* x, int n, float ryy,
                        float gain) {
    float g = (1.f / sqrtf(ryy)) * gain;
    for (int i = 0; i < n; i++) x[i] = g * (float)iy[i];
}

static uint32_t rng_state;
static uint32_t xrand() {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}
static float frand() {  // uniform-ish in (-1, 1)
    return (float)(int32_t)xrand() * (1.0f / 2147483648.0f);
}

// Band/split sizes reachable in standard CELT modes.
static const int kSizes[] = { 2,  3,  4,  5,  6,  8,  12, 16,  18,  22,
                              24, 36, 44, 48, 64, 72, 88, 96, 144, 176 };
static const int kNumSizes = (int)(sizeof(kSizes) / sizeof(kSizes[0]));
static const int kBVals[] = { 1, 2, 4, 8 };

// V(n,k) in 64-bit (driver-local, independent of both codecs) to pick k
// values whose codebook fits the 32-bit index space.
static uint64_t v_size(int n, int k) {
    uint64_t row[131];
    row[0] = 0;
    for (int j = 1; j <= k + 1; j++) row[j] = 2ull * j - 1;
    for (int r = 2; r < n; r++) {
        uint64_t base = 1;
        for (int j = 2; j <= k + 1; j++) {
            uint64_t up = row[j] + row[j - 1] + base;
            row[j - 1] = base;
            base = up;
        }
        row[k + 1] = base;
        if (base > (1ull << 40)) return base;  // already far past 32 bits
    }
    return row[k] + row[k + 1];
}

// Deterministic input vectors over the real CELT domain: dense unit-ish,
// unit-normalized, sparse, near-zero (hits the pyramid-projection guard),
// hot (sum >= 64 hits it from the other side), and silence.
static void build_x(float* x, int n) {
    unsigned mode = xrand() % 8;
    if (mode == 0) {
        for (int j = 0; j < n; j++) x[j] = 0.f;
    } else if (mode == 1) {
        for (int j = 0; j < n; j++) x[j] = frand() * 1e-20f;
    } else if (mode == 2) {
        for (int j = 0; j < n; j++) x[j] = 0.f;
        for (int p = 0; p < 3; p++) x[xrand() % n] = frand();
    } else if (mode == 3) {
        for (int j = 0; j < n; j++) x[j] = frand() * 8.f;
    } else if (mode <= 5) {
        for (int j = 0; j < n; j++) x[j] = frand();
    } else {
        float e = 1e-15f;
        for (int j = 0; j < n; j++) {
            x[j] = frand();
            e += x[j] * x[j];
        }
        float g = 1.f / sqrtf(e);
        for (int j = 0; j < n; j++) x[j] *= g;
    }
}

static uint32_t fbits(float v) {
    uint32_t u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

static void print_x(const char* tag, int i, const float* x, int n) {
    printf("%s %d:", tag, i);
    for (int j = 0; j < n; j++) printf(" %.9e", x[j]);
    printf("\n");
}

// Fuzz volume (override with -DVQ_SEEDS=... for stress runs).
#ifndef VQ_SEEDS
#define VQ_SEEDS 8
#endif
enum { kBufSize = 16384, kCasesPerSeed = 100, kSeeds = VQ_SEEDS };

int main() {
    static uint8_t buf[kBufSize];
    static float xq[kCasesPerSeed][176];  // resynth output (round-trip ref)
    static int pn[kCasesPerSeed], pk[kCasesPerSeed], psp[kCasesPerSeed],
        pb[kCasesPerSeed];
    static float pgain[kCasesPerSeed];

    // ---- alg_quant fuzz: encode, then decode replay ----
    for (uint32_t seed = 1; seed <= kSeeds; seed++) {
        printf("seed %u\n", seed);
        rng_state = seed;
        Enc enc;
        enc.init(buf, kBufSize);
        for (int i = 0; i < kCasesPerSeed; i++) {
            int n = kSizes[xrand() % kNumSizes];
            int k = 1 + (int)(xrand() % 128);
            while (v_size(n, k) >= 0xFFFFFFFFull) k = 1 + (k - 1) / 2;
            int spread = (int)(xrand() % 4);
            int b;
            do {
                b = kBVals[xrand() % 4];
            } while (b > n);
            float gain = (xrand() & 1)
                             ? 1.0f
                             : (float)(1 + xrand() % 1000000) * 1e-6f;
            float x[176];
            build_x(x, n);
            memcpy(xq[i], x, n * sizeof(float));
            unsigned cm = enc.quant(xq[i], n, k, spread, b, gain);
            pn[i] = n; pk[i] = k; psp[i] = spread; pb[i] = b; pgain[i] = gain;
            printf("enc %d n %d k %d spread %d B %d gain %08x cm %u "
                   "tell %u tf %u\n",
                   i, n, k, spread, b, fbits(gain), cm, enc.tell(),
                   enc.tell_frac());
            print_x("X", i, xq[i], n);
        }
        enc.done();
        printf("encerr %d\n", enc.error());
        printf("bytes ");
        for (uint32_t i = 0; i < kBufSize; i++) printf("%02x", buf[i]);
        printf("\n");

        // Decode replay: the stream must decode to the pulse vectors the
        // search chose, and reconstruct to the exact resynth output.
        Dec dec;
        dec.init(buf, kBufSize);
        for (int i = 0; i < kCasesPerSeed; i++) {
            int iy[176];
            int32_t yy = dec.pulses(iy, pn[i], pk[i]);
            printf("dec %d yy %d tell %u iy", i, yy, dec.tell());
            for (int j = 0; j < pn[i]; j++) printf(" %d", iy[j]);
            printf("\n");
            float xr[176];
            reconstruct(iy, xr, pn[i], (float)yy, pgain[i]);
            rot(xr, pn[i], -1, pb[i], pk[i], psp[i]);
            int exact = memcmp(xr, xq[i], pn[i] * sizeof(float)) == 0;
            printf("rt %d %d\n", i, exact);
        }
    }

    // ---- stereo_itheta fuzz ----
    rng_state = 0xace1u;
    for (int i = 0; i < 400; i++) {
        int n = kSizes[xrand() % kNumSizes];
        int stereo = (int)(xrand() & 1);
        float x[176], y[176];
        build_x(x, n);
        unsigned ymode = xrand() % 4;
        if (ymode == 0) {
            build_x(y, n);
        } else if (ymode == 1) {  // strongly correlated pair
            for (int j = 0; j < n; j++) y[j] = x[j] + 0.01f * frand();
        } else if (ymode == 2) {  // one side silent
            for (int j = 0; j < n; j++) y[j] = 0.f;
        } else {                  // anti-correlated
            for (int j = 0; j < n; j++) y[j] = -x[j];
        }
        printf("itheta %d n %d st %d val %d\n", i, n, stereo,
               itheta_fn(x, y, stereo, n));
    }

    // ---- exp_rotation forward/inverse ----
    rng_state = 0xbeefu;
    for (int i = 0; i < 200; i++) {
        int len = kSizes[xrand() % kNumSizes];
        int k = 1 + (int)(xrand() % 128);
        int spread = 1 + (int)(xrand() % 3);
        int b;
        do {
            b = kBVals[xrand() % 4];
        } while (b > len);
        float x[176];
        build_x(x, len);
        rot(x, len, 1, b, k, spread);
        print_x("Xf", i, x, len);
        rot(x, len, -1, b, k, spread);
        print_x("Xb", i, x, len);
    }

    // ---- stereo_split / intensity_stereo ----
    rng_state = 0xfeedu;
    for (int i = 0; i < 100; i++) {
        int n = kSizes[xrand() % kNumSizes];
        float x[176], y[176];
        build_x(x, n);
        build_x(y, n);
        split_fn(x, y, n);
        print_x("Xs", i, x, n);
        print_x("Xt", i, y, n);
        float band_e[2];
        band_e[0] = std::fabs(frand()) * 4.f + 1e-10f;
        band_e[1] = std::fabs(frand()) * 4.f + 1e-10f;
        build_x(x, n);
        build_x(y, n);
        inten_fn(x, y, band_e, 0, 1, n);
        print_x("Xn", i, x, n);
    }
    return 0;
}

// Cross-check driver for the CELT primitives: Laplace coder + CWRS/PVQ
// enumeration (PLAN § O1). Compiled twice by
// tools/crosscheck_opus_celt_prims.py:
//   -DUSE_LIBOPUS : reference libopus ec + ec_laplace_* + *_pulses
//   (default)     : glint opus_ec + opus_laplace + opus_cwrs
// Laplace and PVQ symbols are interleaved in ONE range-coder stream; both
// binaries' stdout (values, tells, buffers) must be byte-identical.

#include <cstdint>
#include <cstdio>

#ifdef USE_LIBOPUS
extern "C" {
#include "entenc.h"
#include "entdec.h"
#include "laplace.h"
#include "cwrs.h"
}

struct EncA {
    ec_enc e;
    void init(uint8_t* b, uint32_t n) { ec_enc_init(&e, b, n); }
    int laplace(int v, unsigned fs, int decay) {
        ec_laplace_encode(&e, &v, fs, decay);
        return v;
    }
    void pulses(const int* y, int n, int k) { encode_pulses(y, n, k, &e); }
    void done() { ec_enc_done(&e); }
    uint32_t tell() { return (uint32_t)ec_tell(&e); }
    int error() { return e.error; }
};
struct DecA {
    ec_dec d;
    void init(uint8_t* b, uint32_t n) { ec_dec_init(&d, b, n); }
    int laplace(unsigned fs, int decay) {
        return ec_laplace_decode(&d, fs, decay);
    }
    int32_t pulses(int* y, int n, int k) {
        return (int32_t)decode_pulses(y, n, k, &d);
    }
    uint32_t tell() { return (uint32_t)ec_tell(&d); }
};
#else
#include "opus_ec.hpp"
#include "opus_laplace.hpp"
#include "opus_cwrs.hpp"

struct EncA {
    glint::opus::RangeEncoder e;
    void init(uint8_t* b, uint32_t n) { e.init(b, n); }
    int laplace(int v, unsigned fs, int decay) {
        return glint::opus::laplace_encode(e, v, fs, decay);
    }
    void pulses(const int* y, int n, int k) {
        glint::opus::encode_pulses(y, n, k, e);
    }
    void done() { e.done(); }
    uint32_t tell() { return e.tell(); }
    int error() { return e.error(); }
};
struct DecA {
    glint::opus::RangeDecoder d;
    void init(const uint8_t* b, uint32_t n) { d.init(b, n); }
    int laplace(unsigned fs, int decay) {
        return glint::opus::laplace_decode(d, fs, decay);
    }
    int32_t pulses(int* y, int n, int k) {
        return glint::opus::decode_pulses(y, n, k, d);
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

// Band/split sizes reachable in standard CELT modes.
static const int kSizes[] = { 2,  3,  4,  5,  6,  8,  12, 16,  18,  22,
                              24, 36, 44, 48, 64, 72, 88, 96, 144, 176 };

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

enum { kOps = 600, kBufSize = 16384 };

// Deterministic pulse-vector construction, shared by the encode pass and the
// decode replay so both consume IDENTICAL PRNG draws (the sign draw only
// happens for a previously-empty slot).
static void build_vec(int n, int k, int* y) {
    for (int j = 0; j < n; j++) y[j] = 0;
    for (int p = 0; p < k; p++) {
        int pos = (int)(xrand() % n);
        if (y[pos] > 0) {
            y[pos]++;
        } else if (y[pos] < 0) {
            y[pos]--;
        } else {
            y[pos] = (xrand() & 1) ? 1 : -1;
        }
    }
}

int main() {
    static uint8_t buf[kBufSize];
    static int coded_vals[kOps];  // post-clamp laplace values
    static int vecs[kOps][176];
    static int vec_n[kOps], vec_k[kOps];

    for (uint32_t seed = 1; seed <= 8; seed++) {
        std::printf("seed %u\n", seed);
        // ---- encode ----
        rng_state = seed;
        EncA enc;
        enc.init(buf, kBufSize);
        for (int i = 0; i < kOps; i++) {
            if (xrand() & 1) {
                // Laplace: fs0/decay drawn exactly like CELT's e_prob_model
                // usage (prob<<7, decay<<6, decay <= 11456).
                unsigned fs = (1 + xrand() % 255) << 7;
                int decay = (1 + xrand() % 178) << 6;
                int v = (int)(xrand() % 81) - 40;
                if (xrand() % 8 == 0) v = (int)(xrand() % 4000) - 2000;
                vec_n[i] = -1;
                coded_vals[i] = enc.laplace(v, fs, decay);
                std::printf("enc %d lap %d tell %u\n", i, coded_vals[i],
                            enc.tell());
            } else {
                int n = kSizes[xrand() % (sizeof(kSizes) / sizeof(int))];
                int k = 1 + (int)(xrand() % 128);
                while (v_size(n, k) >= 0xFFFFFFFFull) k = 1 + (k - 1) / 2;
                int* y = vecs[i];
                build_vec(n, k, y);
                vec_n[i] = n;
                vec_k[i] = k;
                enc.pulses(y, n, k);
                std::printf("enc %d pvq n %d k %d tell %u\n", i, n, k,
                            enc.tell());
            }
        }
        enc.done();
        std::printf("encerr %d\n", enc.error());
        std::printf("bytes ");
        for (uint32_t i = 0; i < kBufSize; i++) std::printf("%02x", buf[i]);
        std::printf("\n");

        // ---- decode: replay the same PRNG script for the parameters ----
        DecA dec;
        dec.init(buf, kBufSize);
        rng_state = seed;
        bool ok = true;
        for (int i = 0; i < kOps; i++) {
            if (xrand() & 1) {
                unsigned fs = (1 + xrand() % 255) << 7;
                int decay = (1 + xrand() % 178) << 6;
                (void)(xrand() % 81);
                if (xrand() % 8 == 0) (void)(xrand() % 4000);
                int v = dec.laplace(fs, decay);
                if (v != coded_vals[i]) ok = false;
                std::printf("dec %d lap %d tell %u\n", i, v, dec.tell());
            } else {
                int n = kSizes[xrand() % (sizeof(kSizes) / sizeof(int))];
                int k = 1 + (int)(xrand() % 128);
                while (v_size(n, k) >= 0xFFFFFFFFull) k = 1 + (k - 1) / 2;
                int expect[176];
                build_vec(n, k, expect);
                int y[176];
                int32_t yy = dec.pulses(y, n, k);
                for (int j = 0; j < n; j++)
                    if (y[j] != vecs[i][j]) ok = false;
                std::printf("dec %d pvq yy %d tell %u\n", i, yy, dec.tell());
            }
        }
        std::printf("roundtrip %s\n", ok ? "OK" : "MISMATCH");
        if (!ok) return 1;
    }
    return 0;
}

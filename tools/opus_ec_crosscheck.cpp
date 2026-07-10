// Cross-check driver for the glint Opus range coder (PLAN § O0).
//
// Compiled twice by tools/crosscheck_opus_ec.py:
//   -DUSE_LIBOPUS : runs the script through the reference libopus entropy
//                   coder (linked from a locally built libopus.a)
//   (default)     : runs it through glint's src/opus_ec.{hpp,cpp}
//
// Both binaries print the op-by-op tell/tell_frac trace, the full encoded
// buffer, an exact-size re-encode, and the decoded values. The outputs must
// be BYTE-IDENTICAL — that is the wire-compatibility proof for O0.

#include <cstdint>
#include <cstdio>
#include <vector>

#ifdef USE_LIBOPUS
extern "C" {
#include "entenc.h"
#include "entdec.h"
}

struct EncA {
    ec_enc e;
    void init(uint8_t* b, uint32_t n) { ec_enc_init(&e, b, n); }
    void encode(uint32_t fl, uint32_t fh, uint32_t ft) { ec_encode(&e, fl, fh, ft); }
    void encode_bin(uint32_t fl, uint32_t fh, unsigned b) { ec_encode_bin(&e, fl, fh, b); }
    void bit_logp(int b, unsigned lp) { ec_enc_bit_logp(&e, b, lp); }
    void icdf(int s, const uint8_t* t, unsigned ftb) { ec_enc_icdf(&e, s, t, ftb); }
    void uint_(uint32_t v, uint32_t ft) { ec_enc_uint(&e, v, ft); }
    void bits(uint32_t v, unsigned n) { ec_enc_bits(&e, v, n); }
    void done() { ec_enc_done(&e); }
    uint32_t tell() { return (uint32_t)ec_tell(&e); }
    uint32_t tellf() { return ec_tell_frac(&e); }
    int error() { return e.error; }
};
struct DecA {
    ec_dec d;
    void init(uint8_t* b, uint32_t n) { ec_dec_init(&d, b, n); }
    unsigned decode(uint32_t ft) { return ec_decode(&d, ft); }
    unsigned decode_bin(unsigned b) { return ec_decode_bin(&d, b); }
    void update(uint32_t fl, uint32_t fh, uint32_t ft) { ec_dec_update(&d, fl, fh, ft); }
    int bit_logp(unsigned lp) { return ec_dec_bit_logp(&d, lp); }
    int icdf(const uint8_t* t, unsigned ftb) { return ec_dec_icdf(&d, t, ftb); }
    uint32_t uint_(uint32_t ft) { return ec_dec_uint(&d, ft); }
    uint32_t bits(unsigned n) { return ec_dec_bits(&d, n); }
    uint32_t tell() { return (uint32_t)ec_tell(&d); }
    uint32_t tellf() { return ec_tell_frac(&d); }
    int error() { return d.error; }
};
#else
#include "opus_ec.hpp"

struct EncA {
    glint::opus::RangeEncoder e;
    void init(uint8_t* b, uint32_t n) { e.init(b, n); }
    void encode(uint32_t fl, uint32_t fh, uint32_t ft) { e.encode(fl, fh, ft); }
    void encode_bin(uint32_t fl, uint32_t fh, unsigned b) { e.encode_bin(fl, fh, b); }
    void bit_logp(int b, unsigned lp) { e.enc_bit_logp(b, lp); }
    void icdf(int s, const uint8_t* t, unsigned ftb) { e.enc_icdf(s, t, ftb); }
    void uint_(uint32_t v, uint32_t ft) { e.enc_uint(v, ft); }
    void bits(uint32_t v, unsigned n) { e.enc_bits(v, n); }
    void done() { e.done(); }
    uint32_t tell() { return e.tell(); }
    uint32_t tellf() { return e.tell_frac(); }
    int error() { return e.error(); }
};
struct DecA {
    glint::opus::RangeDecoder d;
    void init(const uint8_t* b, uint32_t n) { d.init(b, n); }
    unsigned decode(uint32_t ft) { return d.decode(ft); }
    unsigned decode_bin(unsigned b) { return d.decode_bin(b); }
    void update(uint32_t fl, uint32_t fh, uint32_t ft) { d.dec_update(fl, fh, ft); }
    int bit_logp(unsigned lp) { return d.dec_bit_logp(lp); }
    int icdf(const uint8_t* t, unsigned ftb) { return d.dec_icdf(t, ftb); }
    uint32_t uint_(uint32_t ft) { return d.dec_uint(ft); }
    uint32_t bits(unsigned n) { return d.dec_bits(n); }
    uint32_t tell() { return d.tell(); }
    uint32_t tellf() { return d.tell_frac(); }
    int error() { return d.error(); }
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

static const uint8_t kIcdf[5] = { 200, 120, 40, 12, 0 };
enum { kOps = 2000, kBufSize = 8192, kNumKinds = 6 };

static void run_encode(uint32_t seed, EncA& enc, bool trace) {
    rng_state = seed;
    for (int i = 0; i < kOps; i++) {
        switch (xrand() % kNumKinds) {
        case 0: {
            uint32_t ft = 2 + xrand() % ((1u << 15) - 2);
            uint32_t fl = xrand() % ft;
            enc.encode(fl, fl + 1, ft);
            break;
        }
        case 1: {
            unsigned logp = 1 + xrand() % 14;
            enc.bit_logp((int)(xrand() & 1), logp);
            break;
        }
        case 2:
            enc.icdf((int)(xrand() % 5), kIcdf, 8);
            break;
        case 3: {
            uint32_t ft = 2 + xrand() % (1u << 20);
            uint32_t v = xrand() % ft;
            enc.uint_(v, ft);
            break;
        }
        case 4: {
            unsigned bits = 1 + xrand() % 24;
            enc.bits(xrand() & ((1u << bits) - 1), bits);
            break;
        }
        case 5: {
            unsigned b = 1 + xrand() % 15;
            uint32_t fl = xrand() & ((1u << b) - 1);
            enc.encode_bin(fl, fl + 1, b);
            break;
        }
        }
        if (trace)
            std::printf("op %d tell %u frac %u\n", i, enc.tell(), enc.tellf());
    }
}

static void run_decode(uint32_t seed, DecA& dec, bool trace) {
    rng_state = seed;
    for (int i = 0; i < kOps; i++) {
        uint32_t v = 0;
        switch (xrand() % kNumKinds) {
        case 0: {
            uint32_t ft = 2 + xrand() % ((1u << 15) - 2);
            (void)xrand();
            v = dec.decode(ft);
            dec.update(v, v + 1, ft);
            break;
        }
        case 1: {
            unsigned logp = 1 + xrand() % 14;
            (void)xrand();
            v = (uint32_t)dec.bit_logp(logp);
            break;
        }
        case 2:
            (void)xrand();
            v = (uint32_t)dec.icdf(kIcdf, 8);
            break;
        case 3: {
            uint32_t ft = 2 + xrand() % (1u << 20);
            (void)xrand();
            v = dec.uint_(ft);
            break;
        }
        case 4: {
            unsigned bits = 1 + xrand() % 24;
            (void)xrand();
            v = dec.bits(bits);
            break;
        }
        case 5: {
            unsigned b = 1 + xrand() % 15;
            (void)xrand();
            v = dec.decode_bin(b);
            dec.update(v, v + 1, b == 0 ? 1 : (1u << b));
            break;
        }
        }
        if (trace)
            std::printf("dec %d val %u tell %u frac %u\n", i, v, dec.tell(),
                        dec.tellf());
    }
}

static void dump(const uint8_t* buf, uint32_t n) {
    std::printf("bytes ");
    for (uint32_t i = 0; i < n; i++) std::printf("%02x", buf[i]);
    std::printf("\n");
}

int main() {
    static uint8_t buf[kBufSize];
    for (uint32_t seed = 1; seed <= 8; seed++) {
        std::printf("seed %u\n", seed);
        EncA enc;
        enc.init(buf, kBufSize);
        run_encode(seed, enc, true);
        uint32_t final_tell = enc.tell();
        enc.done();
        std::printf("encerr %d\n", enc.error());
        dump(buf, kBufSize);

        DecA dec;
        dec.init(buf, kBufSize);
        run_decode(seed, dec, true);
        std::printf("decerr %d\n", dec.error());

        // Exact-size pass: the stream must fit in ceil(tell/8) bytes.
        uint32_t nbytes = (final_tell + 7) / 8;
        EncA enc2;
        enc2.init(buf, nbytes);
        run_encode(seed, enc2, false);
        enc2.done();
        std::printf("exact %u encerr %d\n", nbytes, enc2.error());
        dump(buf, nbytes);
        DecA dec2;
        dec2.init(buf, nbytes);
        run_decode(seed, dec2, true);
        std::printf("decerr %d\n", dec2.error());
    }
    return 0;
}

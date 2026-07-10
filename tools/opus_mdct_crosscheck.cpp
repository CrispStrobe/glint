// Cross-check driver for the glint CELT MDCT, backward + forward
// (PLAN § O1 / § O4).
//
// Compiled twice by tools/crosscheck_opus_mdct.py:
//   -DUSE_LIBOPUS : runs the test script through libopus 1.5.2's
//                   clt_mdct_backward_c / clt_mdct_forward_c on the real
//                   48 kHz/960 mode (custom-modes build; float arithmetic).
//                   Also prints the max delta between the mode's window[]
//                   table and glint's analytic mdct_window()
//                   ("WINDOW maxdelta ...").
//   (default)     : runs it through glint's src/opus_mdct.{hpp,cpp} in
//                   double precision, and additionally self-checks the fast
//                   paths against direct O(S^2) evaluations of the
//                   documented closed forms ("SELFCHECK ..." backward,
//                   "FWDCHECK ..." forward) and proves the forward/backward
//                   TDAC round trip ("ROUNDTRIP ..." lines).
//
// Both binaries print every output sample as "o <idx> %.9e"; the python
// harness pairs the streams and asserts max |diff| < 2e-4 (the oracle is
// float, so byte identity is impossible). SELFCHECK/FWDCHECK/ROUNDTRIP
// lines appear only in the glint build, WINDOW only in the oracle build —
// the harness looks for each in the right stream.
//
// Test script, backward: for each config (shift 0..3 at stride 1, plus a
// transient-style shift=3 / B=8 interleaved config), run several chained
// MDCT blocks into one output buffer pre-filled with PRNG values, exactly
// the way celt_synthesis() chains them (out pointers spaced S apart, TDAC
// tail handed to the next block). Spectra are unit-RMS-scaled uniform noise
// from the same xorshift PRNG in both builds.
//
// Test script, forward: same configs. A unit-RMS PRNG time signal of
// frames*B*S + overlap samples is analyzed the way celt_encoder's
// compute_mdcts() does it: block b of frame f reads s[f*B*S + b*S ..
// f*B*S + b*S + S + ov) (hop S, ov samples shared with the neighbor) and
// writes interleaved bins X[f*B*S + b + B*k]. The glint build then chains
// its OWN backward over those spectra and checks out[g] == s[g] for
// g in [ov, frames*B*S) (< 1e-9): the TDAC round trip is unit-gain once
// the window mixing completes (first ov samples mix with prefill, the
// last ov/2 are a raw unfinished tail).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "opus_mdct.hpp"  // both builds: glint window (+ transform in default build)

#ifdef USE_LIBOPUS
extern "C" {
#include "modes.h"  // CELTMode, mdct_lookup, clt_mdct_backward_c
}
#endif

namespace {

uint32_t rng_state;
uint32_t xrand() {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}
// Uniform in [-1, 1) with a 24-bit mantissa (exactly representable in float
// AND double, so both builds start from identical values).
double frand() {
    return (xrand() >> 8) * (2.0 / 16777216.0) - 1.0;
}

struct Config {
    int shift;   // 0..3 -> S = 960 >> shift spectral bins per block
    int blocks;  // B: interleave stride / short blocks per frame
    int frames;
};
const Config kConfigs[] = {
    {0, 1, 3}, {1, 1, 3}, {2, 1, 3}, {3, 1, 3},
    {3, 8, 2},  // transient layout: 8 interleaved short MDCTs per frame
};
constexpr int kOverlap = 120;

#ifndef USE_LIBOPUS
constexpr double kPi = 3.14159265358979323846;

// Direct O(S^2) reference for the full contract (see opus_mdct.hpp):
// closed-form half-IMDCT + the documented TDAC mirror. Written from the
// formulas, independent of the FFT fast path it validates.
void direct_backward(const double* in, double* out, const double* w,
                     int overlap, int S, int stride) {
    std::vector<double> u(static_cast<size_t>(S));
    for (int j = 0; j < S; j++) {
        double base = kPi / S * (j + S + 0.5);
        double acc = 0.0;
        for (int k = 0; k < S; k++)
            acc += in[static_cast<size_t>(stride) * k] * std::cos(base * (k + 0.5));
        u[static_cast<size_t>(j)] = acc;
    }
    for (int j = 0; j < S; j++) out[overlap / 2 + j] = u[static_cast<size_t>(j)];
    for (int i = 0; i < overlap / 2; i++) {
        double prev = out[i];
        double cur = out[overlap - 1 - i];
        out[i] = w[overlap - 1 - i] * prev - w[i] * cur;
        out[overlap - 1 - i] = w[i] * prev + w[overlap - 1 - i] * cur;
    }
}

// Direct O(S^2) reference for the forward contract (see opus_mdct.hpp):
// reads x[0 .. S+ov), writes bin k to out[stride*k]. Written from the
// documented closed form, independent of the fold+FFT fast path.
void direct_forward(const double* x, double* out, const double* w,
                    int overlap, int S, int stride) {
    for (int k = 0; k < S; k++) {
        double acc = 0.0;
        for (int m = 0; m < S + overlap; m++) {
            double win = (m < overlap) ? w[m]
                         : (m >= S)    ? w[S + overlap - 1 - m]
                                       : 1.0;
            acc += win * x[static_cast<size_t>(m)] *
                   std::cos(kPi / S * (m + S - 0.5 * overlap + 0.5) * (k + 0.5));
        }
        out[static_cast<size_t>(stride) * k] = 2.0 / S * acc;
    }
}
#endif

}  // namespace

int main() {
#ifdef USE_LIBOPUS
    int err = 0;
    CELTMode* mode = opus_custom_mode_create(48000, 960, &err);
    if (!mode || err != 0) {
        std::printf("FATAL: opus_custom_mode_create failed (err %d)\n", err);
        return 1;
    }
    std::printf("MODE n=%d overlap=%d maxLM=%d shortMdctSize=%d\n",
                mode->mdct.n, mode->overlap, mode->maxLM, mode->shortMdctSize);
    if (mode->mdct.n != 1920 || mode->overlap != kOverlap || mode->maxLM != 3) {
        std::printf("FATAL: unexpected mode geometry\n");
        return 1;
    }
    // Analytic-window check: glint's mdct_window() vs the mode's table.
    double wd = 0.0;
    for (int i = 0; i < mode->overlap; i++) {
        double d = std::fabs(static_cast<double>(mode->window[i]) -
                             glint::opus::mdct_window(i, mode->overlap));
        if (d > wd) wd = d;
    }
    std::printf("WINDOW maxdelta %.9e\n", wd);
#else
    glint::opus::CeltImdct mdct(1920, 3);
    std::vector<double> window(kOverlap);
    glint::opus::mdct_window_fill(window.data(), kOverlap);
#endif

    for (size_t ci = 0; ci < sizeof(kConfigs) / sizeof(kConfigs[0]); ci++) {
        const Config& cfg = kConfigs[ci];
        const int S = 960 >> cfg.shift;  // bins per MDCT block == NB spacing
        const int B = cfg.blocks;
        const int frame_len = S * B;
        const int total = cfg.frames * frame_len + kOverlap / 2;
        std::printf("config shift=%d B=%d frames=%d total=%d\n", cfg.shift, B,
                    cfg.frames, total);

        // Identical PRNG stream in both builds: buffer pre-fill first, then
        // per-frame spectra (scaled to unit RMS in double).
        rng_state = 0x9e3779b9u + static_cast<uint32_t>(ci);
        std::vector<double> prefill(static_cast<size_t>(total));
        for (int i = 0; i < total; i++) prefill[static_cast<size_t>(i)] = frand();
        std::vector<std::vector<double>> spec(static_cast<size_t>(cfg.frames));
        for (int f = 0; f < cfg.frames; f++) {
            auto& s = spec[static_cast<size_t>(f)];
            s.resize(static_cast<size_t>(frame_len));
            double e = 0.0;
            for (int i = 0; i < frame_len; i++) {
                s[static_cast<size_t>(i)] = frand();
                e += s[static_cast<size_t>(i)] * s[static_cast<size_t>(i)];
            }
            double norm = 1.0 / std::sqrt(e / frame_len);
            for (int i = 0; i < frame_len; i++) s[static_cast<size_t>(i)] *= norm;
        }

#ifdef USE_LIBOPUS
        std::vector<float> out(static_cast<size_t>(total));
        for (int i = 0; i < total; i++)
            out[static_cast<size_t>(i)] = static_cast<float>(prefill[static_cast<size_t>(i)]);
        std::vector<float> in(static_cast<size_t>(frame_len));
        for (int f = 0; f < cfg.frames; f++) {
            for (int i = 0; i < frame_len; i++)
                in[static_cast<size_t>(i)] =
                    static_cast<float>(spec[static_cast<size_t>(f)][static_cast<size_t>(i)]);
            for (int b = 0; b < B; b++)
                clt_mdct_backward_c(&mode->mdct, in.data() + b,
                                    out.data() + f * frame_len + S * b,
                                    mode->window, mode->overlap, cfg.shift, B,
                                    /*arch=*/0);
        }
        for (int i = 0; i < total; i++)
            std::printf("o %d %.9e\n", i, static_cast<double>(out[static_cast<size_t>(i)]));
#else
        std::vector<double> out(prefill);
        for (int f = 0; f < cfg.frames; f++) {
            const double* in = spec[static_cast<size_t>(f)].data();
            for (int b = 0; b < B; b++)
                mdct.backward(in + b, out.data() + f * frame_len + S * b,
                              window.data(), kOverlap, cfg.shift, B);
        }
        for (int i = 0; i < total; i++)
            std::printf("o %d %.9e\n", i, out[static_cast<size_t>(i)]);

        // Self-check: rerun the whole config through the direct formula.
        std::vector<double> ref(prefill);
        for (int f = 0; f < cfg.frames; f++) {
            const double* in = spec[static_cast<size_t>(f)].data();
            for (int b = 0; b < B; b++)
                direct_backward(in + b, ref.data() + f * frame_len + S * b,
                                window.data(), kOverlap, S, B);
        }
        double md = 0.0;
        for (int i = 0; i < total; i++) {
            double d = std::fabs(out[static_cast<size_t>(i)] - ref[static_cast<size_t>(i)]);
            if (d > md) md = d;
        }
        std::printf("SELFCHECK config=%zu maxdiff=%.3e %s\n", ci, md,
                    md < 1e-9 ? "PASS" : "FAIL");
#endif
    }

    // ------------------------------------------------------------------
    // Forward MDCT (encoder side), same configs, compute_mdcts layout.
    // ------------------------------------------------------------------
    for (size_t ci = 0; ci < sizeof(kConfigs) / sizeof(kConfigs[0]); ci++) {
        const Config& cfg = kConfigs[ci];
        const int S = 960 >> cfg.shift;  // bins per block == input hop
        const int B = cfg.blocks;
        const int frame_len = S * B;              // bins (and samples) per frame
        const int nbins = cfg.frames * frame_len; // total bins printed
        const int in_len = nbins + kOverlap;      // time signal incl. history
        std::printf("config fwd shift=%d B=%d frames=%d bins=%d\n", cfg.shift,
                    B, cfg.frames, nbins);

        // Unit-RMS PRNG time signal, identical in both builds.
        rng_state = 0x2545f491u + static_cast<uint32_t>(ci);
        std::vector<double> sig(static_cast<size_t>(in_len));
        double e = 0.0;
        for (int i = 0; i < in_len; i++) {
            sig[static_cast<size_t>(i)] = frand();
            e += sig[static_cast<size_t>(i)] * sig[static_cast<size_t>(i)];
        }
        double norm = 1.0 / std::sqrt(e / in_len);
        for (int i = 0; i < in_len; i++) sig[static_cast<size_t>(i)] *= norm;

#ifdef USE_LIBOPUS
        std::vector<float> spec(static_cast<size_t>(nbins));
        std::vector<float> tmp(static_cast<size_t>(S + kOverlap));
        for (int f = 0; f < cfg.frames; f++) {
            for (int b = 0; b < B; b++) {
                // Fresh copy per call: the reference API takes a non-const
                // input ("Forward MDCT trashes the input array").
                const double* src = sig.data() + f * frame_len + b * S;
                for (int i = 0; i < S + kOverlap; i++)
                    tmp[static_cast<size_t>(i)] = static_cast<float>(src[i]);
                clt_mdct_forward_c(&mode->mdct, tmp.data(),
                                   spec.data() + f * frame_len + b,
                                   mode->window, mode->overlap, cfg.shift, B,
                                   /*arch=*/0);
            }
        }
        for (int i = 0; i < nbins; i++)
            std::printf("o %d %.9e\n", i,
                        static_cast<double>(spec[static_cast<size_t>(i)]));
#else
        std::vector<double> spec(static_cast<size_t>(nbins));
        for (int f = 0; f < cfg.frames; f++)
            for (int b = 0; b < B; b++)
                mdct.forward(sig.data() + f * frame_len + b * S,
                             spec.data() + f * frame_len + b, window.data(),
                             kOverlap, cfg.shift, B);
        for (int i = 0; i < nbins; i++)
            std::printf("o %d %.9e\n", i, spec[static_cast<size_t>(i)]);

        // Self-check: rerun through the direct O(S^2) closed form.
        std::vector<double> ref(static_cast<size_t>(nbins));
        for (int f = 0; f < cfg.frames; f++)
            for (int b = 0; b < B; b++)
                direct_forward(sig.data() + f * frame_len + b * S,
                               ref.data() + f * frame_len + b, window.data(),
                               kOverlap, S, B);
        double md = 0.0;
        for (int i = 0; i < nbins; i++) {
            double d = std::fabs(spec[static_cast<size_t>(i)] -
                                 ref[static_cast<size_t>(i)]);
            if (d > md) md = d;
        }
        std::printf("FWDCHECK config=%zu maxdiff=%.3e %s\n", ci, md,
                    md < 1e-9 ? "PASS" : "FAIL");

        // Round trip: chain glint's backward over the forward spectra the
        // way celt_synthesis does; TDAC must rebuild sig at unit gain on
        // [ov, nbins) (head mixes with the zero prefill, the last ov/2
        // written samples are a raw unfinished tail at [nbins, ...)).
        std::vector<double> rec(static_cast<size_t>(nbins + kOverlap / 2), 0.0);
        for (int f = 0; f < cfg.frames; f++)
            for (int b = 0; b < B; b++)
                mdct.backward(spec.data() + f * frame_len + b,
                              rec.data() + f * frame_len + b * S,
                              window.data(), kOverlap, cfg.shift, B);
        double rt = 0.0;
        for (int i = kOverlap; i < nbins; i++) {
            double d = std::fabs(rec[static_cast<size_t>(i)] -
                                 sig[static_cast<size_t>(i)]);
            if (d > rt) rt = d;
        }
        std::printf("ROUNDTRIP config=%zu maxdiff=%.3e %s\n", ci, rt,
                    rt < 1e-9 ? "PASS" : "FAIL");
#endif
    }
    return 0;
}

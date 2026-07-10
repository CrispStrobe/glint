// glint - Opus/CELT inverse MDCT (PLAN.md § O1)
// MIT License - Clean-room implementation
//
// Semantics-compatible with libopus clt_mdct_backward (celt/mdct.c), which
// the CELT decoder's celt_synthesis() calls once per MDCT block. CELT at
// 48 kHz uses one mdct_lookup of size n = 1920 time samples (= 2 x 960
// spectral bins) with maxshift = 3; `shift` selects the transform size
// N = n >> shift, i.e. S = N/2 spectral bins in {960, 480, 240, 120}.
// These are NOT powers of two (960 = 2^6 * 3 * 5), hence the mixed-radix
// complex FFT (radices 2/3/4/5) underneath.
//
// ---------------------------------------------------------------------------
// Contract of CeltImdct::backward(in, out, window, overlap, shift, stride)
// (verified against libopus 1.5.2 by tools/crosscheck_opus_mdct.py):
//
//   S  = (n >> shift) / 2      spectral bins consumed
//   ov = overlap               window length (120 for all 48 kHz modes)
//
// Input:  spectral coefficient k is read from in[stride * k], k = 0..S-1.
//   celt_synthesis() stores interleaved short-block spectra as
//   freq[b + B*k] and calls backward(&freq[b], ..., stride = B) per block b.
//
// Output: the call writes EXACTLY out[0 .. S + ov/2), nothing else.
//   Let u[j], j = 0..S-1, be the unscaled half-IMDCT ("middle half" of the
//   time-aliased 2S-point inverse transform; no normalization factor):
//
//       u[j] = sum_{k=0}^{S-1} X[k] * cos( pi/S * (j + S + 1/2) * (k + 1/2) )
//
//   * out[ov/2 .. S + ov/2) = u[0 .. S)                    -- OVERWRITTEN.
//     The last ov/2 of these (out[S .. S+ov/2)) are raw folded tail values;
//     the NEXT call (with out advanced by S) completes them.
//   * out[0 .. ov) is the TDAC cross-fade region. It combines the PREVIOUS
//     content of out[0 .. ov/2) -- the raw tail the previous block left
//     there -- with this block's u[0 .. ov/2) (which the step above just
//     placed at out[ov/2 .. ov)). For i = 0 .. ov/2 - 1, with
//     prev = old out[i] and cur = u[ov/2 - 1 - i]:
//
//         out[i]          = window[ov-1-i] * prev - window[i]      * cur
//         out[ov-1-i]     = window[i]      * prev + window[ov-1-i] * cur
//
//     So out[0 .. ov/2) is read-modify-write (the only samples whose prior
//     content matters); out[ov/2 .. S + ov/2) is pure overwrite. Nothing is
//     a plain "+=": the overlap-add is realized by this windowed rotation.
//
// Scaling: NONE anywhere (matches the reference: "backward MDCT (no
//   scaling)"; the missing factor 2 of a textbook TDAC round-trip is folded
//   into the window mixing, so forward(4/N-scaled) o backward reconstructs
//   at 1/2 amplitude).
//
// Aliasing: all reads of `in` happen before any write to `out`, so the
//   decoder's in-place trick (in == out + ov/2, celt_synthesis's
//   mono-to-stereo copy path) is safe; `in` is destroyed in that case.
//
// Chaining (what celt_synthesis relies on): consecutive blocks write to
//   out pointers spaced S apart; block m's raw tail at its out[S .. S+ov/2)
//   is exactly block m+1's "previous content of out[0 .. ov/2)".
// ---------------------------------------------------------------------------

#pragma once

#include <vector>

namespace glint {
namespace opus {

// Analytic CELT window (celt/modes.c): a Vorbis-style power window over the
// `overlap` region only (the rest of the effective MDCT window is flat 1):
//
//     w(i) = sin( pi/2 * sin^2( pi/2 * (i + 1/2) / overlap ) ),  i = 0..ov-1
//
// The decoder should use this analytic form, not a generated table.
double mdct_window(int i, int overlap);
// Fills w[0 .. overlap).
void mdct_window_fill(double* w, int overlap);

// One inverse-MDCT engine covering sizes n >> shift for shift = 0..maxshift
// (mirrors libopus's single mdct_lookup with per-shift trig + FFT tables).
// n is the LARGEST transform size in time samples (2x spectral bins);
// the 48 kHz CELT modes use CeltImdct(1920, 3). n >> maxshift must be
// divisible by 4 and (n >> shift)/4 must factor into 2/3/5 for every shift.
// Tables are built once in the constructor. Not thread-safe (per-instance
// scratch); use one instance per decoder.
class CeltImdct {
public:
    explicit CeltImdct(int n = 1920, int maxshift = 3);

    // See the contract block above. window must have `overlap` entries
    // (mdct_window_fill output or a decoded mode's table). overlap must be
    // even and <= (n >> shift) / 2.
    void backward(const double* in, double* out, const double* window,
                  int overlap, int shift, int stride) const;

    int size(int shift) const { return n_ >> shift; }  // time samples (2S)
    int max_shift() const { return maxshift_; }

private:
    struct Cpx {
        double re, im;
    };

    // Mixed-radix (2/3/4/5) forward complex DFT, natural order in and out,
    // unscaled: Z[k] = sum_j z[j] e^{-2 pi i jk / n}.
    struct MixedFft {
        int n = 0;
        std::vector<Cpx> tw;  // e^{-2 pi i k / n}, k = 0..n-1
        void init(int size);
        void run(const Cpx* x, Cpx* y) const;

    private:
        void recurse(const Cpx* x, Cpx* y, int len, int stride) const;
    };

    int n_ = 0;
    int maxshift_ = 0;
    std::vector<std::vector<double>> trig_;  // per shift: cos(2pi(i+1/8)/N), i<N/2
    std::vector<MixedFft> fft_;              // per shift: (N/4)-point
    mutable std::vector<Cpx> scratch_;       // 2 x N/4 (pre-rotate + FFT out)
};

}  // namespace opus
}  // namespace glint

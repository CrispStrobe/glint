// glint - AAC forward MDCT (long windows)
// MIT License - Clean-room implementation
//
// 2048-point MDCT per ISO/IEC 13818-7 / 14496-3:
//   X[k] = 2 * sum_{n=0}^{N-1} z[n] cos(2pi/N (n + n0)(k + 1/2)),  n0 = (N/2+1)/2
// computed as a +/- fold to 1024 points followed by a DCT-IV via a 512-point
// complex FFT. Matches the direct formula to ~1e-13 relative (unit-tested).

#ifndef GLINT_AAC_MDCT_HPP
#define GLINT_AAC_MDCT_HPP

namespace glint {
namespace aac {

constexpr int kAacFrameLen = 1024;  // coefficients / new samples per frame

// prev/cur: the previous and current 1024-sample blocks (unwindowed).
// spec: 1024 MDCT coefficients out. Sine window applied internally.
void aac_mdct_long(const double* prev, const double* cur, double* spec);

}  // namespace aac
}  // namespace glint

#endif  // GLINT_AAC_MDCT_HPP

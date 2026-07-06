// glint - AAC forward MDCT (all four window sequences)
// MIT License - Clean-room implementation

#include "aac_mdct.hpp"

#include <cmath>
#include <cstdint>

namespace glint {
namespace aac {

namespace {

constexpr double kPi = 3.14159265358979323846;

// FFT + twiddle set for one transform size (H-point complex FFT computing a
// 2H-point DCT-IV, i.e. a 4H-point MDCT window length... see mdct_core).
template <int H, int LOG2H>
struct FftSet {
#ifdef GLINT_SMALL_BUFFERS
    using T = float;   // table storage; arithmetic stays double
#else
    using T = double;
#endif
    T tw_re[H];        // DCT-IV pre/post twiddle e^{-i pi (n + 1/8) / (2H)}
    T tw_im[H];
    T fft_re[H / 2];   // e^{-i 2pi j / H}
    T fft_im[H / 2];

    FftSet() {
        const int M = 2 * H;  // DCT-IV size
        for (int n = 0; n < H; n++) {
            double a = kPi * (n + 0.125) / M;
            tw_re[n] = static_cast<T>(std::cos(a));
            tw_im[n] = static_cast<T>(-std::sin(a));
        }
        for (int j = 0; j < H / 2; j++) {
            double a = 2.0 * kPi * j / H;
            fft_re[j] = static_cast<T>(std::cos(a));
            fft_im[j] = static_cast<T>(-std::sin(a));
        }
    }

    void fft(double* re, double* im) const {
        for (int i = 0; i < H; i++) {
            unsigned r = 0;
            for (int b = 0; b < LOG2H; b++) r = (r << 1) | ((i >> b) & 1);
            if (static_cast<int>(r) > i) {
                double t = re[i]; re[i] = re[r]; re[r] = t;
                t = im[i]; im[i] = im[r]; im[r] = t;
            }
        }
        for (int len = 2; len <= H; len <<= 1) {
            int half = len >> 1;
            int stride = H / len;
            for (int base = 0; base < H; base += len) {
                for (int j = 0; j < half; j++) {
                    double wr = fft_re[j * stride];
                    double wi = fft_im[j * stride];
                    int a = base + j, b = a + half;
                    double xr = re[b] * wr - im[b] * wi;
                    double xi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - xr;
                    im[b] = im[a] - xi;
                    re[a] += xr;
                    im[a] += xi;
                }
            }
        }
    }
};

struct Windows {
#ifdef GLINT_SMALL_BUFFERS
    using T = float;
#else
    using T = double;
#endif
    // Sine windows are symmetric (w[n] == w[N-1-n]); store the first half.
    T long_h[1024];
    T short_h[128];
    Windows() {
        for (int n = 0; n < 1024; n++) long_h[n] = static_cast<T>(std::sin(kPi / 2048 * (n + 0.5)));
        for (int n = 0; n < 128; n++) short_h[n] = static_cast<T>(std::sin(kPi / 256 * (n + 0.5)));
    }
    double long_w(int n) const { return long_h[n < 1024 ? n : 2047 - n]; }
    double short_w(int n) const { return short_h[n < 128 ? n : 255 - n]; }
};

const Windows& windows() { static const Windows w; return w; }
const FftSet<512, 9>& set1024() { static const FftSet<512, 9> s; return s; }
const FftSet<64, 6>& set128() { static const FftSet<64, 6> s; return s; }

// MDCT core: windowed input z[N] -> M = N/2 coefficients, via fold + DCT-IV.
//   u[n]     = -z[3Q-1-n] - z[3Q+n]   n = 0..Q-1   (Q = N/4)
//   u[Q+n]   =  z[n]      - z[2Q-1-n]
//   spec = 2 * DCT-IV(u)
template <int H, int LOG2H>
void mdct_core(const double* z, const FftSet<H, LOG2H>& s, SpecT* spec) {
    const int M = 2 * H;
    const int Q = H;  // N/4 == M/2 == H
    double u[2 * H];
    for (int n = 0; n < Q; n++) {
        u[n] = -z[3 * Q - 1 - n] - z[3 * Q + n];
        u[Q + n] = z[n] - z[2 * Q - 1 - n];
    }
    double re[H], im[H];
    for (int n = 0; n < H; n++) {
        double vr = u[2 * n];
        double vi = u[M - 1 - 2 * n];
        re[n] = vr * s.tw_re[n] - vi * s.tw_im[n];
        im[n] = vr * s.tw_im[n] + vi * s.tw_re[n];
    }
    s.fft(re, im);
    for (int k = 0; k < H; k++) {
        double pr = re[k] * s.tw_re[k] - im[k] * s.tw_im[k];
        double pi_ = re[k] * s.tw_im[k] + im[k] * s.tw_re[k];
        spec[2 * k] = static_cast<SpecT>(2.0 * pr);
        spec[M - 1 - 2 * k] = static_cast<SpecT>(-2.0 * pi_);
    }
}

}  // namespace

void aac_mdct_frame(int window_sequence, const PcmT* prev, const PcmT* cur,
                    SpecT* spec) {
    const Windows& w = windows();
    double x[2048];
    for (int n = 0; n < 1024; n++) {
        x[n] = static_cast<double>(prev[n]);
        x[1024 + n] = static_cast<double>(cur[n]);
    }

    if (window_sequence == kSeqShort) {
        // Eight 256-point MDCTs over the centre region [448, 1600).
        double z[256];
        for (int win = 0; win < 8; win++) {
            const double* base = x + 448 + 128 * win;
            for (int n = 0; n < 256; n++) z[n] = w.short_w(n) * base[n];
            mdct_core(z, set128(), spec + 128 * win);
        }
        return;
    }

    double z[2048];
    switch (window_sequence) {
        case kSeqStart:
            // long rise | 448 ones | short fall | 448 zeros
            for (int n = 0; n < 1024; n++) z[n] = w.long_w(n) * x[n];
            for (int n = 1024; n < 1472; n++) z[n] = x[n];
            for (int n = 0; n < 128; n++) z[1472 + n] = w.short_w(128 + n) * x[1472 + n];
            for (int n = 1600; n < 2048; n++) z[n] = 0.0;
            break;
        case kSeqStop:
            // 448 zeros | short rise | ones | long fall
            for (int n = 0; n < 448; n++) z[n] = 0.0;
            for (int n = 0; n < 128; n++) z[448 + n] = w.short_w(n) * x[448 + n];
            for (int n = 576; n < 1024; n++) z[n] = x[n];
            for (int n = 1024; n < 2048; n++) z[n] = w.long_w(n) * x[n];
            break;
        default:  // kSeqLong
            for (int n = 0; n < 2048; n++) z[n] = w.long_w(n) * x[n];
            break;
    }
    mdct_core(z, set1024(), spec);
}

}  // namespace aac
}  // namespace glint

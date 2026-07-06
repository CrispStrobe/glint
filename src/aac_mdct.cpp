// glint - AAC forward MDCT (long windows)
// MIT License - Clean-room implementation

#include "aac_mdct.hpp"

#include <cmath>

namespace glint {
namespace aac {

namespace {

constexpr int kN = 2048;          // window length
constexpr int kM = kN / 2;        // 1024 coefficients
constexpr int kQ = kN / 4;        // 512
constexpr int kH = kM / 2;        // 512-point complex FFT
constexpr int kLog2H = 9;

struct MdctTables {
    double win[kN];        // sine window
    double tw_re[kH];      // pre/post twiddle e^{-i pi (n + 1/8) / M}
    double tw_im[kH];
    double fft_re[kH / 2]; // FFT twiddles e^{-i 2pi j / len}, packed per stage
    double fft_im[kH / 2];
    unsigned short rev[kH];

    MdctTables() {
        const double pi = 3.14159265358979323846;
        for (int n = 0; n < kN; n++) {
            win[n] = std::sin(pi / kN * (n + 0.5));
        }
        for (int n = 0; n < kH; n++) {
            double a = pi * (n + 0.125) / kM;
            tw_re[n] = std::cos(a);
            tw_im[n] = -std::sin(a);
        }
        // half-size twiddles for the largest stage; smaller stages stride
        for (int j = 0; j < kH / 2; j++) {
            double a = 2.0 * pi * j / kH;
            fft_re[j] = std::cos(a);
            fft_im[j] = -std::sin(a);
        }
        for (int i = 0; i < kH; i++) {
            unsigned r = 0;
            for (int b = 0; b < kLog2H; b++) {
                r = (r << 1) | ((i >> b) & 1);
            }
            rev[i] = static_cast<unsigned short>(r);
        }
    }
};

const MdctTables& tables() {
    static const MdctTables t;
    return t;
}

// In-place radix-2 DIT FFT, size kH, twiddles from tables()
void fft512(double* re, double* im) {
    const MdctTables& t = tables();
    for (int i = 0; i < kH; i++) {
        int r = t.rev[i];
        if (r > i) {
            double tr = re[i]; re[i] = re[r]; re[r] = tr;
            double ti = im[i]; im[i] = im[r]; im[r] = ti;
        }
    }
    for (int len = 2; len <= kH; len <<= 1) {
        int half = len >> 1;
        int stride = kH / len;
        for (int base = 0; base < kH; base += len) {
            for (int j = 0; j < half; j++) {
                double wr = t.fft_re[j * stride];
                double wi = t.fft_im[j * stride];
                int a = base + j;
                int b = a + half;
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

}  // namespace

void aac_mdct_long(const double* prev, const double* cur, double* spec) {
    const MdctTables& t = tables();

    // Windowed input z = w .* [prev|cur], folded to u[0..M-1]:
    //   u[n]      = -z[3Q - 1 - n] - z[3Q + n]     n = 0..Q-1
    //   u[Q + n]  =  z[n]          - z[2Q - 1 - n]
    // (quarters: A=z[0,Q) B=z[Q,2Q) C=z[2Q,3Q) D=z[3Q,4Q))
    double u[kM];
    for (int n = 0; n < kQ; n++) {
        double zc = t.win[2 * kQ + (kQ - 1 - n)] * cur[kQ - 1 - n];        // z[3Q-1-n]
        double zd = t.win[3 * kQ + n] * cur[kQ + n];                       // z[3Q+n]
        u[n] = -zc - zd;
        double za = t.win[n] * prev[n];                                    // z[n]
        double zb = t.win[2 * kQ - 1 - n] * prev[2 * kQ - 1 - n];          // z[2Q-1-n]
        u[kQ + n] = za - zb;
    }

    // DCT-IV of u via kH-point complex FFT:
    //   v[n] = u[2n] + i u[M-1-2n], pre-twiddle, FFT, post-twiddle,
    //   Y[2k] = Re P[k], Y[M-1-2k] = -Im P[k];  spec = 2 * Y
    double re[kH], im[kH];
    for (int n = 0; n < kH; n++) {
        double vr = u[2 * n];
        double vi = u[kM - 1 - 2 * n];
        re[n] = vr * t.tw_re[n] - vi * t.tw_im[n];
        im[n] = vr * t.tw_im[n] + vi * t.tw_re[n];
    }
    fft512(re, im);
    for (int k = 0; k < kH; k++) {
        double pr = re[k] * t.tw_re[k] - im[k] * t.tw_im[k];
        double pi_ = re[k] * t.tw_im[k] + im[k] * t.tw_re[k];
        spec[2 * k] = 2.0 * pr;
        spec[kM - 1 - 2 * k] = -2.0 * pi_;
    }
}

}  // namespace aac
}  // namespace glint

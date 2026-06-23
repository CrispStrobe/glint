// glint - MDCT with sine window and aliasing reduction
// MIT License - Clean-room implementation

#include "mdct.hpp"
#include "tables.hpp"
#include <cstring>
#include <cmath>

#ifdef GLINT_FIXED_POINT
#include "fixedpoint.hpp"
#endif

namespace glint {

// === Double-precision path (always compiled) ===

static double mdct_cos_d[36][18];
static double mdct_win_d[36];
static double alias_cs_d[8], alias_ca_d[8];
static bool mdct_init = false;

static void init_mdct_d() {
    if (mdct_init) return;
    constexpr double PI = 3.14159265358979323846;
    for (int n = 0; n < 36; n++) {
        mdct_win_d[n] = std::sin(PI / 36.0 * (n + 0.5));
        for (int k = 0; k < 18; k++)
            mdct_cos_d[n][k] = std::cos(PI / 72.0 * (2.0*n + 19.0) * (2.0*k + 1.0));
    }
    static constexpr double c[8] = {-0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037};
    for (int i = 0; i < 8; i++) {
        alias_cs_d[i] = 1.0 / std::sqrt(1.0 + c[i]*c[i]);
        alias_ca_d[i] = c[i] / std::sqrt(1.0 + c[i]*c[i]);
    }
    mdct_init = true;
}

MDCT::MDCT() { reset(); init_mdct_d(); }
void MDCT::reset() { std::memset(prev_, 0, sizeof(prev_)); }

void MDCT::process(const double subband[32][18], double mdct_out[32][18]) {
    for (int sb = 0; sb < 32; sb++) {
        double x[36];
        for (int n = 0; n < 18; n++) x[n] = prev_[sb][n];
        for (int n = 0; n < 18; n++) x[n + 18] = subband[sb][n];

        for (int n = 0; n < 36; n++) x[n] *= mdct_win_d[n];

        for (int k = 0; k < 18; k++) {
            double sum = 0.0;
            for (int n = 0; n < 36; n++)
                sum += x[n] * mdct_cos_d[n][k];
            mdct_out[sb][k] = sum / 288.0;
        }

        for (int n = 0; n < 18; n++) prev_[sb][n] = subband[sb][n];
    }
}

void alias_reduce_d(double mdct_out[32][18]) {
    for (int sb = 0; sb < 31; sb++) {
        for (int i = 0; i < 8; i++) {
            double a = mdct_out[sb][17 - i];
            double b = mdct_out[sb + 1][i];
            mdct_out[sb][17 - i]  = a * alias_cs_d[i] + b * alias_ca_d[i];
            mdct_out[sb + 1][i]   = b * alias_cs_d[i] - a * alias_ca_d[i];
        }
    }
}

#ifdef GLINT_FIXED_POINT

// === Fixed-point (Q24) path ===

MDCT_FP::MDCT_FP() { reset(); tables::init_tables(); }
void MDCT_FP::reset() { std::memset(prev_, 0, sizeof(prev_)); }

void MDCT_FP::process(const int32_t subband[32][18], int32_t mdct_out[32][18]) {
    for (int sb = 0; sb < 32; sb++) {
        int32_t x[36];
        for (int n = 0; n < 18; n++) x[n] = prev_[sb][n];
        for (int n = 0; n < 18; n++) x[n + 18] = subband[sb][n];

        for (int n = 0; n < 36; n++)
            x[n] = static_cast<int32_t>((static_cast<int64_t>(x[n]) * tables::mdct_window[n]) >> 31);

        // MDCT cosine accumulation: x is Q24, mdct_cos is Q31.
        // Full product Q24*Q31 = Q55, sum of 36 terms can overflow int64.
        // Pre-shift x >> 2 to Q22: Q22*Q31 = Q53, sum of 36 fits int64.
        // Divide by 288, then >> 29 to get Q24.
        for (int k = 0; k < 18; k++) {
            int64_t sum = 0;
            for (int n = 0; n < 36; n++)
                sum += static_cast<int64_t>(x[n] >> 2) * tables::mdct_cos[n][k];
            mdct_out[sb][k] = static_cast<int32_t>((sum / 288) >> 29);
        }

        for (int n = 0; n < 18; n++) prev_[sb][n] = subband[sb][n];
    }
}

void alias_reduce_fp(int32_t mdct_out[32][18]) {
    for (int sb = 0; sb < 31; sb++) {
        for (int i = 0; i < 8; i++) {
            int32_t a = mdct_out[sb][17 - i];
            int32_t b = mdct_out[sb + 1][i];
            int32_t cs = tables::alias_cs[i];
            int32_t ca = tables::alias_ca[i];
            mdct_out[sb][17 - i] = static_cast<int32_t>((static_cast<int64_t>(a) * cs + static_cast<int64_t>(b) * ca) >> 31);
            mdct_out[sb + 1][i]  = static_cast<int32_t>((static_cast<int64_t>(b) * cs - static_cast<int64_t>(a) * ca) >> 31);
        }
    }
}

#endif // GLINT_FIXED_POINT

} // namespace glint

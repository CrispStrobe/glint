// glint - MDCT (Modified Discrete Cosine Transform)
// MIT License - Clean-room implementation

#ifndef GLINT_MDCT_HPP
#define GLINT_MDCT_HPP

#include <cstdint>

namespace glint {

// Double-precision MDCT (always available)
class MDCT {
public:
    MDCT();
    void process(const double subband[32][18], double mdct_out[32][18]);
    void process_short(const double subband[32][18], double mdct_out[32][3][6]);
    void reset();
private:
    double prev_[32][18];
};

void alias_reduce_d(double mdct_out[32][18]);

#ifdef GLINT_FIXED_POINT
// Fixed-point (Q24) MDCT
class MDCT_FP {
public:
    MDCT_FP();
    void process(const int32_t subband[32][18], int32_t mdct_out[32][18]);
    void reset();
private:
    int32_t prev_[32][18];
};

void alias_reduce_fp(int32_t mdct_out[32][18]);
#endif

} // namespace glint

#endif

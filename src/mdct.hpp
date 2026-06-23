// glint - MDCT (Modified Discrete Cosine Transform)
// MIT License - Clean-room implementation

#ifndef GLINT_MDCT_HPP
#define GLINT_MDCT_HPP

#include <cstdint>

namespace glint {

class MDCT {
public:
    MDCT();
    void process(const double subband[32][18], double mdct_out[32][18]);
    void reset();
private:
    double prev_[32][18];
};

void alias_reduce_d(double mdct_out[32][18]);

} // namespace glint

#endif

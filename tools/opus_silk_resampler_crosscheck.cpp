// Cross-check driver for the SILK decode-side resampler (PLAN § O2).
// Compiled twice by tools/crosscheck_opus_silk_resampler.py. Fuzz oracle:
// for every decoder-reachable (fs_in, fs_out) pair, a long chained stream
// of blocks (random noise, saturating steps, triangle sweeps; frame-sized
// and odd lengths) runs through both implementations; state carries across
// blocks and the printed output must be BYTE-IDENTICAL. The out buffer is
// zeroed before each call and printed with slack past the nominal output
// count, so any extra/missing writes diverge too.

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef USE_LIBOPUS
extern "C" {
#include "SigProc_FIX.h"
}
struct SideA {
    silk_resampler_state_struct S;
    int init(int in_khz, int out_khz) {
        return silk_resampler_init(&S, in_khz * 1000, out_khz * 1000, 0);
    }
    void proc(int16_t* out, const int16_t* in, int n) {
        silk_resampler(&S, out, in, n);
    }
};
#else
#include "opus_silk_resampler.hpp"
struct SideA {
    glint::opus::silk::Resampler S;
    int init(int in_khz, int out_khz) { return S.init(in_khz, out_khz); }
    void proc(int16_t* out, const int16_t* in, int n) {
        S.process(out, in, n);
    }
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

int main() {
    // All 15 decoder-reachable pairs: internal {8,12,16} -> API rate
    // {8,12,16,24,48} (copy, up2_HQ, IIR_FIR and down_FIR kernels).
    static const int kFsIn[3] = { 8, 12, 16 };
    static const int kFsOut[5] = { 8, 12, 16, 24, 48 };
    static int16_t inbuf[1024];
    static int16_t outbuf[8192];

    for (int ii = 0; ii < 3; ii++) {
        for (int oo = 0; oo < 5; oo++) {
            const int fi = kFsIn[ii], fo = kFsOut[oo];
            rng_state = 0x9E3779B9u ^ (uint32_t)(fi * 100 + fo);

            SideA a;
            int rc = a.init(fi, fo);
            std::printf("pair %d %d init %d\n", fi, fo, rc);
            if (rc != 0) continue;

            uint32_t phase = 0, freq = xrand() & 0x3FFF;  // sweep state
            for (int blk = 0; blk < 220; blk++) {
                // Block length: mostly whole 10/20 ms frames (what the
                // decoder sends), plus arbitrary odd lengths >= 1 ms
                // (the only constraint silk_resampler imposes).
                int in_len;
                switch (xrand() % 4) {
                    case 0:  in_len = fi * 10; break;
                    case 1:  in_len = fi + (int)(xrand() % (uint32_t)(fi * 40)); break;
                    default: in_len = fi * 20; break;
                }

                // Signal: noise / saturating step / triangle sweep.
                int kind = blk % 3;
                int32_t amp = 1000 + (int32_t)(xrand() % 31768);
                if ((xrand() & 7) == 0) amp = 32767;  // stress SAT16
                int period = 1 + (int)(xrand() % 64);
                for (int i = 0; i < in_len; i++) {
                    if (kind == 0) {
                        inbuf[i] = (int16_t)xrand();
                    } else if (kind == 1) {
                        inbuf[i] = (int16_t)(((i / period) & 1) ? amp : -amp);
                    } else {
                        phase += freq;
                        freq += 7;  // chirp across chained blocks
                        uint32_t p16 = (phase >> 16) & 0xFFFF;
                        int32_t tri = p16 < 32768 ? (int32_t)p16
                                                  : 65535 - (int32_t)p16;
                        inbuf[i] = (int16_t)(tri - 16384);
                    }
                }

                std::memset(outbuf, 0, sizeof(outbuf));
                a.proc(outbuf, inbuf, in_len);

                // Nominal output count + slack (odd in_len can produce a
                // few extra samples; zeros beyond compare equal).
                int n_print = in_len * fo / fi + 32;
                std::printf("b %d n %d out", blk, in_len);
                for (int i = 0; i < n_print; i++)
                    std::printf(" %d", outbuf[i]);
                std::printf("\n");
            }
        }
    }
    return 0;
}

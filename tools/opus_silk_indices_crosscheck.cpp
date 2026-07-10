// Cross-check driver for SILK side-info decoding + gains dequant + pitch
// lags (PLAN § O2). Compiled twice by tools/crosscheck_opus_silk_indices.py.
// Fuzz oracle: identical random streams through decode_indices (state
// carried across frames within a sequence), then gains_dequant and
// decode_pitch on the decoded indices. All integers — byte-identical.

#include <cstdint>
#include <cstdio>

#ifdef USE_LIBOPUS
extern "C" {
#include "main.h"
#include "entdec.h"
}
struct SideA {
    silk_decoder_state st;
    ec_dec d;
    void init(int fs_khz, int nb_subfr) {
        silk_init_decoder(&st);
        st.nb_subfr = nb_subfr;
        silk_decoder_set_fs(&st, fs_khz, 48000);
    }
    void stream(uint8_t* b, uint32_t n) { ec_dec_init(&d, b, n); }
    void indices(int frame_index, int vad, int lbrr, int cond) {
        st.VAD_flags[frame_index] = vad;
        silk_decode_indices(&st, &d, frame_index, lbrr, cond);
    }
    void print_indices() {
        SideInfoIndices* x = &st.indices;
        std::printf("st %d qo %d nic %d lag %d ci %d per %d lts %d seed %d",
                    x->signalType, x->quantOffsetType, x->NLSFInterpCoef_Q2,
                    x->lagIndex, x->contourIndex, x->PERIndex,
                    x->LTP_scaleIndex, x->Seed);
        std::printf(" g");
        for (int i = 0; i < st.nb_subfr; i++)
            std::printf(" %d", x->GainsIndices[i]);
        std::printf(" nlsf");
        for (int i = 0; i <= st.psNLSF_CB->order; i++)
            std::printf(" %d", x->NLSFIndices[i]);
        std::printf(" ltp");
        for (int i = 0; i < st.nb_subfr; i++)
            std::printf(" %d", x->LTPIndex[i]);
    }
    opus_int8 prev_gain = 0;
    void gains_pitch() {
        opus_int32 g[4];
        silk_gains_dequant(g, st.indices.GainsIndices, &prev_gain, 0,
                           st.nb_subfr);
        std::printf(" gains");
        for (int i = 0; i < st.nb_subfr; i++) std::printf(" %d", g[i]);
        if (st.indices.signalType == TYPE_VOICED) {
            opus_int lags[4];
            silk_decode_pitch(st.indices.lagIndex, st.indices.contourIndex,
                              lags, st.fs_kHz, st.nb_subfr);
            std::printf(" lags");
            for (int i = 0; i < st.nb_subfr; i++)
                std::printf(" %d", lags[i]);
        }
    }
    uint32_t tell() { return (uint32_t)ec_tell(&d); }
};
#else
#include "opus_silk_indices.hpp"
using namespace glint::opus::silk;
struct SideA {
    DecoderState st;
    glint::opus::RangeDecoder d;
    int8_t prev_gain = 0;
    void init(int fs_khz, int nb_subfr) { st.set_fs(fs_khz, nb_subfr); }
    void stream(const uint8_t* b, uint32_t n) { d.init(b, n); }
    void indices(int frame_index, int vad, int lbrr, int cond) {
        st.vad_flags[frame_index] = vad;
        decode_indices(&st, d, frame_index, lbrr != 0, cond);
    }
    void print_indices() {
        SideInfoIndices* x = &st.indices;
        std::printf("st %d qo %d nic %d lag %d ci %d per %d lts %d seed %d",
                    x->signal_type, x->quant_offset_type,
                    x->nlsf_interp_coef_q2, x->lag_index, x->contour_index,
                    x->per_index, x->ltp_scale_index, x->seed);
        std::printf(" g");
        for (int i = 0; i < st.nb_subfr; i++)
            std::printf(" %d", x->gains_indices[i]);
        std::printf(" nlsf");
        for (int i = 0; i <= st.nlsf_cb->order; i++)
            std::printf(" %d", x->nlsf_indices[i]);
        std::printf(" ltp");
        for (int i = 0; i < st.nb_subfr; i++)
            std::printf(" %d", x->ltp_index[i]);
    }
    void gains_pitch() {
        int32_t g[4];
        gains_dequant(g, st.indices.gains_indices, &prev_gain, 0,
                      st.nb_subfr);
        std::printf(" gains");
        for (int i = 0; i < st.nb_subfr; i++) std::printf(" %d", g[i]);
        if (st.indices.signal_type == kTypeVoiced) {
            int lags[4];
            decode_pitch(st.indices.lag_index, st.indices.contour_index,
                         lags, st.fs_khz, st.nb_subfr);
            std::printf(" lags");
            for (int i = 0; i < st.nb_subfr; i++)
                std::printf(" %d", lags[i]);
        }
    }
    uint32_t tell() { return d.tell(); }
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
    static uint8_t buf[512];
    static const int kFs[3] = { 8, 12, 16 };
    for (uint32_t seed = 1; seed <= 400; seed++) {
        rng_state = seed;
        int fs = kFs[xrand() % 3];
        int nsf = (xrand() & 1) ? 4 : 2;
        SideA a;
        a.init(fs, nsf);
        uint32_t len = 8 + xrand() % 200;
        for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)xrand();
        a.stream(buf, len);
        std::printf("seed %u fs %d nsf %d\n", seed, fs, nsf);
        // Several frames on one stream: exercises conditional coding and
        // the prev-lag/type chaining.
        for (int f = 0; f < 3; f++) {
            int vad = (int)(xrand() & 1);
            int lbrr = (int)(xrand() % 4 == 0);
            int cond = (int)(xrand() % 3);
            a.indices(0, vad, lbrr, cond);
            std::printf("f %d vad %d lbrr %d cond %d: ", f, vad, lbrr,
                        cond);
            a.print_indices();
            a.gains_pitch();
            std::printf(" tell %u\n", a.tell());
        }
    }
    return 0;
}

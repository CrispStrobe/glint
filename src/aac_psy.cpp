// glint - AAC psychoacoustic band masks
// MIT License - Clean-room implementation

#include "aac_psy.hpp"
#include "aac_coder.hpp"
#include "aac_tables.hpp"

#include <cmath>

namespace glint {
namespace aac {

using namespace aac_tables;

namespace {

struct MaskModel {
#ifdef GLINT_SMALL_BUFFERS
    using T = float;   // table storage; arithmetic stays double
#else
    using T = double;
#endif
    int nb;
    T spread[kMaxSfb][kMaxSfb];  // energy-domain Schroeder spreading
    T ath_rel[kMaxSfb];          // 10^((ath_db - ath_min - 96)/10)
};

// Single-slot cache keyed by sample rate (embedded-friendly; matches the
// GLINT_SMALL_BUFFERS pattern used by the MP3 mask models).
const MaskModel* get_model(int sr_index) {
    static MaskModel model;
    static int slot_sr = -1;
    if (slot_sr == sr_index) return &model;

    const uint16_t* swb = kSwbOffsetLong[sr_index];
    const int nb = kNumSwbLong[sr_index];
    const double sr = kSampleRates[sr_index];

    double z[kMaxSfb];
    double ath_db[kMaxSfb];
    double ath_min = 1e30;
    for (int b = 0; b < nb; b++) {
        double fc = 0.5 * (swb[b] + swb[b + 1]) * (sr / 2.0) / 1024.0;
        if (fc < 20.0) fc = 20.0;
        z[b] = 13.0 * std::atan(0.00076 * fc) +
               3.5 * std::atan((fc / 7500.0) * (fc / 7500.0));
        double khz = fc / 1000.0;
        ath_db[b] = 3.64 * std::pow(khz, -0.8) -
                    6.5 * std::exp(-0.6 * (khz - 3.3) * (khz - 3.3)) +
                    1e-3 * khz * khz * khz * khz;
        if (ath_db[b] < ath_min) ath_min = ath_db[b];
    }
    for (int b = 0; b < nb; b++) {
        model.ath_rel[b] = static_cast<MaskModel::T>(
            std::pow(10.0, (ath_db[b] - ath_min - 96.0) / 10.0));
        for (int j = 0; j < nb; j++) {
            double dz = z[b] - z[j];
            double s_db = 15.81 + 7.5 * (dz + 0.474) -
                          17.5 * std::sqrt(1.0 + (dz + 0.474) * (dz + 0.474));
            model.spread[b][j] = static_cast<MaskModel::T>(std::pow(10.0, s_db / 10.0));
        }
    }
    model.nb = nb;
    slot_sr = sr_index;
    return &model;
}

}  // namespace

double aac_compute_masks(const SpecT* spec, int sr_index, int max_sfb,
                         double emax_ref, double* mask, bool tonal) {
    const MaskModel* m = get_model(sr_index);
    const uint16_t* swb = kSwbOffsetLong[sr_index];
    static const double kOffset = std::pow(10.0, -14.0 / 10.0);

    double e[kMaxSfb];
    double off[kMaxSfb];
    double emax = 0.0;
    const int nb = max_sfb < m->nb ? max_sfb : m->nb;
    for (int b = 0; b < nb; b++) {
        double acc = 0.0, lg = 0.0;
        for (int i = swb[b]; i < swb[b + 1]; i++) {
            double p = spec[i] * spec[i];
            acc += p;
            if (tonal) lg += std::log(p + 1e-30);
        }
        e[b] = acc;
        if (acc > emax) emax = acc;
        if (tonal) {
            // Spectral flatness -> tonality alpha in [0,1]; tonal maskers
            // mask less: per-masker offset -(6 + 12*alpha) dB.
            int n = swb[b + 1] - swb[b];
            double gm = std::exp(lg / n);
            double sfm_db = 10.0 * std::log10(gm / (acc / n + 1e-30) + 1e-30);
            double a = sfm_db / -20.0;
            if (a > 1.0) a = 1.0;
            if (a < 0.0) a = 0.0;
            off[b] = std::pow(10.0, -(6.0 + 12.0 * a) / 10.0);
        }
    }
    double ref = emax_ref > emax ? emax_ref : emax;
    for (int b = 0; b < nb; b++) {
        double acc = 0.0;
        if (tonal) {
            for (int j = 0; j < nb; j++) acc += e[j] * off[j] * m->spread[b][j];
        } else {
            for (int j = 0; j < nb; j++) acc += e[j] * m->spread[b][j];
            acc *= kOffset;
        }
        double floor = ref * m->ath_rel[b];
        mask[b] = acc > floor ? acc : floor;
    }
    return emax;
}

}  // namespace aac
}  // namespace glint

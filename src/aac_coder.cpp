// glint - AAC-LC quantization, sectioning and noiseless coding
// MIT License - Clean-room implementation

#include "aac_coder.hpp"
#include "aac_tables.hpp"

#include <cmath>
#include <cstring>

namespace glint {
namespace aac {

using namespace aac_tables;

namespace {

constexpr int kInf = 1 << 28;
constexpr int kSectHdrBits = 4 + 5;  // sect_cb + first sect_len_incr (long windows)

// Code (or just count, bw == nullptr) lines [start,end) with spectral book
// 1..11. Returns bit cost, or -1 if the book cannot represent the values.
int code_band(AacBitWriter* bw, int book, const int16_t* ix, int start, int end) {
    const int dim = kBookDim[book - 1];
    const int lav = kBookLav[book - 1];
    const bool sgn = kBookSigned[book - 1] != 0;
    const uint8_t* blen = kSpecBits[book - 1];
    const uint16_t* bcode = kSpecCodes[book - 1];

    int bits = 0;
    for (int i = start; i < end; i += dim) {
        int idx = 0;
        int sbits = 0;
        uint32_t sword = 0;
        int esc[2];
        int nesc = 0;
        for (int j = 0; j < dim; j++) {
            int v = ix[i + j];
            if (sgn) {
                if (v < -lav || v > lav) return -1;
                idx = idx * (2 * lav + 1) + (v + lav);
            } else {
                int a = v < 0 ? -v : v;
                if (book == 11) {
                    if (a > kMaxQuant) return -1;
                    idx = idx * 17 + (a >= 16 ? 16 : a);
                    if (a >= 16) esc[nesc++] = a;
                } else {
                    if (a > lav) return -1;
                    idx = idx * (lav + 1) + a;
                }
                if (a != 0) {
                    sword = (sword << 1) | (v < 0 ? 1u : 0u);
                    sbits++;
                }
            }
        }
        bits += blen[idx] + sbits;
        if (bw) {
            bw->put(bcode[idx], blen[idx]);
            if (sbits) bw->put(sword, sbits);
        }
        for (int e = 0; e < nesc; e++) {
            int a = esc[e];
            int n1 = 0;
            while (a >> (n1 + 5)) n1++;  // 2^(n1+4) <= a < 2^(n1+5)
            bits += (n1 + 1) + (n1 + 4);
            if (bw) {
                bw->put((1u << (n1 + 1)) - 2, n1 + 1);  // n1 ones, then 0
                bw->put(static_cast<uint32_t>(a) - (1u << (n1 + 4)), n1 + 4);
            }
        }
    }
    return bits;
}

}  // namespace

int aac_quantize(const double* p34, const double* spec, const uint16_t* swb,
                 int max_sfb, const uint8_t* sf, int16_t* ix) {
    int maxabs = 0;
    for (int b = 0; b < max_sfb; b++) {
        const double step = std::pow(2.0, -0.1875 * (sf[b] - kSfOffset));
        for (int i = swb[b]; i < swb[b + 1]; i++) {
            double q = p34[i] * step + 0.4054;
            int a = (q > 32000.0) ? 32000 : static_cast<int>(q);
            if (a > maxabs) maxabs = a;
            ix[i] = static_cast<int16_t>(spec[i] < 0 ? -a : a);
        }
    }
    return maxabs;
}

void aac_band_noise(const AacChannelPlan& plan, const double* spec,
                    int sr_index, double* noise) {
    const uint16_t* swb = kSwbOffsetLong[sr_index];
    for (int b = 0; b < plan.max_sfb; b++) {
        const double gain = std::pow(2.0, 0.25 * (plan.sf[b] - kSfOffset));
        double acc = 0.0;
        for (int i = swb[b]; i < swb[b + 1]; i++) {
            double xhat = 0.0;
            int a = plan.ix[i] < 0 ? -plan.ix[i] : plan.ix[i];
            if (a != 0) {
                double a43 = static_cast<double>(a) * std::cbrt(static_cast<double>(a));
                xhat = (plan.ix[i] < 0 ? -a43 : a43) * gain;
            }
            double err = spec[i] - xhat;
            acc += err * err;
        }
        noise[b] = acc;
    }
}

void aac_section_and_count(const int16_t* ix, int sr_index, AacChannelPlan* plan) {
    const uint16_t* swb = kSwbOffsetLong[sr_index];
    const int nb = plan->max_sfb;
    const int scf_zero_bits = kScfBits[60];  // dpcm 0 (flat scalefactors)

    // Per-band cost with each candidate book (0 = zero band, transmits nothing).
    int cost[kMaxSfb][12];
    for (int b = 0; b < nb; b++) {
        int start = swb[b];
        int end = swb[b + 1];
        int maxabs = 0;
        for (int i = start; i < end; i++) {
            int a = ix[i] < 0 ? -ix[i] : ix[i];
            if (a > maxabs) maxabs = a;
        }
        cost[b][0] = (maxabs == 0) ? 0 : kInf;
        for (int cb = 1; cb <= 11; cb++) {
            int c = (maxabs == 0) ? code_band(nullptr, cb, ix, start, end)
                    : (cb < 11 && maxabs > kBookLav[cb - 1])
                        ? -1
                        : code_band(nullptr, cb, ix, start, end);
            cost[b][cb] = (c < 0) ? kInf : c + scf_zero_bits;
        }
    }

    // Optimal sectioning: dp[b][cb] = min bits for bands 0..b with band b using
    // cb; changing books (or starting) costs a section header. Section-length
    // escapes for runs > 30 are ignored here and picked up by the exact count.
    int dp[kMaxSfb][12];
    uint8_t from[kMaxSfb][12];
    for (int cb = 0; cb <= 11; cb++) {
        dp[0][cb] = cost[0][cb] >= kInf ? kInf : kSectHdrBits + cost[0][cb];
        from[0][cb] = 12;
    }
    for (int b = 1; b < nb; b++) {
        int prev_best = kInf, prev_arg = 0;
        for (int cb = 0; cb <= 11; cb++) {
            if (dp[b - 1][cb] < prev_best) {
                prev_best = dp[b - 1][cb];
                prev_arg = cb;
            }
        }
        for (int cb = 0; cb <= 11; cb++) {
            if (cost[b][cb] >= kInf) {
                dp[b][cb] = kInf;
                from[b][cb] = 12;
                continue;
            }
            int cont = dp[b - 1][cb];                 // extend current section
            int fresh = prev_best + kSectHdrBits;     // start a new section
            if (cont <= fresh) {
                dp[b][cb] = cont + cost[b][cb];
                from[b][cb] = static_cast<uint8_t>(cb);
            } else {
                dp[b][cb] = fresh + cost[b][cb];
                from[b][cb] = static_cast<uint8_t>(prev_arg);
            }
        }
    }
    int best = kInf, arg = 0;
    for (int cb = 0; cb <= 11; cb++) {
        if (dp[nb - 1][cb] < best) {
            best = dp[nb - 1][cb];
            arg = cb;
        }
    }
    for (int b = nb - 1; b >= 0; b--) {
        plan->book[b] = static_cast<uint8_t>(arg);
        arg = (from[b][arg] == 12) ? arg : from[b][arg];
    }

    // Wire global_gain = scalefactor of the first coded band (its dpcm is 0).
    // With no coded band the value is irrelevant to reconstruction; keep the
    // caller's anchor so the field stays in range.
    for (int b = 0; b < nb; b++) {
        if (plan->book[b] != 0) {
            plan->global_gain = plan->sf[b];
            break;
        }
    }

    // Exact ICS bit count, excl. ics_info (includes section-length escapes).
    AacBitWriter counter(0);
    aac_write_ics_body(counter, *plan, sr_index, false);
    plan->ics_bits = counter.bits();
}

namespace {

// Evaluate anchor gain G: quantize with sf[b] = clamp(G - off[b]), section,
// exact-count. Returns true when magnitudes and the bit budget both fit.
bool eval_gain(const double* p34, const double* spec, int sr_index,
               int budget_bits, const int* off, int gain,
               AacChannelPlan* trial) {
    const uint16_t* swb = kSwbOffsetLong[sr_index];
    for (int b = 0; b < trial->max_sfb; b++) {
        int s = gain - (off ? off[b] : 0);
        if (s < 0) s = 0;
        if (s > 255) s = 255;
        trial->sf[b] = static_cast<uint8_t>(s);
    }
    trial->global_gain = gain;  // fallback; section_and_count overrides
    trial->fit_gain = gain;
    int maxabs = aac_quantize(p34, spec, swb, trial->max_sfb, trial->sf, trial->ix);
    if (maxabs > kMaxQuant) return false;
    aac_section_and_count(trial->ix, sr_index, trial);
    return trial->ics_bits <= budget_bits;
}

}  // namespace

void aac_fit_channel(const double* spec, int sr_index, int max_sfb,
                     int budget_bits, const int* sf_offsets, int gain_hint,
                     AacChannelPlan* plan) {
    const uint16_t* swb = kSwbOffsetLong[sr_index];
    plan->max_sfb = max_sfb;
    plan->num_lines = swb[max_sfb];
    const int n = plan->num_lines;

    double p34[1024];
    for (int i = 0; i < n; i++) {
        p34[i] = std::pow(std::fabs(spec[i]), 0.75);
    }
    std::memset(plan->ix, 0, sizeof(plan->ix));

    AacChannelPlan trial;
    trial.max_sfb = max_sfb;
    trial.num_lines = n;
    std::memset(trial.ix, 0, sizeof(trial.ix));

    bool have_fit = false;
    if (gain_hint >= 0) {
        // Local walk from the hint: bits(G) is (near-)monotone decreasing in
        // G, and shaping moves the answer by only a step or two per iteration.
        int g = gain_hint;
        if (eval_gain(p34, spec, sr_index, budget_bits, sf_offsets, g, &trial)) {
            *plan = trial;
            have_fit = true;
            while (g > 0 &&
                   eval_gain(p34, spec, sr_index, budget_bits, sf_offsets, g - 1, &trial)) {
                g--;
                *plan = trial;
            }
        } else {
            while (g < 255) {
                g++;
                if (eval_gain(p34, spec, sr_index, budget_bits, sf_offsets, g, &trial)) {
                    *plan = trial;
                    have_fit = true;
                    break;
                }
            }
        }
    } else {
        int lo = 0, hi = 255;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (eval_gain(p34, spec, sr_index, budget_bits, sf_offsets, mid, &trial)) {
                *plan = trial;
                have_fit = true;
                hi = mid - 1;
            } else {
                lo = mid + 1;
            }
        }
    }
    if (!have_fit) {
        // Unreachable for int16-derived spectra (gain 255 zeroes everything),
        // but keep the guarantee: take the coarsest anchor as-is.
        eval_gain(p34, spec, sr_index, budget_bits, sf_offsets, 255, plan);
    }
}

void aac_write_ics_info(AacBitWriter& bw, int max_sfb) {
    bw.put(0, 1);        // ics_reserved_bit
    bw.put(0, 2);        // window_sequence = ONLY_LONG_SEQUENCE
    bw.put(0, 1);        // window_shape = sine
    bw.put(max_sfb, 6);
    bw.put(0, 1);        // predictor_data_present (none in LC)
}

void aac_write_ics_body(AacBitWriter& bw, const AacChannelPlan& plan, int sr_index,
                        bool include_ics_info) {
    const uint16_t* swb = kSwbOffsetLong[sr_index];
    const int nb = plan.max_sfb;

    bw.put(plan.global_gain, 8);
    if (include_ics_info) {
        aac_write_ics_info(bw, plan.max_sfb);
    }

    // section_data
    for (int b = 0; b < nb;) {
        int cb = plan.book[b];
        int e = b + 1;
        while (e < nb && plan.book[e] == cb) e++;
        int len = e - b;
        bw.put(cb, 4);
        while (len >= 31) {
            bw.put(31, 5);
            len -= 31;
        }
        bw.put(len, 5);
        b = e;
    }

    // scale_factor_data: dpcm chain over coded bands, starting at global_gain
    // (zero-book bands transmit nothing and do not advance the chain).
    {
        int last = plan.global_gain;
        for (int b = 0; b < nb; b++) {
            if (plan.book[b] == 0) continue;
            int dpcm = plan.sf[b] - last + 60;
            if (dpcm < 0) dpcm = 0;          // unreachable if offsets <= 60
            if (dpcm > 120) dpcm = 120;
            bw.put(kScfCodes[dpcm], kScfBits[dpcm]);
            last = last + (dpcm - 60);
        }
    }

    bw.put(0, 1);  // pulse_data_present
    bw.put(0, 1);  // tns_data_present
    bw.put(0, 1);  // gain_control_data_present

    // spectral_data
    for (int b = 0; b < nb; b++) {
        if (plan.book[b] != 0) {
            code_band(&bw, plan.book[b], plan.ix, swb[b], swb[b + 1]);
        }
    }
}

}  // namespace aac
}  // namespace glint

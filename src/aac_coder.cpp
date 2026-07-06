// glint - AAC-LC quantization, sectioning and noiseless coding
// MIT License - Clean-room implementation

#include "aac_coder.hpp"
#include "aac_mdct.hpp"
#include "aac_tables.hpp"

#include <cmath>
#include <cstring>

namespace glint {
namespace aac {

using namespace aac_tables;

namespace {

constexpr int kInf = 1 << 28;

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

void aac_make_layout(int sr_index, int window_sequence, int max_sfb,
                     const uint8_t* group_len, int num_groups,
                     AacBandLayout* L) {
    L->window_sequence = static_cast<uint8_t>(window_sequence);
    L->max_sfb = static_cast<uint8_t>(max_sfb);
    if (window_sequence == kSeqShort) {
        const uint16_t* swb = kSwbOffsetShort[sr_index];
        L->num_groups = static_cast<uint8_t>(num_groups);
        for (int g = 0; g < num_groups; g++) L->group_len[g] = group_len[g];
        L->sect_bits = 3;
        L->sect_esc = 7;
        int nb = 0, off = 0;
        L->offset[0] = 0;
        for (int g = 0; g < num_groups; g++) {
            for (int b = 0; b < max_sfb; b++) {
                off += (swb[b + 1] - swb[b]) * group_len[g];
                L->group_of_band[nb] = static_cast<uint8_t>(g);
                L->offset[++nb] = static_cast<uint16_t>(off);
            }
        }
        L->num_bands = nb;
        L->num_lines = off;
    } else {
        const uint16_t* swb = kSwbOffsetLong[sr_index];
        L->num_groups = 1;
        L->group_len[0] = 1;
        L->sect_bits = 5;
        L->sect_esc = 31;
        for (int b = 0; b <= max_sfb; b++) L->offset[b] = swb[b];
        for (int b = 0; b < max_sfb; b++) L->group_of_band[b] = 0;
        L->num_bands = max_sfb;
        L->num_lines = swb[max_sfb];
    }
}

void aac_reorder_short(const SpecT* natural, const AacBandLayout& L,
                       int sr_index, SpecT* coded) {
    const uint16_t* swb = kSwbOffsetShort[sr_index];
    int k = 0;
    int wbase = 0;
    for (int g = 0; g < L.num_groups; g++) {
        for (int b = 0; b < L.max_sfb; b++) {
            for (int w = 0; w < L.group_len[g]; w++) {
                const SpecT* src = natural + 128 * (wbase + w);
                for (int i = swb[b]; i < swb[b + 1]; i++) coded[k++] = src[i];
            }
        }
        wbase += L.group_len[g];
    }
    while (k < 1024) coded[k++] = SpecT(0);
}

#ifdef GLINT_AAC_INT

namespace {

// log2 / exp2 in Q16, 128-segment LUTs with linear interpolation.
// Relative error ~1e-6: the quantizer's decision boundaries move by less
// than the double path's own rounding for all realistic spectra.
struct PowLuts {
    int32_t log2q16[130];   // log2(1 + i/128) * 65536
    int32_t exp2q15[130];   // 2^(i/128) * 32768
    PowLuts() {
        for (int i = 0; i <= 129; i++) {
            double f = i / 128.0;
            if (i < 130) {
                log2q16[i] = static_cast<int32_t>(std::lround(std::log2(1.0 + f) * 65536.0));
                exp2q15[i] = static_cast<int32_t>(std::lround(std::pow(2.0, f) * 32768.0));
            }
        }
    }
};
const PowLuts& luts() { static const PowLuts t; return t; }

// Q16 log2 of v (v >= 1)
inline int32_t ilog2_q16(uint32_t v) {
    const PowLuts& t = luts();
    int b = 31 - __builtin_clz(v);
    uint32_t u = v << (31 - b);           // MSB at bit 31
    int idx = (u >> 24) & 0x7F;           // 7 bits below the MSB
    int r = (u >> 16) & 0xFF;             // next 8 bits for interpolation
    int32_t lo = t.log2q16[idx];
    int32_t hi = t.log2q16[idx + 1];
    return (b << 16) + lo + static_cast<int32_t>((static_cast<int64_t>(hi - lo) * r) >> 8);
}

// floor(2^(l/65536) + 0.4054), clamped to 32000. l may be negative.
inline int quant_exp2(int64_t l) {
    const PowLuts& t = luts();
    int64_t I = l >> 16;                  // floor
    int f = static_cast<int>(l & 0xFFFF);
    if (I >= 15) return 32000;
    int idx = (f >> 9) & 0x7F;
    int r = (f >> 1) & 0xFF;
    int32_t lo = t.exp2q15[idx];
    int32_t hi = t.exp2q15[idx + 1];
    int64_t m = lo + ((static_cast<int64_t>(hi - lo) * r) >> 8);  // Q15, [1,2)
    // q in Q16 = m * 2^(I+1); add 0.4054 (Q16) and floor
    int sh = static_cast<int>(I) + 1;
    int64_t q16;
    if (sh >= 0) {
        q16 = m << sh;
    } else {
        q16 = m >> (-sh);
    }
    return static_cast<int>((q16 + 26573) >> 16);   // 0.4054 * 65536 = 26572.6
}

}  // namespace

int aac_quantize(const P34T* p34, const SpecT* spec, const AacBandLayout& L,
                 const uint8_t* sf, int16_t* ix) {
    int maxabs = 0;
    for (int b = 0; b < L.num_bands; b++) {
        // log2(step) = -0.1875 * (sf - 100); 0.1875 * 65536 = 12288 exactly
        const int32_t lstep = -12288 * (sf[b] - kSfOffset);
        for (int i = L.offset[b]; i < L.offset[b + 1]; i++) {
            int a = 0;
            if (p34[i] != INT32_MIN) {
                a = quant_exp2(static_cast<int64_t>(p34[i]) + lstep);
                if (a > 32000) a = 32000;
            }
            if (a > maxabs) maxabs = a;
            ix[i] = static_cast<int16_t>(spec[i] < 0 ? -a : a);
        }
    }
    return maxabs;
}

#else  // !GLINT_AAC_INT

int aac_quantize(const P34T* p34, const SpecT* spec, const AacBandLayout& L,
                 const uint8_t* sf, int16_t* ix) {
    int maxabs = 0;
    for (int b = 0; b < L.num_bands; b++) {
        const double step = std::pow(2.0, -0.1875 * (sf[b] - kSfOffset));
        for (int i = L.offset[b]; i < L.offset[b + 1]; i++) {
            double q = p34[i] * step + 0.4054;
            int a = (q > 32000.0) ? 32000 : static_cast<int>(q);
            if (a > maxabs) maxabs = a;
            ix[i] = static_cast<int16_t>(spec[i] < 0 ? -a : a);
        }
    }
    return maxabs;
}

#endif  // GLINT_AAC_INT

void aac_band_noise(const AacChannelPlan& plan, const SpecT* spec,
                    const AacBandLayout& L, double* noise) {
    for (int b = 0; b < L.num_bands; b++) {
        // dequant into the STORED spec domain (Q(kSpecFracBits) on the
        // integer profile) so noise and masks share one domain.
        const double gain = std::pow(2.0, 0.25 * (plan.sf[b] - kSfOffset)) *
                            (1 << kSpecFracBits);
        double acc = 0.0;
        for (int i = L.offset[b]; i < L.offset[b + 1]; i++) {
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

void aac_section_and_count(const int16_t* ix, const AacBandLayout& L,
                           AacChannelPlan* plan) {
    const int nb = L.num_bands;
    const int scf_zero_bits = kScfBits[60];  // dpcm 0 (dominant case)

    // Per-band cost with each candidate book (0 = zero band, transmits nothing).
    int cost[kMaxSfb][12];
    for (int b = 0; b < nb; b++) {
        int start = L.offset[b];
        int end = L.offset[b + 1];
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
    // cb; changing books (or starting, or crossing a group boundary — sections
    // never span groups) costs a section header. Section-length escapes for
    // long runs are ignored here and picked up by the exact count.
    const int hdr = 4 + L.sect_bits;
    int dp[kMaxSfb][12];
    uint8_t from[kMaxSfb][12];
    for (int cb = 0; cb <= 11; cb++) {
        dp[0][cb] = cost[0][cb] >= kInf ? kInf : hdr + cost[0][cb];
        from[0][cb] = 12;
    }
    for (int b = 1; b < nb; b++) {
        bool boundary = L.group_of_band[b] != L.group_of_band[b - 1];
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
            int cont = boundary ? kInf : dp[b - 1][cb];  // extend current section
            int fresh = prev_best + hdr;                 // start a new section
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
    aac_write_ics_body(counter, *plan, L, false);
    plan->ics_bits = counter.bits();
}

namespace {

// Evaluate anchor gain G: quantize with sf[b] = clamp(G - off[b]), section,
// exact-count. Returns true when magnitudes and the bit budget both fit.
bool eval_gain(const P34T* p34, const SpecT* spec, const AacBandLayout& L,
               int budget_bits, const int* off, int gain,
               AacChannelPlan* trial) {
    for (int b = 0; b < L.num_bands; b++) {
        int s = gain - (off ? off[b] : 0);
        if (s < 0) s = 0;
        if (s > 255) s = 255;
        trial->sf[b] = static_cast<uint8_t>(s);
    }
    trial->global_gain = gain;  // fallback; section_and_count overrides
    trial->fit_gain = gain;
    int maxabs = aac_quantize(p34, spec, L, trial->sf, trial->ix);
    if (maxabs > kMaxQuant) return false;
    aac_section_and_count(trial->ix, L, trial);
    return trial->ics_bits <= budget_bits;
}

}  // namespace

void aac_fit_channel(const SpecT* spec, const AacBandLayout& L,
                     int budget_bits, const int* sf_offsets, int gain_hint,
                     AacChannelPlan* plan) {
    const int n = L.num_lines;

    P34T p34[1024];
#ifdef GLINT_AAC_INT
    // Log-domain cache: p34log = 0.75 * (log2|S| - kSpecFracBits) in Q16,
    // so quantization is one add + one exp2 lookup per coefficient.
    for (int i = 0; i < n; i++) {
        int32_t v = spec[i] < 0 ? -spec[i] : spec[i];
        if (v == 0) {
            p34[i] = INT32_MIN;
        } else {
            int64_t l2 = static_cast<int64_t>(ilog2_q16(static_cast<uint32_t>(v))) -
                         (static_cast<int64_t>(kSpecFracBits) << 16);
            p34[i] = static_cast<int32_t>((l2 * 3) >> 2);
        }
    }
#else
    for (int i = 0; i < n; i++) {
        p34[i] = static_cast<P34T>(std::pow(std::fabs(static_cast<double>(spec[i])), 0.75));
    }
#endif
    std::memset(plan->ix, 0, sizeof(plan->ix));

    AacChannelPlan trial;
    trial.tns = plan->tns;  // caller decides TNS before fitting; bits count
    std::memset(trial.ix, 0, sizeof(trial.ix));

    bool have_fit = false;
    if (gain_hint >= 0) {
        // Local walk from the hint: bits(G) is (near-)monotone decreasing in
        // G, and shaping moves the answer by only a step or two per iteration.
        int g = gain_hint;
        if (eval_gain(p34, spec, L, budget_bits, sf_offsets, g, &trial)) {
            *plan = trial;
            have_fit = true;
            while (g > 0 &&
                   eval_gain(p34, spec, L, budget_bits, sf_offsets, g - 1, &trial)) {
                g--;
                *plan = trial;
            }
        } else {
            while (g < 255) {
                g++;
                if (eval_gain(p34, spec, L, budget_bits, sf_offsets, g, &trial)) {
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
            if (eval_gain(p34, spec, L, budget_bits, sf_offsets, mid, &trial)) {
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
        eval_gain(p34, spec, L, budget_bits, sf_offsets, 255, plan);
    }
}

void aac_write_ics_info(AacBitWriter& bw, const AacBandLayout& L) {
    bw.put(0, 1);                     // ics_reserved_bit
    bw.put(L.window_sequence, 2);
    bw.put(0, 1);                     // window_shape = sine
    if (L.window_sequence == kSeqShort) {
        bw.put(L.max_sfb, 4);
        // scale_factor_grouping: 7 bits, MSB first; bit for window w (1..7)
        // is 1 when w belongs to the same group as window w-1.
        uint32_t grouping = 0;
        int w = 0;
        for (int g = 0; g < L.num_groups; g++) {
            for (int j = 0; j < L.group_len[g]; j++, w++) {
                if (w == 0) continue;
                grouping = (grouping << 1) | (j > 0 ? 1u : 0u);
            }
        }
        bw.put(grouping, 7);
    } else {
        bw.put(L.max_sfb, 6);
        bw.put(0, 1);                 // predictor_data_present (none in LC)
    }
}

void aac_write_ics_body(AacBitWriter& bw, const AacChannelPlan& plan,
                        const AacBandLayout& L, bool include_ics_info) {
    const int nb = L.num_bands;

    bw.put(plan.global_gain, 8);
    if (include_ics_info) {
        aac_write_ics_info(bw, L);
    }

    // section_data (sections never cross group boundaries)
    for (int b = 0; b < nb;) {
        int cb = plan.book[b];
        int g = L.group_of_band[b];
        int e = b + 1;
        while (e < nb && plan.book[e] == cb && L.group_of_band[e] == g) e++;
        int len = e - b;
        bw.put(cb, 4);
        while (len >= L.sect_esc) {
            bw.put(L.sect_esc, L.sect_bits);
            len -= L.sect_esc;
        }
        bw.put(len, L.sect_bits);
        b = e;
    }

    // scale_factor_data: dpcm chain over coded bands, starting at global_gain
    // (zero-book bands transmit nothing and do not advance the chain).
    {
        int last = plan.global_gain;
        for (int b = 0; b < nb; b++) {
            if (plan.book[b] == 0) continue;
            int dpcm = plan.sf[b] - last + 60;
            if (dpcm < 0) dpcm = 0;          // unreachable if offsets within +-30
            if (dpcm > 120) dpcm = 120;
            bw.put(kScfCodes[dpcm], kScfBits[dpcm]);
            last = last + (dpcm - 60);
        }
    }

    bw.put(0, 1);  // pulse_data_present
    bw.put(plan.tns.active, 1);  // tns_data_present
    if (plan.tns.active) {
        // Long windows, one filter, 4-bit coefficients, forward direction.
        bw.put(1, 2);                    // n_filt
        bw.put(1, 1);                    // coef_res = 4-bit
        bw.put(plan.tns.length, 6);
        bw.put(plan.tns.order, 5);
        bw.put(0, 1);                    // direction = forward
        bw.put(0, 1);                    // coef_compress = 0
        for (int m = 0; m < plan.tns.order; m++) {
            bw.put(static_cast<uint32_t>(plan.tns.coef_idx[m]) & 0xF, 4);
        }
    }
    bw.put(0, 1);  // gain_control_data_present

    // spectral_data
    for (int b = 0; b < nb; b++) {
        if (plan.book[b] != 0) {
            code_band(&bw, plan.book[b], plan.ix, L.offset[b], L.offset[b + 1]);
        }
    }
}

}  // namespace aac
}  // namespace glint

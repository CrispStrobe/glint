// glint - Quantization loop (double-precision input)
// MIT License - Clean-room implementation

#include "quantize.hpp"
#include "psycho.hpp"
#include "tables.hpp"
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <functional>

namespace glint {

// ---------------------------------------------------------------------------
// Thread pool for the per-granule scale-factor search.
//
// The factor search in quantize_granule evaluates nf independent candidates
// (each a full quantize_base + granule_mse). quantize_base is pure, so the
// candidates can run on a pool and be reduced in deterministic index order.
// That makes the output BYTE-IDENTICAL regardless of thread count — the
// reduction picks the lowest-index minimum exactly as the old sequential
// loop did. Default thread count is 1 (the pool is never created; the loop
// runs inline), so the library stays deterministic and dependency-free unless
// a caller explicitly opts in via glint_config.threads / CLI -j.
// ---------------------------------------------------------------------------
// Spin-wait pool. The per-granule tasks are tiny (tens of microseconds), so a
// condition-variable wake/sleep per dispatch (thousands of dispatches/sec)
// costs more than the work it parallelizes. Workers instead spin on an atomic
// generation counter (yielding after a spin budget), giving sub-microsecond
// dispatch latency. This burns idle cores during the sequential per-frame work
// between dispatches, which is the right trade for a batch encoder.
class ThreadPool {
public:
    explicit ThreadPool(int n) {
        for (int i = 0; i < n; i++)
            workers_.emplace_back([this] { worker(); });
    }
    ~ThreadPool() {
        stop_.store(true, std::memory_order_release);
        for (auto& t : workers_) t.join();
    }
    int size() const { return static_cast<int>(workers_.size()); }

    // Run fn(i) for i in [0, n). Blocks until all complete. The calling
    // thread participates, so total parallelism is workers + 1.
    void parallel_for(int n, const std::function<void(int)>& fn) {
        if (n <= 0) return;
        fn_ = &fn;
        n_ = n;
        next_.store(0, std::memory_order_relaxed);
        active_.store(static_cast<int>(workers_.size()) + 1,
                      std::memory_order_relaxed);
        gen_.fetch_add(1, std::memory_order_release);  // publish the job
        run_indices(&fn, n);                            // calling thread works
        active_.fetch_sub(1, std::memory_order_acq_rel);
        // Wait for workers to drain.
        int spins = 0;
        while (active_.load(std::memory_order_acquire) != 0) {
            if (++spins > 2048) std::this_thread::yield();
        }
    }

private:
    void run_indices(const std::function<void(int)>* fn, int n) {
        for (;;) {
            int i = next_.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) break;
            (*fn)(i);
        }
    }
    void worker() {
        uint64_t seen = 0;
        int spins = 0;
        for (;;) {
            uint64_t g = gen_.load(std::memory_order_acquire);
            if (g == seen) {
                if (stop_.load(std::memory_order_acquire)) return;
                if (++spins > 2048) std::this_thread::yield();
                continue;
            }
            seen = g;
            spins = 0;
            const std::function<void(int)>* fn = fn_;
            int n = n_;
            run_indices(fn, n);
            active_.fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{false};
    const std::function<void(int)>* fn_ = nullptr;
    int n_ = 0;
    std::atomic<int> next_{0};
    std::atomic<int> active_{0};
    std::atomic<uint64_t> gen_{0};
};

static int g_quant_threads = 1;
static ThreadPool* g_pool = nullptr;

void quantize_set_threads(int n) {
    if (n < 1) n = 1;
    if (n == g_quant_threads) return;
    g_quant_threads = n;
    delete g_pool;
    g_pool = nullptr;
    if (n > 1) g_pool = new ThreadPool(n - 1);  // + the calling thread
}

// Run fn(i) for i in [0,n): on the pool when threads > 1, else inline. Inline
// runs in ascending index order — identical to the old sequential loop.
static void quant_parallel_for(int n, const std::function<void(int)>& fn) {
    if (g_pool && n > 1) {
        g_pool->parallel_for(n, fn);
    } else {
        for (int i = 0; i < n; i++) fn(i);
    }
}

static double gain_table[256];
static double sf_table[2][16];
// cbrt_lut[a] = a * cbrt(a), for a = |ix| in [0, 8191] (ix is clamped to 8191
// in the quantizer). Used in granule_mse to avoid a std::cbrt per nonzero
// coefficient. Holds exactly the same double the inline expression produced,
// so the MSE — and the scale-factor it selects — is bit-identical.
static double cbrt_lut[8192];
static bool tables_init = false;

static void init_quant_tables() {
    if (tables_init) return;
    for (int g = 0; g < 256; g++)
        gain_table[g] = std::pow(2.0, -3.0 * (g - 210.0) / 16.0);
    for (int a = 0; a < 8192; a++)
        cbrt_lut[a] = static_cast<double>(a) * std::cbrt(static_cast<double>(a));
    for (int sf = 0; sf < 16; sf++) {
        // Encoder uses positive exponent to compensate decoder's negative:
        // Decoder: 2^(-0.5*(1+sfs)*sf), Encoder: 2^(+0.75*0.5*(1+sfs)*sf)
        sf_table[0][sf] = std::pow(2.0, 0.75 * sf * 0.5);   // sfs=0
        sf_table[1][sf] = std::pow(2.0, 0.75 * sf * 1.0);   // sfs=1
    }
    tables_init = true;
}

// x^0.75 for the quantizer. Inputs are raw MDCT coefficients — almost all
// FRACTIONAL (~1e-6..0.3). The old integer-grid table interpolation
// (pow34_table[int(x)] + frac*...) degenerated to pow34(x)=x on (0,1) — a
// 33% error at x=0.2 — so ix was linear in x instead of x^0.75 and the
// decoder's exact ix^(4/3) expansion warped the entire spectrum by x^(4/3):
// content 20 dB below the granule peak decoded 6.7 dB too quiet. That single
// curve error capped whole-file SNR at ~15 dB independent of bitrate.
static double fast_pow34(double x) {
    if (x <= 0.0) return 0.0;
    return std::pow(x, 0.75);
}

// Cache for pre-computed per-coefficient values (constant across binary search)
struct QuantCache {
    double pow34_sf[576]; // pow34(|xr|) * sf_scale - precomputed
    int8_t sign[576];     // +1 or -1
};

static int find_count1_start(const int16_t* ix, int rzero) {
    int count1_start = (rzero + 3) & ~3;
    if (count1_start > rzero) count1_start = rzero;

    while (count1_start >= 4) {
        bool all_small = true;
        for (int i = count1_start - 4; i < count1_start; i++) {
            if (i < 576 && std::abs(ix[i]) > 1) {
                all_small = false;
                break;
            }
        }
        if (!all_small) break;
        count1_start -= 4;
    }

    if (count1_start & 1) count1_start++;
    return count1_start;
}

static void fill_quant_cache(QuantCache& cache, const double* mdct_in,
                              const int scalefac[21], int scalefac_scale,
                              int preflag, int sr_index) {
    const int* sfb = tables::get_sfb_long_by_unified(sr_index);
    int band = 0;
    for (int i = 0; i < 576; i++) {
        while (band < 21 && i >= sfb[band + 1]) band++;
        // Bins at/above sfb[21] are the standard's "sfb21" region: no
        // scalefactor is transmitted, so the decoder applies none — the
        // encoder must not either. (This used to read scalefac[21], out of
        // bounds — aliasing scalefac_compress in GranuleInfo and silently
        // boosting everything above ~9 kHz by up to 29x with no decoder-side
        // compensation.)
        int sf = 0;
        if (band < 21 && i < sfb[21]) {
            sf = scalefac[band];
            if (preflag) sf += tables::preemphasis[band];
        }
        double sfs = (sf > 0 && sf < 16) ? sf_table[scalefac_scale][sf] : 1.0;
        cache.pow34_sf[i] = fast_pow34(std::fabs(mdct_in[i])) * sfs;
        cache.sign[i] = (mdct_in[i] < 0.0) ? -1 : 1;
    }
}

static int quantize_and_count(const double* mdct_in, int16_t* ix,
                               int global_gain, const int scalefac[21],
                               int scalefac_scale, int preflag,
                               int sr_index,
                               HuffRegions* out_regions = nullptr,
                               const QuantCache* cache = nullptr,
                               bool short_block = false,
                               int bit_limit = -1) {
    init_quant_tables();
    double base_step = gain_table[global_gain];

    // Fused quantize + region detection in a single pass
    int rzero = 0;          // last nonzero index + 1

    if (cache) {
        for (int i = 0; i < 576; i++) {
            double qval_d = cache->pow34_sf[i] * base_step + 0.4054;
            int qval = (qval_d >= 8191.0) ? 8191 : static_cast<int>(qval_d);
            ix[i] = cache->sign[i] * qval;
        }
        // Scan backward for rzero (avoids branch in hot loop)
        rzero = 576;
        while (rzero > 0 && ix[rzero - 1] == 0) rzero--;
    } else {
        const int* sfb = tables::get_sfb_long_by_unified(sr_index);
        int band = 0;
        for (int i = 0; i < 576; i++) {
            while (band < 21 && i >= sfb[band + 1]) band++;
            // sfb21 region (>= sfb[21]): no scalefactor (see fill_quant_cache)
            int sf = 0;
            if (band < 21 && i < sfb[21]) {
                sf = scalefac[band];
                if (preflag) sf += tables::preemphasis[band];
            }
            double sf_scale = (sf > 0 && sf < 16) ? sf_table[scalefac_scale][sf] : 1.0;
            double abs_xr = std::fabs(mdct_in[i]);
            double pow34_val = fast_pow34(abs_xr);
            double qval_d = pow34_val * base_step * sf_scale + 0.4054;
            int qval = (qval_d >= 8191.0) ? 8191 : static_cast<int>(qval_d);
            ix[i] = (mdct_in[i] < 0.0) ? -qval : qval;
        }
        rzero = 576;
        while (rzero > 0 && ix[rzero - 1] == 0) rzero--;
    }

    // Short blocks use a different region layout; the gain search must count
    // bits with the same layout the encoder will actually use.
    int count1_start = find_count1_start(ix, rzero);
    if (!short_block) {
        // Fused region/table selection + count: one pass picks each region's
        // cheapest candidate table and returns the total.
        return huffman_select_and_count(ix, sr_index, rzero, count1_start,
                                        bit_limit, out_regions);
    }
    HuffRegions regions = huffman_determine_regions_short_from_bounds(
        ix, sr_index, rzero, count1_start);
    if (out_regions) *out_regions = regions;
    return bit_limit >= 0 ? huffman_count_bits_limited(ix, regions, sr_index,
                                                       bit_limit)
                          : huffman_count_bits(ix, regions, sr_index);
}

static int encode_scalefac_compress(int slen1, int slen2) {
    static const int table[16][2] = {
        {0,0},{0,1},{0,2},{0,3},{3,0},{1,1},{1,2},{1,3},
        {2,1},{2,2},{2,3},{3,1},{3,2},{3,3},{4,2},{4,3}
    };
    for (int i = 0; i < 16; i++)
        if (table[i][0] == slen1 && table[i][1] == slen2) return i;
    return 0;
}

// MPEG-2/2.5 scalefac_compress encoding (9 bits), ISO 13818-3 mapping for
// normal (non-intensity) long blocks, first range:
//   sfc < 400: slen[0]=(sfc>>4)/5, slen[1]=(sfc>>4)%5,
//              slen[2]=(sfc&15)>>2, slen[3]=sfc&3; band groups [6,5,5,5]
// so sfc = ((slen0*5 + slen1) << 4) | (slen2 << 2) | slen3, valid for
// slen0/1 <= 4 and slen2/3 <= 3 (max sfc 399 < 400 by construction).
// (The previous custom mapping — sfc = slen0*36 + slen1*6 + slen2 — matched
// the encoder's own emission code but NOT the standard, so every real
// decoder derived different slens and misparsed the whole granule: MPEG-2
// output measured -10 dB SNR. See PLAN.md item 7.)
static int encode_scalefac_compress_m2(int slen0, int slen1, int slen2, int slen3) {
    if (slen0 <= 4 && slen1 <= 4 && slen2 <= 3 && slen3 <= 3) {
        return ((slen0 * 5 + slen1) << 4) | (slen2 << 2) | slen3;
    }
    return -1;  // not representable in this range
}

// Thread-local psycho model for use by quantize_granule
static PsychoModel s_psycho;

// Compute headroom-based scalefactors using Vorbis/Opus/FLAC psychoacoustic
// masking model. Assigns SF inversely proportional to masking headroom:
// loud bands (high SMR) get sf=0 (noise masked by signal), bands near
// masking threshold get higher SF (need precision), bands below threshold
// get sf=0 (inaudible, save bits).
static bool compute_headroom_scalefactors(int scalefac[21],
                                           const double band_energy[21],
                                           int /* active_bands */) {
    // Compute per-band masking threshold via simplified spreading function
    double band_mask[21];
    for (int band = 0; band < 21; band++) {
        band_mask[band] = 0;
        for (int other = 0; other < 21; other++) {
            if (other == band) continue;  // exclude self-masking
            double spread;
            if (other > band)
                spread = std::pow(10.0, -0.8 * (other - band));  // 8 dB/band upward
            else
                spread = std::pow(10.0, -0.4 * (band - other));  // 4 dB/band downward
            band_mask[band] += band_energy[other] * spread;
        }
        // ATH floor, scaled to MDCT energy domain (MDCT divides by 288)
        double ath_linear = std::pow(10.0, (tables::ath_cb[std::min(band, 24)] - 96.0) / 10.0);
        ath_linear /= (288.0 * 288.0);
        // Use 0.1 (-10 dB) masking offset for inter-band masking
        band_mask[band] = std::max(band_mask[band] * 0.1, ath_linear);
    }

    // SMR (signal-to-mask ratio) per band, assign SF
    // Bands with energy well above their mask have lots of headroom and
    // can tolerate coarse quantization. Bands near the mask need precision.
    bool any = false;
    for (int band = 0; band < 21; band++) {
        double smr = band_energy[band] / band_mask[band];
        double headroom_db = 10.0 * std::log10(std::max(smr, 1e-10));
        // Map headroom to SF with wider range for better differentiation:
        // headroom <= 0 dB  -> sf = 0 (below mask, inaudible)
        // headroom  0-30 dB -> sf = 7..0 (linear mapping)
        // headroom > 30 dB  -> sf = 0 (far above mask, self-masked)
        int sf;
        if (headroom_db <= 0.0)
            sf = 0;
        else if (headroom_db > 30.0)
            sf = 0;
        else
            sf = static_cast<int>((30.0 - headroom_db) * 7.0 / 30.0);
        scalefac[band] = sf;
        if (sf > 0) any = true;
    }
    return any;
}

// Per-band source energy for the granule_mse envelope penalty. Depends only
// on the unscaled input, so it is identical for every candidate in the scale
// search — compute it once per granule and share it. Loop order matches the
// old in-loop accumulation exactly (ascending i within each band), so the
// sums are bit-identical.
static void compute_src_band(const double* mdct_in, int sr_index,
                             double src_band[21]) {
    const int* sfb = tables::get_sfb_long_by_unified(sr_index);
    for (int b = 0; b < 21; b++) {
        src_band[b] = 0.0;
        int start = sfb[b];
        int end = (b < 20) ? sfb[b + 1] : 576;
        for (int i = start; i < end; i++)
            src_band[b] += mdct_in[i] * mdct_in[i];
    }
}

// Decoder-reconstruction MSE for a quantized granule vs the original MDCT.
// Used to pick the per-granule input scale ("factor") that best preserves the
// spectrum through the quantizer dead-zone.
//
// This used to add an envelope-retention penalty (log-loss on bands whose
// decoded energy fell below 90% of the source, HF-weighted) to fight the
// "dull" rolloff collapse. That collapse was a symptom of the pow34 curve
// bug; with the curve fixed the penalty measured as a no-op on music and
// -0.05..-0.13 dB SNR on speech at 256k/128k, so it was removed. Pure
// reconstruction MSE is the objective now.
static double granule_mse(const GranuleInfo& gi, const double* mdct_in,
                          int sr_index) {
    const int* sfb = tables::get_sfb_long_by_unified(sr_index);
    double decoder_gain = std::pow(2.0, 0.25 * (gi.global_gain - 210));
    double noise = 0.0;
    // Per-band iteration so the expensive std::pow(2.0, ...) sf_d term is
    // computed once per band instead of once per coefficient. "Band" 21 is
    // the sfb21 region [sfb[21], 576): no scalefactor is transmitted there,
    // so the decoder reconstructs with sf_d = 1 — mirror that exactly.
    for (int b = 0; b < 22; b++) {
        int start = sfb[b];
        int end = (b < 21) ? sfb[b + 1] : 576;
        if (start >= end) continue;
        double sf_d = 1.0;
        if (b < 21) {
            int sf = gi.scalefac[b];
            if (gi.preflag) sf += tables::preemphasis[b];
            sf_d = std::pow(2.0, -0.5 * sf * (1 + gi.scalefac_scale));
        }
        for (int i = start; i < end; i++) {
            double xr_hat = 0.0;
            if (gi.ix[i] != 0) {
                // a^(4/3) == a * a^(1/3); precomputed in cbrt_lut[a]. ix is
                // always in [0,8191], but guard the index defensively.
                int a = std::abs(static_cast<int>(gi.ix[i]));
                double a43 = (a < 8192) ? cbrt_lut[a]
                                        : static_cast<double>(a) * std::cbrt(static_cast<double>(a));
                xr_hat = std::copysign(a43 * decoder_gain * sf_d, mdct_in[i]);
            }
            double err = mdct_in[i] - xr_hat;
            noise += err * err;
        }
    }
    return noise;
}

// Base quantizer: gain search to the bit budget + energy-based scalefactors.
// Operates on already-scaled input; the public quantize_granule wraps this in
// the per-granule scale search. gain_floor > 0 keeps the search from going
// finer than that gain (VBR constant-quality target).
static GranuleInfo quantize_base(const double* mdct_in, int available_bits,
                                 int sr_index, bool short_block,
                                 int gain_floor = 0);
static void gain_search_with_scalefacs(GranuleInfo& info, const double* mdct_in,
                                       int available_bits, int sr_index,
                                       bool short_block, int gain_floor = 0);
static bool encode_scalefac_fields(GranuleInfo& info, int sr_index);

// Per-band decoder-reconstruction noise vs the original MDCT (same
// reconstruction expression as granule_mse).
static void compute_band_noise(const GranuleInfo& gi, const double* mdct_in,
                               int sr_index, double noise_band[21]) {
    const int* sfb = tables::get_sfb_long_by_unified(sr_index);
    double decoder_gain = std::pow(2.0, 0.25 * (gi.global_gain - 210));
    for (int b = 0; b < 21; b++) noise_band[b] = 0.0;
    // Same band/sf_d handling as granule_mse (sfb21 region reconstructs with
    // sf_d = 1); its noise is accounted to band 20 for scoring purposes.
    for (int b = 0; b < 22; b++) {
        int start = sfb[b];
        int end = (b < 21) ? sfb[b + 1] : 576;
        if (start >= end) continue;
        double sf_d = 1.0;
        if (b < 21) {
            int sf = gi.scalefac[b];
            if (gi.preflag) sf += tables::preemphasis[b];
            sf_d = std::pow(2.0, -0.5 * sf * (1 + gi.scalefac_scale));
        }
        double acc = 0.0;
        for (int i = start; i < end; i++) {
            double xr_hat = 0.0;
            if (gi.ix[i] != 0) {
                int a = std::abs(static_cast<int>(gi.ix[i]));
                double a43 = (a < 8192) ? cbrt_lut[a]
                                        : static_cast<double>(a) * std::cbrt(static_cast<double>(a));
                xr_hat = std::copysign(a43 * decoder_gain * sf_d, mdct_in[i]);
            }
            double err = mdct_in[i] - xr_hat;
            acc += err * err;
        }
        noise_band[(b < 21) ? b : 20] += acc;
    }
}

// Per-band masking thresholds for the outer loop, from the per-band source
// energies. Self-masking at -20 dB SMR plus neighbor spreading with a steep
// 15 dB/band decay, floored at an ATH-shaped term. (The PsychoModel in
// psycho.cpp is intentionally NOT used here: its 1.5-3 dB/Bark slopes and
// excluded self-masking are tuned for conservative coefficient pruning in
// VBR — as an allocation target they let a loud low band "mask" the entire
// spectrum, and the loop then trades away 30 dB of low-band SNR.)
static void compute_band_masks(const double* src_band, double mask_band[21]) {
    static const double kSMR = 1e-2;  // -20 dB signal-to-mask within a band
    for (int b = 0; b < 21; b++) {
        double m = src_band[b];
        for (int j = 0; j < 21; j++) {
            if (j == b) continue;
            m += src_band[j] * std::pow(10.0, -1.5 * std::abs(b - j));
        }
        // ATH floor in MDCT-energy domain (see compute_headroom_scalefactors)
        double ath_linear = std::pow(10.0, (tables::ath_cb[std::min(b, 24)] - 96.0) / 10.0);
        ath_linear /= (288.0 * 288.0);
        mask_band[b] = std::max(m * kSMR, ath_linear);
    }
}

// Perceptual score for the outer loop: (number of bands whose noise exceeds
// the mask, summed log-excess of those bands). Lexicographic — fewer audible
// bands always wins; ties broken by total excess.
static void nmr_score(const GranuleInfo& gi, const double* mdct_in,
                      int sr_index, const double* mask_band,
                      int* over_count, double* over_sum) {
    double noise_band[21];
    compute_band_noise(gi, mdct_in, sr_index, noise_band);
    int oc = 0;
    double os = 0.0;
    for (int b = 0; b < 21; b++) {
        if (mask_band[b] <= 0.0) continue;
        if (noise_band[b] > mask_band[b]) {
            oc++;
            os += std::log(noise_band[b] / mask_band[b]);
        }
    }
    *over_count = oc;
    *over_sum = os;
}

// NMR-driven outer loop (LAME-style noise shaping): amplify the scalefactors
// of bands whose reconstruction noise exceeds the masking threshold, then
// re-run the gain search so global_gain coarsens to pay for it — noise moves
// from audible bands into masked ones instead of being added on top. This is
// the mechanism the failed iterative-sf-amplify / smr-sf-amplify branches
// lacked: they kept the gain fixed, so with a full bit budget every boost
// was reverted (see CLAUDE.md history). Keeps the best iterate by
// (over_count, over_sum); returns `start` untouched if nothing improves.
// MPEG-1 long blocks only (the MPEG-2 scalefac_compress ranges are narrower
// and the m2 field encoding can't represent nonzero bands 16-20).
static GranuleInfo nmr_outer_loop(const GranuleInfo& start, double factor,
                                  const double* mdct_in, int available_bits,
                                  int sr_index, int max_iters,
                                  const double* src_band) {
    double scaled[576];
    for (int i = 0; i < 576; i++) scaled[i] = mdct_in[i] * factor;

    double mask_band[21];
    compute_band_masks(src_band, mask_band);

    GranuleInfo best = start;
    int best_oc;
    double best_os;
    nmr_score(start, mdct_in, sr_index, mask_band, &best_oc, &best_os);
    if (best_oc == 0) return best;

    // Total-noise guard: the loop may only REDISTRIBUTE noise, never blow it
    // up — a candidate whose total noise exceeds the start's by more than
    // this factor is rejected regardless of its NMR score. Without it the
    // loop trades tens of dB of loud-band SNR for the last few "audible"
    // bands.
    static const double kNoiseGuard = 1.15;
    double noise0[21];
    compute_band_noise(start, mdct_in, sr_index, noise0);
    double total0 = 0.0;
    for (int b = 0; b < 21; b++) total0 += noise0[b];

    // MPEG-1 slen field limits: bands 0-10 max 15, bands 11-20 max 7.
    static const int kSfMax[21] = { 15,15,15,15,15,15,15,15,15,15,15,
                                    7,7,7,7,7,7,7,7,7,7 };

    GranuleInfo cur = start;
    for (int iter = 0; iter < max_iters; iter++) {
        double noise_band[21];
        compute_band_noise(cur, mdct_in, sr_index, noise_band);
        GranuleInfo cand = cur;
        bool amplified = false;
        for (int b = 0; b < 21; b++) {
            if (mask_band[b] > 0.0 && noise_band[b] > mask_band[b] &&
                cand.scalefac[b] < kSfMax[b]) {
                cand.scalefac[b]++;
                amplified = true;
            }
        }
        if (!amplified) break;
        if (!encode_scalefac_fields(cand, sr_index)) break;
        gain_search_with_scalefacs(cand, scaled, available_bits, sr_index,
                                   /*short_block=*/false);

        double cand_noise[21];
        compute_band_noise(cand, mdct_in, sr_index, cand_noise);
        double cand_total = 0.0;
        for (int b = 0; b < 21; b++) cand_total += cand_noise[b];

        int oc = 0;
        double os = 0.0;
        for (int b = 0; b < 21; b++) {
            if (mask_band[b] <= 0.0) continue;
            if (cand_noise[b] > mask_band[b]) {
                oc++;
                os += std::log(cand_noise[b] / mask_band[b]);
            }
        }
        if (cand_total <= total0 * kNoiseGuard &&
            (oc < best_oc || (oc == best_oc && os < best_os))) {
            best_oc = oc;
            best_os = os;
            best = cand;
        }
        if (oc == 0 || cand_total > total0 * kNoiseGuard) break;
        cur = cand;
    }
    return best;
}

// Per-granule scale search (normal/best tiers).
//
// The quantizer is is = int(|xr|^0.75 * step + 0.4054); coefficients whose
// scaled magnitude is below the ~0.6 dead-zone round to zero. With the pow34
// curve fixed, reconstruction is level-exact at f=1.0; small boosts >1 only
// serve to rescue dead-zone coefficients (at a matching level cost the MSE
// weighs), so the grids sit tightly above 1.0. The old grids (1.3..4.2, and
// the fixed 288/194 "gain correction" before them) existed to compensate the
// broken pow34 curve — post-fix they are pure level errors. -q speed skips
// the search entirely: f=1.0 wins almost always, and the tier's contract is
// throughput.
GranuleInfo quantize_granule(const double* mdct_in, int available_bits,
                              int sr_index, int quality_mode, bool short_block,
                              int gain_floor) {
    init_quant_tables();

    if (quality_mode <= 0) {
        return quantize_base(mdct_in, available_bits, sr_index, short_block,
                             gain_floor);
    }

    static const double kNormal[] = { 1.0, 1.04, 1.09, 1.15, 1.22, 1.30 };
    static const double kBest[]   = {
        1.0, 1.02, 1.05, 1.08, 1.11, 1.15, 1.19, 1.24, 1.30, 1.38, 1.48, 1.60 };
    const double* factors; int nf;
    if (quality_mode >= 2) { factors = kBest;   nf = 12; }
    else                   { factors = kNormal; nf = 6; }

    // Per-band source energy is factor-independent; compute it once (used by
    // the NMR outer loop when enabled).
    double src_band[21];
    compute_src_band(mdct_in, sr_index, src_band);

    // Evaluate all nf candidate factors (each independent), then reduce in
    // ascending index order — same selection as the old sequential loop
    // (first/lowest-index minimum wins), so the result is byte-identical for
    // any thread count.
    std::vector<GranuleInfo> results(nf);
    std::vector<double> mses(nf);
    quant_parallel_for(nf, [&](int fi) {
        double f = factors[fi];
        double scaled[576];
        for (int i = 0; i < 576; i++) scaled[i] = mdct_in[i] * f;
        results[fi] = quantize_base(scaled, available_bits, sr_index,
                                    short_block, gain_floor);
        mses[fi] = granule_mse(results[fi], mdct_in, sr_index);
    });

    GranuleInfo best_result = results[0];
    double best_mse = mses[0];
    double best_factor = factors[0];
    for (int fi = 1; fi < nf; fi++) {
        if (mses[fi] < best_mse) {
            best_mse = mses[fi];
            best_result = results[fi];
            best_factor = factors[fi];
        }
    }

    if (quality_mode >= 2) {
        // Fine refinement around best_factor: candidates for step -2,-1,1,2
        // (skipping any f < 0.98), kept in that exact order for the reduction.
        double cand[4];
        int nc = 0;
        for (int step = -2; step <= 2; step++) {
            if (step == 0) continue;
            double f = best_factor + step * 0.015;
            if (f < 0.98) continue;
            cand[nc++] = f;
        }
        std::vector<GranuleInfo> rres(nc);
        std::vector<double> rmse(nc);
        quant_parallel_for(nc, [&](int k) {
            double scaled[576];
            for (int i = 0; i < 576; i++) scaled[i] = mdct_in[i] * cand[k];
            rres[k] = quantize_base(scaled, available_bits, sr_index,
                                    short_block, gain_floor);
            rmse[k] = granule_mse(rres[k], mdct_in, sr_index);
        });
        for (int k = 0; k < nc; k++) {
            if (rmse[k] < best_mse) {
                best_mse = rmse[k];
                best_result = rres[k];
                best_factor = cand[k];
            }
        }
    }

    // NMR-driven noise shaping on the winning candidate (MPEG-1 long blocks;
    // see nmr_outer_loop). Disabled since the pow34-curve fix: the loop's
    // -20 dB SMR masks sit far above the new noise floor, so it amplifies
    // bands whose noise is already inaudible and pays with real SNR
    // (-0.7 dB at -q best). Re-enable only after re-tuning the masks to the
    // post-fix noise floor (PLAN.md item 3).
    static constexpr bool kNmrOuterLoop = false;
    if (kNmrOuterLoop && quality_mode >= 2 && sr_index < 3 && !short_block) {
        int max_iters = 10;
        best_result = nmr_outer_loop(best_result, best_factor, mdct_in,
                                     available_bits, sr_index, max_iters,
                                     src_band);
    }
    return best_result;
}

// Encode info.scalefac into scalefac_compress/part2_length (slen selection).
// Returns false when the scalefactors cannot be represented (a band exceeds
// its slen field's range), leaving the fields untouched.
static bool encode_scalefac_fields(GranuleInfo& info, int sr_index) {
    bool is_mpeg2 = (sr_index >= 3);
    if (is_mpeg2) {
        // Band groups [6,5,5,5]; slen limits 4/4/3/3 in the sfc<400 range,
        // i.e. sf <= 15 in groups 0-1 and sf <= 7 in groups 2-3.
        int max_sf[4] = {};
        for (int b = 0; b < 6; b++) max_sf[0] = std::max(max_sf[0], info.scalefac[b]);
        for (int b = 6; b < 11; b++) max_sf[1] = std::max(max_sf[1], info.scalefac[b]);
        for (int b = 11; b < 16; b++) max_sf[2] = std::max(max_sf[2], info.scalefac[b]);
        for (int b = 16; b < 21; b++) max_sf[3] = std::max(max_sf[3], info.scalefac[b]);
        if (max_sf[0] > 15 || max_sf[1] > 15 || max_sf[2] > 7 || max_sf[3] > 7)
            return false;
        int sl[4];
        for (int g = 0; g < 4; g++) {
            sl[g] = 0;
            while ((1 << sl[g]) <= max_sf[g]) sl[g]++;
        }
        int sfc = encode_scalefac_compress_m2(sl[0], sl[1], sl[2], sl[3]);
        if (sfc < 0) return false;
        info.scalefac_compress = sfc;
        info.part2_length = sl[0]*6 + sl[1]*5 + sl[2]*5 + sl[3]*5;
    } else {
        int max_sf1 = 0, max_sf2 = 0;
        for (int b = 0; b < 11; b++) max_sf1 = std::max(max_sf1, info.scalefac[b]);
        for (int b = 11; b < 21; b++) max_sf2 = std::max(max_sf2, info.scalefac[b]);
        if (max_sf1 > 15 || max_sf2 > 7) return false;
        int slen1 = 0; while ((1 << slen1) <= max_sf1) slen1++;
        int slen2 = 0; while ((1 << slen2) <= max_sf2) slen2++;
        if (slen1 > 4) slen1 = 4;
        if (slen2 > 3) slen2 = 3;
        info.scalefac_compress = encode_scalefac_compress(slen1, slen2);
        info.part2_length = slen1 * 11 + slen2 * 10;
    }
    return true;
}

// Gain search with the scalefactors already fixed in info: fills the quant
// cache once, binary-searches global_gain to the bit budget (reusing the last
// accepted iteration's state — see quantize_base history), and runs the
// budget-guarantee loop. Sets ix/global_gain/regions/part2_3_length.
static void gain_search_with_scalefacs(GranuleInfo& info, const double* mdct_in,
                                       int available_bits, int sr_index,
                                       bool short_block, int gain_floor) {
    int target_bits = available_bits - info.part2_length;
    if (target_bits < 0) target_bits = 0;

    // The cache is filled once and reused for the final quantize and the
    // budget-guarantee loop below (scalefactors no longer change here).
    QuantCache cache;
    fill_quant_cache(cache, mdct_in, info.scalefac, info.scalefac_scale,
                     info.preflag, sr_index);

    // Gain bounds are computed from the cache's actual quantizer input —
    // pow34(|xr|) INCLUDING the per-band scalefactor boost. Computing them
    // from pow34(max|xr|) alone (the historical bug) let any band with a
    // nonzero scalefactor (sfs up to 6.17x at sf=7) sail past the 8191 hard
    // clamp at the "minimum" gain: the loudest band then decoded up to
    // ~-21 dB quiet, capping whole-file SNR at ~15 dB regardless of bitrate,
    // piling the error into the loudest (0-1 kHz) bands, and forcing the
    // scale search to prefer f>1 purely to coarsen the gain away from
    // clipping.
    double peak34 = 0.0;
    for (int i = 0; i < 576; i++)
        if (cache.pow34_sf[i] > peak34) peak34 = cache.pow34_sf[i];

    // Minimum gain to prevent clipping (ix > 8191):
    // peak34 * 2^(-3*(g-210)/16) + 0.4054 < 8191
    // → g > 210 - (16/3) * log2(8190 / peak34)
    int min_gain = 0;
    int max_gain = 255;
    if (peak34 > 0.0) {
        double g_min = 210.0 - (16.0 / 3.0) * std::log2(8190.0 / peak34);
        min_gain = static_cast<int>(std::ceil(g_min));
        if (min_gain < 0) min_gain = 0;

        // Tighten the search upper bound: gain where the peak quantizes to ~1.
        // peak34 * gain_table[g] + 0.4054 < 1.0
        // → g > 210 - (16/3)*log2(0.6/peak34)
        double g_max = 210.0 - (16.0 / 3.0) * std::log2(0.6 / peak34);
        int est = static_cast<int>(g_max) + 2;
        if (est < max_gain && est > min_gain) max_gain = est;
    }
    // VBR constant-quality floor: never quantize finer than the target gain.
    if (gain_floor > min_gain) min_gain = gain_floor;
    if (max_gain < min_gain) max_gain = min_gain;

    // When a search iteration accepts a gain (bits <= target_bits), its bit
    // count ran to completion (the early exit only fires when over the limit),
    // so that iteration's ix[]/regions/count are exactly what a fresh
    // quantize at that gain would produce. Snapshot the last accepted state
    // and skip the redundant final quantize+count.
    int lo = min_gain, hi = max_gain, best_gain = max_gain;
    int best_bits = -1;
    HuffRegions best_regions{};
    int16_t best_ix[576];
    for (int iter = 0; iter < 8 && lo <= hi; iter++) {
        int gain = (lo + hi) / 2;
        HuffRegions regs;
        int bits = quantize_and_count(mdct_in, info.ix, gain, info.scalefac,
                                       info.scalefac_scale, info.preflag, sr_index,
                                       &regs, &cache, short_block, target_bits);
        if (bits <= target_bits) {
            hi = gain - 1; best_gain = gain;
            best_bits = bits;
            best_regions = regs;
            std::memcpy(best_ix, info.ix, sizeof(best_ix));
        }
        else { lo = gain + 1; }
    }
    info.global_gain = best_gain;

    int huff_bits;
    if (best_bits >= 0) {
        std::memcpy(info.ix, best_ix, sizeof(best_ix));
        info.regions = best_regions;
        huff_bits = best_bits;
    } else {
        // No gain in the search range fit the budget (or the range was empty):
        // quantize at the fallback max_gain the search never evaluated.
        huff_bits = quantize_and_count(mdct_in, info.ix, best_gain,
                                       info.scalefac, info.scalefac_scale,
                                       info.preflag, sr_index, &info.regions,
                                       &cache, short_block);
    }
    info.part2_3_length = info.part2_length + huff_bits;

    // Budget guarantee: part2_3_length must fit both the per-granule bit
    // budget and the 12-bit side-info field. The gain search above counts
    // bits with the correct (long/short) region layout, so this rarely
    // triggers, but it is a hard safety net: coarsen the quantization
    // (raise global_gain) until the granule fits. Without it, an overflowing
    // part2_3_length wraps the 12-bit field and desyncs the decoder.
    {
        int limit = available_bits;
        if (limit > 4095) limit = 4095;  // 12-bit part2_3_length field
        while (info.part2_3_length > limit && info.global_gain < 255) {
            info.global_gain++;
            huff_bits = quantize_and_count(mdct_in, info.ix, info.global_gain,
                                           info.scalefac, info.scalefac_scale,
                                           info.preflag, sr_index, &info.regions,
                                           &cache, short_block);
            info.part2_3_length = info.part2_length + huff_bits;
        }
    }
}

static GranuleInfo quantize_base(const double* mdct_in, int available_bits,
                                 int sr_index, bool short_block,
                                 int gain_floor) {
    GranuleInfo info{};
    std::memset(&info, 0, sizeof(info));
    init_quant_tables();

    info.scalefac_compress = 0;
    info.part2_length = 0;

    // Assign scalefactors FIRST, then run a single gain search with them.
    {
        const int* sfb = tables::get_sfb_long_by_unified(sr_index);
        double band_energy[21], max_energy = 0.0;
        for (int band = 0; band < 21; band++) {
            double e = 0.0;
            for (int i = sfb[band]; i < sfb[band+1] && i < 576; i++)
                e += mdct_in[i] * mdct_in[i];
            band_energy[band] = e;
            if (e > max_energy) max_energy = e;
        }

        // Count active bands
        int active_bands = 0;
        if (max_energy > 0.0) {
            for (int band = 0; band < 21; band++)
                if (band_energy[band] / max_energy > 0.01) active_bands++;
        }

        if (max_energy > 0.0 && active_bands >= 3 && !short_block) {
            // Energy-based scalefactor assignment: give a little extra precision
            // to higher-energy bands. Modes 1/2 get their per-granule fidelity
            // from the scale search that wraps this base quantizer.
            bool any = false;
            for (int band = 0; band < 21; band++) {
                double ratio = band_energy[band] / max_energy;
                if (ratio > 0.01) {
                    int sf = static_cast<int>(ratio * 4.0 + 0.5);
                    if (sf > 7) sf = 7;
                    if (sf > 0) { info.scalefac[band] = sf; any = true; }
                }
            }
            if (any && !encode_scalefac_fields(info, sr_index)) {
                // Not representable in the scalefac_compress range: drop the
                // scalefactors rather than desync fields from values.
                std::memset(info.scalefac, 0, sizeof(info.scalefac));
            }
        }
    }

    gain_search_with_scalefacs(info, mdct_in, available_bits, sr_index,
                               short_block, gain_floor);
    return info;
}

// VBR quality 0-9 maps to target global_gain values (recalibrated after the
// pow34-curve fix: each gain step is ~1.1 dB of quantization noise; the old
// table's 194-230 range dead-zoned entire granules to silence).
// 0 (best): gain ~134 (fine quantization, many bits)
// 9 (worst): gain ~178 (coarse quantization, few bits)
static const int vbr_target_gain[10] = {
    134, 140, 144, 148, 152, 156, 161, 166, 172, 178
};

GranuleInfo quantize_granule_vbr(const double* mdct_in, int available_bits,
                                  int sr_index, int /*quality_mode*/,
                                  int vbr_quality, bool short_block) {
    // Same path as CBR (energy-based scalefactors + gain search under the
    // frame budget), with the search floored at the constant-quality target
    // gain: "the finest gain >= target that fits". The old VBR-only
    // preprocessing is gone: the psycho-model coefficient zeroing used the
    // shallow-slope masks that are unusable as an allocation target (see
    // compute_band_masks), and the headroom scalefactor assignment measured
    // as a no-op pre-fix and a regression post-fix.
    init_quant_tables();
    if (vbr_quality < 0) vbr_quality = 0;
    if (vbr_quality > 9) vbr_quality = 9;
    return quantize_base(mdct_in, available_bits, sr_index, short_block,
                         vbr_target_gain[vbr_quality]);
}

} // namespace glint

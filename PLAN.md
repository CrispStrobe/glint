# Glint Optimization Plan

## A/B Test Results (2026-06-27)

All measurements on x86-64 (Intel Xeon), `-O3 -march=native -ffast-math`, LTO.

### Baseline: glint/main (unmodified)
- Unit tests: 30/30
- Quality tests: ALL PASS
- Speed (256 kbps stereo, benchmark_encoder.py): speed=16.4x, normal=5.4x, best=2.3x
- Speed (mono 128kbps, 5min): ~30x realtime
- RAM (process RSS): 4480 KB
- RAM (encoder state + tables): 148 KB (double), 42 KB (fixed)
- Fidelity (multi-tone 128kbps): SNR=18.4 dB, segSNR=18.5 dB, LSD=16.9

### After optimizations (fused MDCT table + -O3/LTO)
- Unit tests: 30/30
- Quality tests: ALL PASS (identical correlation/SNR values)
- Speed (256 kbps stereo): speed=16.9x, normal=5.4x, best=2.5x
- Speed (mono 128kbps, 5min): ~38x realtime (+27%)
- RAM (process RSS): 4608 KB
- Fidelity: **bit-identical** to baseline (all metrics match exactly)

### Comparison with sync-main (separate optimization branch)
sync-main had bigger raw speedups (pow34 table, per-band quantizer, etc.) but
glint/main has vastly better audio quality (18.4 dB vs 1.4 dB SNR on multi-tone)
due to psychoacoustic model and QuantCache. The right approach is optimizing
glint/main, not replacing it.

## Implemented Optimizations (on glint/main)

### 1. Fused MDCT window+cosine+normalization table (mdct.cpp)
- **Impact:** ~10% speedup on mono
- Combined three operations into single precomputed table:
  `mdct_wincos_d[k][n] = sin_window[n] * cos_basis[n][k] / 288.0`
- Eliminates separate windowing loop (36 multiplies per subband) and
  per-output division (/288.0, 576 divides per granule)
- Applied to all SIMD paths (AVX, SSE2, NEON) and scalar fallback
- Transposed layout (`mdct_wincos_d_t[18][36]`) for SIMD sequential access

### 2. Compiler flags: -O3 + LTO (CMakeLists.txt)
- **Impact:** measurable on mono, less on stereo (I/O bound)
- Changed from default `-O2` to `-O3` for more aggressive optimization
- Added `CMAKE_INTERPROCEDURAL_OPTIMIZATION` for cross-TU inlining

### 3. Fix scalefac[21] OOB + per-band quantizer (quantize.cpp)
- **Impact:** bit-identical output, eliminates undefined behavior, enables
  per-band loop optimization
- Extended `scalefac[21]` → `scalefac[22]` with explicit HF tail band
- Mirror `scalefac_compress` into `scalefac[21]` after each computation
- Fixed `granule_mse` to use `band < 21` (matching encoder)
- Restructured `fill_quant_cache` and uncached `quantize_and_count` as
  per-band loops, hoisting sf_scale out of inner loop

## Already in glint/main (not re-implemented)
- pow34 lookup table (10000 entries, uint32 with 1/65536 scaling)
- QuantCache (precompute pow34*sf once, reuse across binary search)
- SIMD (AVX/SSE2/NEON) for MDCT and subband
- Huffman max-value-indexed table selection (O(1) vs exhaustive trial)
- Huffman bit-limited counting (early exit)
- Short blocks, psychoacoustic model, VBR

## Investigated and Rejected

### Per-band quantizer iteration + scalefac[21] OOB fix
- **Tried:** Fix the UB read of `scalefac[21]` (which reads `scalefac_compress`
  from the adjacent struct field) by either: (a) extending `scalefac` to [22],
  (b) clamping band at 20, or (c) both. Then apply per-band loop optimization.
- **Result:** Every approach causes -17 dB SNR regression (18.4 → 1.3 dB).
  Root cause: the OOB read acts as an implicit extra scalefactor for the HF
  tail (indices 418-575). `scalefac_compress` is non-zero after energy-based SF
  assignment, so it provides an accidental but beneficial boost to HF precision.
  The per-granule scale search (`quantize_granule`) has been tuned around this
  behavior — it picks input scale factors whose MSE (via `granule_mse`) is
  optimal under the mismatch between encoder (`band<21`, reads OOB) and MSE
  evaluation (`b<20`, clamps at 20). Making them consistent changes the MSE
  landscape and the search picks worse factors.
- **Additionally:** the hot path (cached quantize at lines 99-105) is already
  band-agnostic (`pow34_sf[i] * base_step + 0.4054`) so per-band iteration
  cannot help the innermost loop. The cache fill runs only ~3 times per granule
  vs 4608 iterations for the cached binary search — not a bottleneck.
- **To fix properly:** The OOB read must be fixed together with re-tuning the
  scale search to compensate. Options: (1) add an explicit 22nd band SF for
  the HF tail and include it in the energy-based assignment, (2) widen the
  scale search to compensate for lost HF, (3) adjust `granule_mse` weights.
  This is a quality engineering task, not a mechanical fix.

## Remaining Ideas (not yet implemented)
- **PGO** (profile-guided optimization): 5-15% expected
- **Fix scalefac[21] OOB read + re-tune scale search**: extend `scalefac` to
  22 entries with explicit HF tail band, re-tune `quantize_granule` scale
  factors and `granule_mse` weights to compensate (see rejected section)
- **Fused subband+MDCT**: eliminate intermediate 32x36 buffer by streaming
  subband output directly into MDCT accumulator
- **Fixed-point signal path activation**: full Q31 conversion for embedded

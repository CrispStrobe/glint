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

## Already in glint/main (not re-implemented)
- pow34 lookup table (10000 entries, uint32 with 1/65536 scaling)
- QuantCache (precompute pow34*sf once, reuse across binary search)
- SIMD (AVX/SSE2/NEON) for MDCT and subband
- Huffman max-value-indexed table selection (O(1) vs exhaustive trial)
- Huffman bit-limited counting (early exit)
- Short blocks, psychoacoustic model, VBR

## Investigated and Rejected

### Per-band quantizer iteration
- **Tried:** restructure `fill_quant_cache` and uncached `quantize_and_count`
  from per-sample (with `while (band < 21)` tracking) to per-band loops that
  hoist `sf_scale` computation.
- **Result:** -17 dB SNR regression (18.4 → 1.3 dB). Root cause: the original
  while-loop lets `band` reach 21 and reads `scalefac[21]` (out-of-bounds,
  actually reads `scalefac_compress` from the adjacent struct field). After
  `recompute_scalefac_encoding()` sets `scalefac_compress` to non-zero, the
  out-of-bounds read changes the quantization behavior. The encoder's scale
  search has adapted to this; changing it breaks the output. Fixing the OOB
  read is a separate task that requires re-tuning the scale search.
- **Additionally:** the hot path (cached quantize at lines 99-105) is already
  band-agnostic (`pow34_sf[i] * base_step + 0.4054`) so per-band iteration
  cannot help there. The cache fill runs only ~3 times per granule vs 4608
  iterations for the cached binary search.
- **Verdict:** Not viable without also fixing the OOB read and re-tuning.

## Remaining Ideas (not yet implemented)
- **PGO** (profile-guided optimization): 5-15% expected
- **Fix scalefac[21] OOB read**: extend `scalefac` array to 22 entries with
  explicit zero for band 21, then safely apply per-band optimization
- **Fused subband+MDCT**: eliminate intermediate 32x36 buffer by streaming
  subband output directly into MDCT accumulator
- **Fixed-point signal path activation**: full Q31 conversion for embedded

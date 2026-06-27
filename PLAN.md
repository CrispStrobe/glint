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

## Remaining Ideas (not yet implemented)
- **PGO** (profile-guided optimization): 5-15% expected
- **Per-band quantizer iteration**: restructure `quantize_and_count` inner
  loop from per-sample with while-loop to per-band. Hoists `step*sf_scale`.
  Already in sync-main, gave ~15-30% speedup — needs careful porting to
  glint/main's QuantCache-based path.
- **Fused subband+MDCT**: eliminate intermediate 32x36 buffer by streaming
  subband output directly into MDCT accumulator
- **Fixed-point signal path activation**: full Q31 conversion for embedded

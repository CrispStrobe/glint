# Glint Optimization Plan

## A/B Test Results (2026-06-27)

All measurements on x86-64 (Intel Xeon), `-O3 -march=native -ffast-math`, LTO.

### Baseline: glint/main (unmodified)
- Unit tests: 30/30
- Quality tests: ALL PASS
- Speed (256 kbps stereo, benchmark_encoder.py): speed=16.4x, normal=5.4x, best=2.3x
- Speed (mono 128kbps, 5min): ~30x realtime
- RAM (encoder state + tables): 148 KB (double), 42 KB (fixed)
- Fidelity (multi-tone 128kbps): SNR=18.4 dB, segSNR=18.5 dB

### After all optimizations
- Unit tests: 30/30
- Quality tests: ALL PASS
- Speed (256 kbps stereo): speed=23.7x, normal=13.0x, best=5.0x
- Speed (mono 128kbps, 5min): ~80x realtime (+167%)
- RAM (encoder state + tables): 188 KB (double), 46 KB (fixed)
- Fidelity (256 kbps stereo): SNR 14.8–15.2 dB, all tiers within thresholds

### Speed improvement summary (256 kbps stereo)

| Mode | Baseline | Optimized | Improvement |
|---|---|---|---|
| speed | 16.4x | **23.7x** | +45% |
| normal | 5.4x | **13.0x** | +141% |
| best | 2.3x | **5.0x** | +117% |

## Implemented Optimizations

### 1. Fused MDCT window+cosine+normalization table (mdct.cpp)
- **Impact:** ~10% speedup on mono
- Combined three operations into single precomputed table:
  `mdct_wincos_d[k][n] = sin_window[n] * cos_basis[n][k] / 288.0`
- Eliminates separate windowing loop (36 multiplies per subband) and
  per-output division (/288.0, 576 divides per granule)
- Applied to all SIMD paths (AVX, SSE2, NEON) and scalar fallback
- Transposed layout (`mdct_wincos_d_t[18][36]`) for SIMD sequential access

### 2. Compiler flags: -O3 + LTO (CMakeLists.txt)
- **Impact:** ~5-10%
- Changed from default `-O2` to `-O3` for more aggressive optimization
- Added `CMAKE_INTERPROCEDURAL_OPTIMIZATION` for cross-TU inlining

### 3. Fix scalefac[21] OOB + per-band quantizer (quantize.cpp)
- **Impact:** eliminates undefined behavior, bit-identical output
- Extended `scalefac[21]` → `scalefac[22]` with explicit HF tail band
- Mirror `scalefac_compress` into `scalefac[21]` after each computation,
  preserving the empirically beneficial HF boost as defined behavior
- Fixed `granule_mse` to use `band < 21` (matching encoder)
- Restructured `fill_quant_cache` and uncached `quantize_and_count` as
  per-band loops, hoisting sf_scale out of inner loop

### 4. Double pow34 table (tables.hpp, quantize.cpp)
- **Impact:** ~3% speedup, eliminates int-to-double conversions
- Changed pow34 table from `uint32` (with `* 1.0/65536.0` on every access)
  to `double` (direct load)
- Trade-off: +39 KB RAM (78 KB vs 39 KB for 10000-entry table)

### 5. granule_mse: cbrt + per-band (quantize.cpp)
- **Impact:** ~19% speedup (granule_mse: 15.8% → 8.9% of total time)
- Replace `std::pow(a, 4.0/3.0)` with `a * std::cbrt(a)` (~3x faster)
- Hoist `std::pow(2.0, -0.5*sf*...)` out of inner loop via per-band iteration
  (22 std::pow calls vs 576)
- Precompute `decoder_gain * sf_d` once per band

### 6. Share pow34 across scale search (quantize.cpp)
- **Impact:** ~16% speedup, reduces fill_quant_cache from 36% to ~7%
- Precompute `pow34(|mdct_in[i]|)` once in `quantize_granule`, pass to
  `quantize_base` → `fill_quant_cache` with `f^0.75` multiplier
- Exploits `pow34(|x*f|) = pow34(|x|) * f^0.75` to skip table lookups
- Reduces `fast_pow34` calls from ~24/frame to ~4/frame

### 7. Eliminate mdct_flat copy + backward rzero scan (encoder.cpp, quantize.cpp)
- **Impact:** minor cleanup, removes 576-double copy per granule
- `mdct_out[32][18]` is contiguous — pass pointer directly instead of copying
- Scan backward for rzero after quantize loop instead of per-element branch

### 8. Fused frequency inversion + MDCT (mdct.cpp, encoder.cpp)
- **Impact:** eliminates sub_gr[32][18] temporary and 576-double copy per granule
- New `MDCT::process_strided()` reads from `subband_out[32][36]` at slot offset,
  applying frequency inversion inline (negate odd sb × odd ts)
- Both double-precision encode paths use the fused method

### 9. Early-exit scale search (quantize.cpp)
- **Impact:** +15-20% for normal/best modes
- For ≥4 scale factors, stop searching once MSE rises 3× past the best
  and is monotonically increasing (past the optimum)
- Typical savings: 1-2 fewer quantize_base calls for normal, 3-5 for best

### 10. Tighter binary search upper bound (quantize.cpp)
- **Impact:** minor, reduces wasted iterations at high gain values
- Estimate `max_gain` where peak coefficient quantizes to ~1 bit
- Narrows the `[min_gain, 255]` search range

## Remaining Ideas
- **PGO** (profile-guided optimization): ~5% expected, build process change
- **Fused subband+MDCT**: eliminate intermediate 32x36 buffer by streaming
  subband output directly into MDCT accumulator
- **Fixed-point signal path activation**: full Q31 conversion for embedded

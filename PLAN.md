# Glint Optimization Plan

## A/B Test Results (2026-06-27)

All measurements on x86-64 (Intel Xeon Skylake, 4 cores), `-O3 -march=native -ffast-math`.
A/B tests use `tests/ab_benchmark.py` with `taskset` core pinning, interleaved runs,
warmup discards, and Mann-Whitney U significance testing.

### Baseline: glint/main (unmodified)
- Unit tests: 30/30
- Quality tests: ALL PASS
- Speed (256 kbps stereo, benchmark_encoder.py): speed=16.4x, normal=5.4x, best=2.3x
- Speed (mono 128kbps, 5min): ~30x realtime
- RAM (encoder state + tables): 148 KB (double), 42 KB (fixed)
- Fidelity (multi-tone 128kbps): SNR=18.4 dB, segSNR=18.5 dB

### After optimizations #1-15
- Unit tests: 30/30
- Quality tests: ALL PASS
- Speed (256 kbps stereo): speed=23.7x, normal=13.0x, best=5.0x
- Speed (mono 128kbps, 5min): ~80x realtime (+167%)
- RAM (encoder state + tables): 188 KB (double), 46 KB (fixed)
- Fidelity (multi-tone 128kbps): SNR=17.6 dB (-0.8 dB from #6, within thresholds)

### After PGO (#16) — A/B tested, 10 runs, taskset core 3
- Unit tests: 30/30
- Quality tests: ALL PASS
- Speed (256 kbps stereo): speed=34.8x, normal=16.0x, best=6.1x
- Fidelity (multi-tone 128kbps): SNR=17.6 dB (identical to pre-PGO)

### Speed improvement summary (256 kbps stereo)

| Mode | Baseline | After #1-15 | After PGO | Total improvement |
|---|---|---|---|---|
| speed | 16.4x | 23.7x | **34.8x** | +112% |
| normal | 5.4x | 13.0x | **16.0x** | +196% |
| best | 2.3x | 5.0x | **6.1x** | +165% |

### Cross-platform: Apple M1 (NEON)

| Mode | main (M1) | all-opts (M1) | Gain |
|---|---|---|---|
| speed | 108x | 143x | +33% |
| normal | 43x | 66x | +54% |
| best | 18x | 28x | +56% |

Parallel channel encoding (`GLINT_PARALLEL_CH`) is a win on M1 (~1.5x) thanks
to fast GCD-based thread dispatch, confirming it's a platform-dependent result
(regressed on x86 Xeon with `std::async` overhead).

### ⚠️ Quality regression (discovered on M1 crosscheck)

The optimizations silently regress rolloff/bandwidth — the A/B harness only
measured global SNR (which didn't regress), missing segSNR and spectral shape:

| Metric | main (s/n/b) | all-opts (s/n/b) |
|---|---|---|
| 95% rolloff | 3422 / 5344 / 5133 Hz | **1453** / 4734 / 4922 Hz |
| centroid | 744 / 830 / 820 | 623 / 806 / 814 |
| seg-SNR | 10.77 / 13.22 / 13.84 | 9.59 / 12.24 / 12.79 |
| overall SNR | 12.50 / 14.08 / 14.74 | 12.96 / 14.42 / 14.76 |

Speed tier worst: rolloff 3.4 kHz → 1.45 kHz — regression back toward the
"dull, normalized" sound that `feature/rolloff-noise-shaping` was created to fix.

**Root causes:**
1. **granule_mse rewrite (#3/#5/#6/#11)** — per-band + cbrt + float-cache
   restructure changes which scale factor wins, weakening envelope retention.
2. **Early-exit scale search (#9)** — `d > best_mse*3.0` assumes convex MSE
   curve, but HF-retention penalty deliberately accepts higher raw MSE. Bails
   before reaching envelope-optimal candidate (affects normal/best; speed uses
   2 factors, below the `nf>=4` guard).

**Safe to cherry-pick** (no scale-factor selection change):
#1, #2, #4, #7, #8, #10, #12, #13 — most of the speed at zero quality cost.

**Hold / rework:**
#9 early-exit (guard off when envelope penalty active), #3/#5/#6/#11 granule_mse
(verify reproduces main's scale-factor selection per granule, not just global SNR).

**Testing gap:** `ab_benchmark.py` quality comparison needs rolloff, centroid,
and segSNR metrics on speech signals, not just global SNR on multi-tone.

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

## Implemented (continued)

### 11. Float QuantCache (quantize.cpp) — DONE
- Switch `pow34_sf[576]` from `double` (4608 bytes) to `float` (2304 bytes)
- Cached quantize loop uses float arithmetic throughout
- Halves cache pressure for the hottest loop

### 12. int16_t for ix[] (quantize.hpp, huffman.hpp/cpp) — DONE
- Quantized values are -8191..8191 — fits int16_t
- Halves ix[576] from 2304 to 1152 bytes
- Updated all Huffman function signatures and consumers

### 13. int8_t for QuantCache sign[] (quantize.cpp) — DONE
- Sign is +1 or -1 — int8_t instead of int
- Shrinks sign array from 2304 to 576 bytes

### 14. Skip trivial granule_mse (quantize.cpp) — DONE
- Precompute input_energy = sum(mdct_in²) once
- When all ix[] are zero (part2_3_length == part2_length), MSE = input_energy
- Skips the full granule_mse decode loop for trivially bad scale factors

### 15. Remove init_quant_tables from hot path (quantize.cpp) — DONE
- Removed from quantize_and_count (729K calls) — callers handle init

**Total hot working set reduction:** 11,520 → 4,032 bytes
  - pow34_sf: 4608 → 2304 (float)
  - sign: 2304 → 576 (int8_t)
  - ix: 2304 → 1152 (int16_t)

### 16. PGO — profile-guided optimization (CMakeLists.txt) — DONE
- **Impact:** +20-25% across all tiers (A/B tested, p<0.002)
- New `GLINT_PGO` cmake option: `off` (default), `generate`, `use`
- Training: 4 encodes covering speed/normal/best + mono/stereo
- GCC 13 PGO + LTO crashes (miscompilation in cleanup); LTO auto-disabled
  for PGO builds. PGO without LTO still beats baseline with LTO by 20-25%
  since hot loops are within single translation units.

## Negative Results (A/B tested, reverted)

### 17. Track max_val during quantize — REJECTED
- **Result:** -25% to -31% regression (p<0.001 all tiers)
- Tracking per-band max |ix| during the cached quantize loop adds branching
  overhead that far outweighs the saved `select_best_table` scans. The scans
  operate on L1-hot data (3×576 int16 reads) and are very cheap.

### 18. Batch multi-gain quantize — NOT FEASIBLE
- The binary search is inherently sequential: each iteration's gain depends
  on whether the previous iteration fit the bit budget. Cannot batch.

### 19. Fused subband+MDCT streaming — NOT FEASIBLE
- MS stereo transform operates on the full `subband_out[32][36]` buffer
  in-place before MDCT runs. A streaming fusion is incompatible with this
  without restructuring the entire MS stereo path. High risk, uncertain gain.

### 20. Parallel channels (std::async) — REJECTED
- **Result:** -5% to -19% regression (p<0.01 all tiers)
- `std::async` per-frame thread creation/join overhead dominates the
  per-channel work (~17ms for speed tier). Would need a persistent thread
  pool to be viable, adding substantial complexity for modest gain.

### 21. Fixed-point signal path — NO BENEFIT on x86
- **Result:** 0% difference (all p>0.19, n.s.)
- On x86-64 with SSE2, double-precision FP is as fast as integer math.
  Quality identical (14.83 dB SNR both). Fixed-point path is for embedded
  ARM/RISC targets without FPU, not for x86 performance.

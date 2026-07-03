# Quality improvement plan

## 0. ~15 dB SNR ceiling — RESOLVED (pow34 curve bug, fixed on main)

**Root cause:** `fast_pow34` interpolated x^0.75 on an *integer* grid, but
quantizer inputs are fractional MDCT coefficients — on (0,1) it degenerated
to pow34(x)=x, so ix was linear in x and the decoder's ix^(4/3) warped the
spectrum by x^(4/3). Signal-proportional error ⇒ SNR flat vs bitrate. The
scale-search grids (1.3–4.2) and the old "288/194 gain correction" existed to
compensate this curve. Secondary bug fixed too: gain clipping bounds ignored
the scalefactor boost in the quant cache.

**After the fix** (256 kbps speech): SNR 34.5–34.7 dB all tiers (was
12.5–14.7), seg-SNR 37.4, LSD 6.7, mean NMR −8.6 dB / 1.9% audible, rolloff
= source. Music: electronic 29.2 dB, quartet 27.2 dB. A 441 Hz sine: 79 dB.
LAME on the same clips: 36.9/44.5/46.0 SNR, NMR −16/−15.8/−11.1 — glint is
now in the same league, ~2–15 dB behind depending on content.

**Follow-ups opened by the fix:**
- The tiers barely differ now (34.51/34.55/34.69) — the extra `-q best`
  machinery was compensation. Re-differentiate the tiers with real work
  (items 3/5).
- `kNmrOuterLoop` and `kGranuleRedistribution` disabled — both regressed
  post-fix (−0.7 / −1.9 dB); re-tune to the new noise floor before
  re-enabling.
- `fast_pow34` is now plain `std::pow` — restore a fast path (frexp-based
  mantissa table) if profiling shows it hot; encode times were unchanged.
- VBR is mis-calibrated post-fix (`vbr_target_gain` tuned for the old
  curve): 20.6 dB / NMR +5.3 on speech. Recalibrate the target-gain table.
- White-noise pathology: full-scale white noise decodes +11 dB hot and
  clipped (pre-existing; LAME also only manages 10 dB here). Suspect the
  +0.4054 rounding bias at ultra-coarse quantization when the 4095-bit cap
  binds, times the scale search. Edge case, but worth a look with item 5.
- Re-tune post-fix: envelope-retention penalty in granule_mse, the
  45/55 channel-split clamp, and the scale-search grids/tier widths were
  all tuned against the broken curve.

### Original diagnosis (kept for the record)

Measured on identical inputs at 256 kbps CBR (LAME via ffmpeg libmp3lame,
`tests/measure_audio.py`, NMR metric):

| clip (60 s, 44.1k stereo) | metric | LAME | glint `-q best` |
|---|---|---|---|
| speech ref | SNR / LSD / mean NMR | 36.9 / 4.6 / **−16.1** | 14.9 / 12.0 / **+5.1** |
| electronic (bass-heavy) | SNR / mean NMR | 44.5 / −15.8 | 14.7 / +6.3 |
| string quartet | SNR / mean NMR | 46.0 / −11.1 | 14.2 / +5.3 |

256 kbps stereo should be near-transparent; glint plateaus ~14-15 dB SNR on
every content type while spending the same bits. This is structural, not
tuning. Diagnostic signatures to chase:
- **Mid-band starvation**: bandSNR 1-4 kHz is 3.5-8 dB on music (LAME: 24)
  even though little total error power sits there — the 1-4k content is
  simply gone. Rolloff on the quartet: 603 Hz vs 1141 Hz source; on speech
  it's fine (tuned there). The envelope-retention penalty and scale search
  were tuned on speech and do not generalize.
- **Noise placement is inverted vs LAME**: glint puts 64-85% of error power
  in 0-1 kHz; LAME pushes ~50% above 8 kHz where masking absorbs it.
- **`-q speed` collapses on bass-heavy content**: 6.7 dB SNR (electronic).
- The per-granule input-scale search (factors 1.0-4.2) reconstructs ≈ f·x
  and relies on dead-zone down-bias to land near the right level — check
  whether a systematic MDCT/quantizer normalization mismatch is being
  papered over by the factor search; if so small coefficients die in the
  dead zone by construction and no amount of scalefactor work will fix it.
- Check per-granule budget utilization (sum part2_3_length vs
  available_bits): if utilization is high, the bits are being spent on the
  wrong things (huge low-band ix values?); if low, the gain search is
  leaving budget on the table.

Prioritized quality (not speed) work. Anchor facts from the 256 kbps speech
measurements: 65–74% of all error power sits in 0–1 kHz, and several standard
MP3 bitstream tools are unused (`preflag`/`scalefac_scale` never set, Huffman
tables chosen by max-value only, M/S all-or-nothing with a fixed 50/50 channel
bit split).

Gate for every item: `tests/measure_audio.py` on the canonical speech clip
(SNR/seg-SNR/LSD/rolloff/centroid must improve or hold), `ffmpeg` backstep
check (zero), unit tests, `fixed` path metrics match `double`. (The two
signal paths have never been byte-identical to each other; "double==fixed" in
the docs means metrics-identical.) Quality changes are *not* byte-identical
to the previous main by design — that's what the metrics gate is for.

## 1. Real Huffman table selection — DONE (merged)

`choose_huff_table` picked one fixed table per max-value class (always 7 never
8/9, always 10 never 11/12, always 13 never 15, first ESC table with enough
linbits, never the 24–31 family). Count actual bits for every candidate in
the same-range group and pick the minimum. Hot path uses a fused
`huffman_select_and_count` (one pass accumulates all candidate totals; the
running minimum keeps the bit_limit early exit exact) — the naive version
cost +22–40% encode time, the fused one +5–7%. Every saved bit lets the gain
search land on a finer global_gain at the same bitrate.
Measured (speech, 256 kbps): SNR +0.01..0.03 dB, seg-SNR +0.01..0.04 dB,
LSD −0.5 dB at all tiers, rolloff +23/+70 Hz (normal/best).

## 2. Smarter bit distribution — DONE (merged)

- Per-granule channel bit split proportional to post-transform energy,
  clamped to **45–55%** (`kChSplitLo/Hi` in encoder.cpp), integer-exact (the
  shares sum to the old 50/50 total). Was a fixed 50/50.
  Measured (speech, 256 kbps, vs post-#1): joint speed +0.13 dB SNR /
  LSD −0.96; joint normal +0.03 / −0.40; joint best −0.01 / −0.37 with
  seg-SNR +0.02; stereo +0.03..0.05 dB SNR at all tiers.
  **Clamp tuning matters**: 25/75 regressed joint best SNR by 1.5 dB (side
  channel starved in loud passages — its error adds directly to decoded L/R
  even when mid-band metrics improve); 42/58 still −0.09 dB. 45/55 is the
  everything-improves-or-holds point on the speech clip. Re-tune with real
  wide-stereo music before trusting it beyond speech.
- Inter-granule redistribution now uses both channels' energy (was ch 0
  only). Extending it from best-only to normal was measured and regressed
  (joint normal −0.05 dB SNR, seg-SNR and LSD worse), so it stays best-only
  with the 30/70 clamp.
- Music check (electronic + string quartet, see item 0): joint ≈ stereo on
  both clips, so the 45/55 clamp holds up beyond speech.

## 3. Outer-loop noise shaping (NMR-driven) — DONE at `-q best` (merged)

`nmr_outer_loop` in quantize.cpp: amplify scalefactors of bands whose
reconstruction noise exceeds a per-band mask, re-run the gain search so
global_gain coarsens to pay (the mechanism both failed branches lacked —
they kept the gain fixed), keep the best iterate by (bands-over-mask,
log-excess), ≤10 iterations, MPEG-1 long blocks.
Measured: best-tier SNR +0.09/+0.10 dB (stereo/joint), seg-SNR +0.07,
mean NMR 5.12 → 5.06 dB. Cost +64% encode time at best.

**Learned the hard way:**
- The `psycho.cpp` model is unusable as an allocation target: 1.5–3 dB/Bark
  slopes + excluded self-masking let one loud band "mask" the whole
  spectrum; scoring with it drove SNR to −4 dB (HF polished, low bands
  destroyed). The loop uses its own masks (−20 dB self SMR, 15 dB/band
  spreading, ATH floor).
- A total-noise guard (≤1.15× the start's noise) is load-bearing: the loop
  may redistribute noise, never grow it. Loosening to 1.3× changed nothing —
  the loop exits via convergence, not the guard, once masks are sane.
- At `-q normal` the loop finds nothing the guard allows (2× cost, zero
  gain) — it stays best-only; speed/normal remain byte-identical.

**Still open here:** `scalefac_scale=1` when sf range is too narrow,
`preflag` for the HF preemphasis pattern, fixing the perceptual inversion in
the CBR energy-based scalefactor seed (loud bands get extra precision —
backwards), and making the loop pay off at `-q normal`.

## 4. Perceptual scale-search objective — TODO (pairs with 3)

`granule_mse` optimizes raw MSE + envelope-retention heuristic. Weight
per-band noise by the masking threshold (minimize NMR, not MSE) using the same
search machinery. Needs metric support (see 6) to evaluate honestly.

## 5. Bit reservoir rate control → short blocks — TODO (biggest ceiling, design-heavy)

Mechanism is verified on `feature/bit-reservoir` (0 backstep, transparent with
conservative budget). Missing: the policy — per-granule perceptual-entropy
target, save-on-easy/borrow-on-hard, cap per-frame borrowing so it stops
regressing low bitrates (96 kbps best: 14.0 → 8.1 dB with the naive policy).
Once merged, re-enable short blocks (`kShortBlocksEnabled`) — plosives
currently smear through long blocks (pre-echo); short blocks cost 4–9 dB
without a reservoir to fund them, so the order is reservoir first.

## 6. Perceptual measurement — DONE (NMR in measure_audio.py, merged)

`nmr_metrics` in `tests/measure_audio.py`: Bark-band noise-to-mask with
Schroeder spreading, −14 dB offset, ATH-shaped floor calibrated ~96 dB below
the loudest band-frame. Reports mean/p95 NMR and % band-frames over mask.
Ranks tiers correctly (speed 6.6 > normal 5.3 > best 5.1 dB mean at
256 kbps). Calibration is relative — compare builds on the same reference.
Still open: ViSQOL/PEAQ for a MOS-like score; A/B against LAME at 256 kbps.

## 7. MPEG-2/2.5 path is broken — TODO (pre-existing, all rates ≤ 24 kHz)

CBR at 22050 Hz measures **−10 dB SNR** (decoded audio is garbage),
reproduced on pre-quality-work main — long-standing, not caused by recent
changes. Prime suspect: `encode_scalefac_compress_m2` returns 0 (encodes
nothing) whenever bands 16–20 carry a nonzero scalefactor (slen3 ≠ 0 fails
both supported sfc ranges), so written scalefactors and the decoder's
readback disagree. VBR at m2 rates additionally emits an occasional
backstep frame. The unit tests pass without catching it — they don't
decode-verify m2 output; add a decode-based m2 test when fixing.

## 8. VBR — DONE for MPEG-1 (merged), see item 7 for m2

Real variable-size frames (smallest bitrate index that fits), unified with
the CBR quantizer path via a gain floor, target-gain table recalibrated
post-pow34-fix, and a budget bug fixed (VBR quantized under the caller's
default 128k frame budget instead of 320k). Speech ladder: V0 319 kbps /
39.2 dB / NMR −13.4 → V9 53 kbps / 23.3 dB. Follow-up: write a Xing/VBRI
header so players show correct duration/seek for VBR files.

## Smaller dials (experiment-sized)

- Per-frame M/S vs L/R decision (estimate bits/NMR both ways, pick per frame).
- Region boundary search (`region0_count`/`region1_count`) via per-sfb
  cumulative bit costs + DP over splits (LAME-style); the fixed heuristic
  wastes bits. Too hot to brute-force inside the gain search.
- Adaptive rounding offset (fixed 0.4054 dead-zone today; LAME adapts by
  tonality).
- VBR: replace the fixed `vbr_target_gain` table with NMR-driven allocation.

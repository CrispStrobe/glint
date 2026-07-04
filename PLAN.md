# Quality improvement plan

**Scoreboard** (256 kbps joint, `-q best`, vs LAME on identical inputs,
after the 2026-07 pass: shape-below-mask + attack-decay shorts + sfb21
lowpass + short-sfb table fixes + LSF shorts + shaping/rate-control
budget fix + region-split polish): speech SNR **38.0 vs 36.9** (glint
ahead), NMR −13.8 vs −16.1, audible band-frames 0.2% vs 0.0%;
electronic 43.5 vs 44.5 / NMR −15.9 vs −15.8 (tied); quartet 44.9 vs
46.0 / **NMR −14.0 vs −11.1 (glint ahead)**, audible 0.0%. Castanets (clip REGENERATED 2026-07 —
noise-burst train over a 220 Hz bed, `tests/gen_castanet.py`; harsher
than the old clip, absolute NMRs not comparable to older scoreboards)
128k: mean NMR 8.4 vs LAME 1.2, but **p95 −1.7 vs 2.6 and audible 2.5%
vs 6.2% (glint ahead on both)**; 256k −3.9 vs −8.6. MPEG-2 64k speech
**21.1 vs 17.6 SNR** / NMR 2.4 vs 2.4; m2-64k castanets 15.2/17.8 vs
14.7/16.5. VBR V0: 40.7 dB / NMR −15.3 / 0.0% audible, now with a Xing
header. Stereo speech tiers: 36.7/36.1/36.2 at NMR −8.6/−11.0/−11.1
(normal/best trade ~0.6 dB SNR for NMR vs the target-1.0 era).
**Remaining LAME gaps**: speech NMR tail (−13.5 vs −16.1; the shaping
target is exhausted — 0.0625 costs seg-SNR; next: per-band-frame outlier
control, adaptive rounding offset), castanets-128k MEAN (8.4 vs 1.2 —
p95/audible already ahead; the mean is dominated by the attack instant
itself), music SNR (elec −1.0, quartet −1.3 — masked per NMR).
Done in the 2026-07 pass (commit messages have full numbers): outer-loop
shaping target 0.125 = push bands ~9 dB below mask; kShortAttackExtend=2
decay granules; **sfb21 lowpass** (the region has no scalefactor ⇒
unshapeable quantizer spray; zeroing it wins NMR on every clip at every
rate incl. VBR); **short-sfb table fixes** (44.1k boundary 138→136 per
ISO; the m2 tables were MPEG-1 copies — the real cause of the historical
LSF collapse); LSF short blocks live (four-slen groups of 9, START/STOP
region0=54, wire-validated ffmpeg+CoreAudio); m2 long-granule shaping;
preemphasis[20] 3→2 per ISO; **shaping budget = spend + slack/2 and the
anchor EMA tracks pre-shaping rc_gain** (full-budget shaping drained the
reservoir signal and ratcheted the anchor coarse: stereo best −5 dB SNR;
slack·3/4 re-triggers it).
Measured dead ends: shaping guard >1.25 (speech −0.6 dB SNR for +0.3
NMR; short-loop-only 2.0 made castanets WORSE — short masks lack temporal
masking); mixed-frame bit tilt toward the short granule (worse on every
metric); attack-only sfb21 zeroing (keeps p95 win, loses mean win);
transient frames taking the full reservoir + dropped gain floor; reservoir
fill target 60–90% (+0.4 SNR / −0.9 NMR speech). Do NOT shape start/stop
granules (types 1/3): +3 dB NMR.

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
- ~~VBR mis-calibrated post-fix~~ — done, see item 8.
- White-noise pathology: full-scale white noise decodes +11 dB hot and
  clipped (pre-existing; LAME also only manages 10 dB here). Suspect the
  +0.4054 rounding bias at ultra-coarse quantization when the 4095-bit cap
  binds, times the scale search. Edge case, but worth a look with item 5.
- ~~Re-tune envelope penalty~~ — measured obsolete post-fix and removed
  (granule_mse is pure reconstruction MSE now); `-q speed` skips the scale
  search entirely (f=1.0 always won). The 45/55 channel-split clamp holds on
  the music clips.

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

## 3. Outer-loop noise shaping — REBUILT as psy allocation (merged); see commit
`quality: psychoacoustic bit allocation`. Schroeder-Bark masks aligned with
the metric, Σ(noise/mask) objective, seedless start, side-channel excluded,
+9% encode time at best. The section below describes the FIRST version and
its lessons; superseded.

### First version (historical)

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

## 5. Bit reservoir + rate control — DONE (merged); short blocks still TODO

**Reservoir + rate control (merged):** the ReservoirStream mechanism
(continuous main-data stream, deferred frame emission, `finish_frame` unifies
both encode paths' emission) plus a buffer-feedback constant-quality
controller: each CBR frame may spend slot + reservoir (capped at one extra
slot), a per-frame gain floor (`rc_anchor`) stops gold-plating on easy
frames, and the anchor adapts ±1 gain step/frame from reservoir fill,
tethered to an EMA of achieved gains. Gains vs no-reservoir: speech 256k
joint 35.1 → 37.3 dB (now above LAME 256k's 36.9 on this clip), 128k +2.5,
96k +2.6, electronic 256k 39.7 → 43.2, quartet 44.4 → 45.2, m2-64k
18.8 → 20.8 (LAME: 17.6). Soft spot: stereo-speech mean NMR eased
−8.8 → −7.2 while SNR rose; consider an NMR-aware anchor.

**Short blocks — DONE for MPEG-1 (merged).** The blocker was a
region-layout bug: window-switching side info has no region counts and only
two table_selects (decoder hardwires region1 = [36, big_values·2)), but the
encoder computed region1_end = 44 from the long-sfb tables and encoded
everything past it as region2 with table 0 — which writes *nothing*. Every
coefficient above wire index 44 was silently dropped while decoders kept
reading. With the `window_switching` flag in HuffRegions forcing the
decoder's boundaries, an all-short sine encodes at 80.7 dB (long path: 79).
The forward transforms (long/start/short/stop windows, both MDCTs, TDAC
across every transition) were proven exact with a Python
perfect-reconstruction harness first — the math was never the problem.

Landed with it: START/STOP transition windows in the MDCT, a per-frame
window scheduler (shared across channels, as M/S requires), and a
**one-granule encoder lookahead** (last granule held back; glint_flush
releases it, +576 samples latency) so a START always precedes the transient.
Castanets at 256k: mean NMR 17.5 → −0.2 dB (noise at the masking threshold);
at 128k: 28.9 → 10.7 (LAME: 4.8 — remaining gap is bits). SNR drops on
transients by design (LAME shows the same signature); judge by NMR. Steady
content within 0.4 dB at unchanged NMR.

**Still open here:** (all resolved since)
- ~~MPEG-2/LSF short blocks~~ — DONE. The historical 20.8 → 8.6 dB collapse
  was the sfb_short_table_m2 being an MPEG-1 copy (reorder scrambled vs
  every real decoder), NOT the scalefac_compress semantics. With the ISO
  tables, the four-slen LSF field encoding (groups of 3 bands × 3 windows,
  9 values each), scalefac_s emission, and the START/STOP region0_end=54
  boundary (ffmpeg init_short_region), m2-64k speech measures 21.1 dB /
  NMR 1.40 (LAME 17.6 / 2.38) and castanets NMR 36.3 → 17.6.
  Wire-validated on ffmpeg + CoreAudio (identical metrics).
- ~~Short-block scalefactors / subblock_gain~~ — done earlier.
- ~~128k castanet gap~~ — attacked via kShortAttackExtend=2 + sfb21
  lowpass + the 44.1k short-table fix (see scoreboard).
- `GLINT_FORCE_SHORT=1` forces all-short for diagnostics.

## 6. Perceptual measurement — DONE (NMR in measure_audio.py, merged)

`nmr_metrics` in `tests/measure_audio.py`: Bark-band noise-to-mask with
Schroeder spreading, −14 dB offset, ATH-shaped floor calibrated ~96 dB below
the loudest band-frame. Reports mean/p95 NMR and % band-frames over mask.
Ranks tiers correctly (speed 6.6 > normal 5.3 > best 5.1 dB mean at
256 kbps). Calibration is relative — compare builds on the same reference.
Still open: ViSQOL/PEAQ for a MOS-like score; A/B against LAME at 256 kbps.

## 7. MPEG-2/2.5 path — FIXED (merged)

Root cause was an invented `scalefac_compress` mapping
(sfc = slen0·36 + slen1·6 + slen2), internally consistent between glint's
encode and emission but not ISO 13818-3 — every real decoder derived
different slens and misparsed the granule (−10 dB SNR garbage,
long-standing). Both sides now use the standard sfc<400 mapping.
Result: CBR-64k at 22.05 kHz measures 18.8 dB SNR (LAME 64k: 17.6 —
glint ahead), VBR V4 31.5 dB, 0 backstep.

**Bonus find while fixing:** the quantizer band loop read `scalefac[21]`
out of bounds for the sfb21 region (bins ≥ sfb[21], ~9 kHz+ at 44.1k) —
aliasing scalefac_compress as a phantom ~29× boost with no decoder-side
compensation. Fixing it (sfb21 has no scalefactor, period) was worth
+0.3 dB/−1.4 LSD on speech and **+8 to +17 dB SNR on music**:
electronic 29.2 → 39.7, quartet 27.2 → 44.4 dB (LAME: 44.5/46.0).
Follow-up: add a decode-based m2 test — the unit suite never caught any
of this.

## 8. VBR — DONE (merged), MPEG-1 and MPEG-2

Real variable-size frames (smallest bitrate index that fits), unified with
the CBR quantizer path via a gain floor, target-gain table recalibrated
post-pow34-fix, and two budget bugs fixed (VBR quantized under the caller's
default 128k frame budget instead of the max; and budgets assumed the CBR
padding byte, overflowing the largest unpadded frame ~2×/min). Speech
ladder: V0 319 kbps / 39.2 dB / NMR −13.4 → V9 53 kbps / 23.3 dB; MPEG-2
V4 at 22.05k: 31.5 dB. Follow-up: write a Xing/VBRI header so players show
correct duration/seek for VBR files.

## Smaller dials (experiment-sized)

(Absorbed into the follow-up queue below.)

## 9. Follow-up queue (2026-07, prioritized)

Work these in order; record measured results (or dead-end learnings) per
item, in place. Gate: the standard battery (speech + music + castanets +
m2, joint/stereo, CBR/VBR), 0 backstep, unit tests, double==fixed metrics.

1. **Decode-based m2 regression test** — DONE. `test_m2_decode` in
   tests/test_quality.py (also `--m2-only`), registered in ctest as
   `m2_decode_quality` when python3+numpy and ffmpeg exist (~3 s, FFT
   alignment). Three cases at 22.05k: CBR-64k best (21.5 dB), forced
   all-short (22.7 dB — pins the LSF short wire format), VBR V4
   (24.9 dB); floors 16/16/18. Verified discriminative: re-breaking the
   22.05k short-sfb table drops allshort to 11.4 dB -> FAIL. Note the
   test passes `-p double` explicitly — the CLI default is the FIXED
   path, which has no short blocks.
2. **Adaptive rounding offset** — DEAD END (measured 2026-07). Global
   sweep (0.30/0.4054/0.50, 256k best): 0.4054 wins or ties on speech
   joint+stereo, quartet, castanets — the per-granule factor search
   already owns the level dimension. Per-granule offset candidates
   {0.30, 0.50} at the winning factor, picked by granule_mse: speech
   joint NMR −13.48→−13.36 (worse), stereo/castanets marginally worse,
   quartet ≈, at +2 quantize_base runs/granule. MSE cannot see what the
   NMR metric penalizes here. Reverted; per-BAND offsets (LAME's actual
   trick is tonality-dependent) would need a tonality estimate first.
3. **Per-band-frame outlier control** — DEAD END (measured 2026-07).
   Two alternative outer-loop objectives tried: Σlog(max(r, target))
   (matches the metric's mean-dB) and Σmax(r−target, 0) (audible-excess
   only). Log objective: castanets-128k mean 8.37→7.99 but everything
   else a hair WORSE (elec −15.92→−15.87, quartet −13.87→−13.81, m2
   speech 2.50→2.59, stereo SNR −0.07); excess objective: pure no-op.
   The castanets mean outliers are attack-frame-local — there the noise
   guard and shaping budget bind, not the objective's shape. Reverted.
4. **Xing/VBRI header for VBR** — DONE. VBR streams start with a silent
   placeholder frame (64 kbps-index size); `glint_vbr_header()` (public
   API, call after glint_flush) produces the finalized Xing frame —
   frames, bytes, 100-point TOC (up to 256 stored offsets with stride
   doubling, fixed memory) — and the CLI rewrites frame 0 with it.
   ffprobe now reports exact duration and true average bitrate (V0
   318.7k / V9 46k); streaming consumers that cannot seek get ~26 ms of
   leading silence instead of a bogus header. Metrics unchanged.
5. **Perceptual scale-search objective (old item 4)** — DEAD END
   (measured 2026-07, as the envelope-penalty history predicted).
   Weighting granule_mse per band by 1/mask (same masks as the outer
   loop): NMR improves only marginally (speech −13.48→−13.61, elec
   −15.92→−16.02) while SNR collapses — quartet 44.7→36.9 (−7.7 dB!),
   speech 37.7→35.4, 0-1k band-SNR −8 dB. The mask-relative objective
   happily degrades loud (self-masked) low bands for slivers of NMR;
   the outer loop already harvests those trades SAFELY because its
   total-noise guard bounds the damage — the factor search has no such
   guard. Raw MSE stays the factor-search objective. Reverted.
6. **Per-frame M/S vs L/R decision** — DEAD END as tried (2026-07).
   Energy-skew criterion (pick the representation concentrating more
   energy, letting the 45/55 bit split starve the cheap channel):
   regressions wherever it switched to L/R — quartet-256k 44.7→42.5 dB /
   NMR −13.9→−11.0, speech −13.5→−12.6; electronic never switched
   (identical). The test set has no truly decorrelated wide-stereo clip,
   so the upside is unmeasurable here while the criterion's mistakes are
   expensive. Revisit only with (a) a real wide-stereo test clip and
   (b) a trial-encode decision (quantize both ways, compare NMR), not an
   energy heuristic. Reverted.
7. **Region boundary search** — DONE (merged). `huffman_optimize_regions`
   enumerates all valid (region0_count, region1_count) splits over
   per-table prefix bit costs on the FINISHED granule (`polish_regions`
   at the end of quantize_granule/_vbr; the gain search keeps the cheap
   heuristic). Saved bits shrink part2_3_length and feed the reservoir:
   speech-256k joint **+0.31 dB SNR / NMR −13.48→−13.77**, stereo
   +0.34/−11.05, quartet 44.87/−14.01, m2-64k 21.06/2.41, VBR V4 −1.4%
   bytes at identical quality; electronic/castanets hold. 0 backstep,
   ffmpeg+CoreAudio validated.
8. **NMR-driven VBR allocation** — DEAD END as designed (2026-07).
   Per-granule floor = coarsest gain whose analytic band noise stays
   within mask*offset(V) (offset −9 dB at V0 … +18 dB at V9, binary
   search, pow34 precomputed). Result: NOT more efficient — at V4 it
   spends +15% bytes for better NMR, but the fixed table interpolated to
   EQUAL bytes still wins (quartet: table-V0 2399 kB/−15.6 vs psy-V4
   2385 kB/−15.0); at V9 the permissive offset drives the floor to gain
   ~220 and output collapses to near-silence. The fixed table + frame
   budget feedback is the better allocator; a real win would need the
   full outer-loop shaping in the VBR path (it currently has none), not
   a smarter floor. Reverted.
9. **White-noise pathology** — RESOLVED by earlier fixes / explained
   (2026-07). The "+11 dB hot and clipped" behavior no longer exists
   (killed by the pow34-curve and gain-clipping-bound fixes). Current
   full-scale white noise: raw SNR −2.4 dB looks alarming but decomposes
   into (a) the sfb21 lowpass deleting the ~28% of white-noise energy
   above 15.8 kHz — a structural corr ceiling of √0.72 ≈ 0.85, measured
   0.84, with per-band coherence 0.99 everywhere below the cutoff — and
   (b) a −1.6 dB level bias from the dead zone at ultra-coarse gains.
   Level- and bitrate-independent by construction. LAME measures 8.9 dB
   via its 17.5k lowpass + exact level. Not a defect worth machinery;
   revisit only if the level bias shows up on real content.
10. **fast_pow34 fast path** — NOT NEEDED (profiled 2026-07, macOS
    `sample` on -q best speech): std::pow is absent from the flat
    profile; exp2 (pow's backend plus the mask model's pow10) totals
    ~1.3%. The heat is in huffman_select_and_count / quantize_and_count
    / gain_search (~55%) as pass 3 found. No fast path.
11. **Speed re-measure + docs** — DONE. Interleaved A/B vs pre-pass
    main (2de393e) initially showed the quality pass cost
    +59%/+81%/+58% (speed/normal/best). Recouped (metrics-neutral,
    committed as PLAN 9.11): lazy per-table prefix memoization + suffix
    cost sharing in huffman_optimize_regions (byte-identical), no region
    polish at -q speed (was ~30% of that tier's profile), and a 3-stall
    early exit in both NMR outer loops (metrics identical ±0.01 dB).
    Final: speed +1.8%, normal +60%, best +33% vs pre-pass — the
    remainder is the shape-below-mask iterations themselves. Absolutes
    (M1, moderate load): ~260×/52×/34× realtime. README refreshed.

Not queued: ViSQOL/PEAQ (needs external tooling), sfb21 partial-keep at
high rates (blocked on a shapeable-sfb21 mechanism), mode-dependent
shaping target for stereo (only if a stereo-SNR use case shows up).

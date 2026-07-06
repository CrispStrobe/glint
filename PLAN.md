# Quality improvement plan

**Scoreboard** (vs LAME -q2 on identical inputs, after the 2026-07
passes through PLAN items 9/10 and the corpus-driven round of
2026-07-04). 128 kbps joint PEAQ-ODG league over the 10-clip corpus:
glint ahead on 8 (choir −1.28 vs −2.06, orchestral −0.75 vs −1.09,
quartet −0.46 vs −0.88, industrial −0.82 vs −1.00, piano −0.63 vs
−0.70, torture −0.32 vs −0.45, speech −1.24 vs −1.29, electronic
≈tied); LAME ahead on drums (−1.03 vs −1.18) and castanets-ODG (−0.29
vs −0.62) — though castanets mean NMR is now glint's at BOTH rates
(128k −2.1 vs 2.6; 256k −9.7 vs −8.6). 256k: everything transparent
(ODG ≈0), glint ahead on SNR throughout (speech 38.4 vs 36.9,
electronic 44.8/NMR −18.0 vs 44.5/−15.8, quartet 44.9/−13.7 vs
46.0/−11.2). 64k-stereo speech: ODG −3.17 vs −3.32 (glint; PESQ still
LAME's). VBR psy-shaped with gapless playback (Xing + LAME tag,
0-sample decode offset). fixed==double everywhere incl. transients. Castanets (clip REGENERATED 2026-07 —
noise-burst train over a 220 Hz bed, `tests/gen_castanet.py`; harsher
than the old clip, absolute NMRs not comparable to older scoreboards)
128k: mean NMR 8.4 vs LAME 1.2, but **p95 −1.7 vs 2.6 and audible 2.5%
vs 6.2% (glint ahead on both)**; 256k −3.9 vs −8.6. MPEG-2 64k speech
**21.1 vs 17.6 SNR** / NMR 2.4 vs 2.4; m2-64k castanets 15.2/17.8 vs
14.7/16.5. VBR V0: 40.7 dB / NMR −15.3 / 0.0% audible, now with a Xing
header. Stereo speech tiers: 36.7/36.1/36.2 at NMR −8.6/−11.0/−11.1
(normal/best trade ~0.6 dB SNR for NMR vs the target-1.0 era).
**RAM (2026-07-04)**: fixed+GLINT_SMALL_BUFFERS measures ~64 KB (22.6 KB
BSS + ~41.5 KB context after sizing frame buffers for every legal frame
incl. 320 kbps — the earlier 1024-byte cap silently overflowed there)
vs Shine's 95.9 KB — after removing dead
PsychoModel instances (10 KB), replacing the 64 KB cbrt_lut with a
65-float mantissa table under SMALL_BUFFERS (metrics-identical), single-
slotting the per-samplerate mask/window caches (−31 KB) and float
transition tables. The audit also found live memory corruption: VBR's
320 kbps budget frame overflowed the 1024-byte SMALL_BUFFERS assembler —
glint_create now caps/rejects unfittable configs. Desktop builds are
byte-unchanged.
**Remaining LAME gaps** (updated 2026-07-04): speech NMR tail (−13.7 vs
−16.1, in-house metric only — ODG calls speech-128 for glint and PESQ
for LAME; INFORMAL LISTENING 2026-07-04: the maintainer could not tell
glint-128 from LAME-128 on the speech clip, so the model contradiction
is below this listener's audibility threshold — do not spend further
tuning effort on speech-128 metric deltas of this size), the drums/castanets 128k ODG gap (−1.18/−0.62 vs LAME's
−1.03/−0.29 — attack CHARACTER, not bits: detector sensitivity at 4x/6x/8x
identical on drums, an HF-band (subband>=8) detector term fired on piano
hammers and speech consonants instead (ODG −0.63→−0.86 / −1.26→−1.48,
reverted), and banking/mixed-block premises measured out earlier; the
remaining suspects were LAME's pre-echo control and short-window tuning
— also now measured out: tightening pre-attack window masks inside
short granules is a wash (castanets +0.02 ODG, speech −0.02, drums
untouched — drums has only 1.6% shorts and LAME's own block counts are
similar at 2.7%; the drums gap is diffuse psymodel character, not any
single mechanism; investigation closed at diminishing returns),
quartet/piano SNR at 256k
(masked per NMR/ODG). ~~castanets mean NMR~~ — CLOSED 2026-07-04 by the
6x attack threshold: 128k 8.0→−2.1 (LAME 2.6 — glint ahead), 256k
−3.9→−9.7 (LAME −8.6 — glint ahead).
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

## 6c. Test clips — canonical set (extended 2026-07-04)

Two higher-quality lossless clips added (the electronic/quartet sources
are dark — rolloff 474/1141 Hz):
- `03_music_industrial_60s.wav` — NIN "19 Ghosts III" 60-120s, from the
  official 24bit/48k ALAC on archive.org (NineInchNailsGhostsI-Iv24bit48khz,
  CC BY-NC-SA 3.0); broadband to ~17 kHz, dense production. First
  standings: glint-128 ahead of LAME on everything (NMR −1.9 vs 0.0,
  audible 18 vs 31%, ODG −0.80 vs −1.00).
- `04_music_piano_60s.wav` — Kimiko Ishizaka, Open Goldberg Variations
  Var. 1 (OpenGoldbergVariations on archive.org, CC0, 24bit/96k FLAC).
  First standings: LAME slightly ahead at 128k (audible 1.4 vs 5.5%,
  ODG −0.70 vs −0.79) — a NEW measured weakness worth chasing.
Sources kept alongside as ghosts19_src.m4a / goldberg_var1_src.flac.
Corpus extended to 10 clips (2026-07-04, user request): additionally
- `05_music_orchestral_60s.wav` — Beethoven Eroica mvt 1 (Musopen
  `MusopenCollectionAsFlac`, PD mark, 24/48 FLAC), full orchestra.
- `06_music_drums_60s.wav` — NIN "22 Ghosts III" (same CC BY-NC-SA
  item), drum-forward.
- `07_music_choir_60s.wav` — The Tudor Consort, Animuccia Credo
  (archive.org, CC BY 3.0, 16/44.1 FLAC), a cappella polyphony.
- `08_torture_60s.wav` — deterministic synthetic (tests/gen_torture.py):
  log sweep, AM multitone, glockenspiel-like bursts, stepped band noise.
For more material: archive.org lossless items are the reliable pool
(Pixabay/FMA serve lossy MP3 downloads — unusable as codec references);
EBU SQAM is now behind an EBU login; Jamendo offers FLAC for CC tracks.

## 6b. Encoder league table — tests/compare_encoders.py (2026-07)

Automated comparison harness: encodes every clip with glint
(speed/normal/best), LAME (`lame -q2`/`-q0`), and Shine (`shineenc`,
build from github.com/toots/shine — fixed-point, no psymodel: a floor
baseline), decodes via ffmpeg, reports SNR/seg-SNR/NMR (+PESQ-WB and
STOI via pip `pesq`/`pystoi` for speech clips) and encode speed.
Standings at 2026-07 (CBR, joint):
- Music: glint ahead or tied at BOTH rates — electronic-128 decisively
  (NMR −6.3 vs LAME −3.1, audible 5.4% vs 16%), electronic-256 tied
  (−16.0 vs −16.1), quartet-128 −3.8 vs −2.0, quartet-256 −14.0 vs −11.2.
- Speech-256: LAME ahead on NMR (−15.5 vs −13.8), PESQ saturated/tied
  (4.62-4.64), glint ahead on SNR (38.0 vs 35.9).
- Speech-128: glint ahead on NMR (−2.9 vs −2.2) and audible (14% vs 20%)
  BUT **PESQ prefers LAME (4.57 vs 4.43)** — the first independent
  perceptual model disagreeing with our NMR ranking. Our NMR metric and
  the encoder share the mask model, so it is partially self-confirming;
  treat low-rate speech tuning wins as unproven until PESQ agrees.
  (PESQ is only discriminative below ~192k; at 256k everything including
  Shine scores 4.6.)
- Shine: 10-20 dB below everyone everywhere (psymodel-free floor ✓).
- glint-speed on quartet-128 (NMR −0.8 vs normal's −3.8) shows what the
  psy shaping buys.
Extended 2026-07: PEAQ basic-model ODG for every clip via peaqb-fast
(github.com/akinori-ito/peaqb-fast, plain autotools; peaqb does NOT
time-align, the harness feeds it an aligned 48 kHz 20 s pair or every
score saturates at −4), ViSQOL MOS-LQO via the Rust port
(`cargo install visqol`, fullband mode with google/visqol's libsvm
model file — the upstream bazel build no longer compiles against
current Xcode SDKs: bazel 5's crosstool aborts, bazel 6 removed the
platforms API its 2022 TF pin needs, and the cpp-only toolchain trips
over TF's zlib on the macOS 26 SDK), and a `--mode mono` ladder.
The tools live PERSISTENTLY in ~/code/glint-tools/ (peaqb-fast/, shine/
— static-built with --disable-shared, visqol-model/); the visqol binary
itself is ~/.cargo/bin/visqol. The harness defaults point there.
**Treat ViSQOL with caution on these clips**: it ranks Shine (audibly
the worst encoder by every other measure, NMR −3 vs −14) ABOVE glint
and LAME on quartet-256 (4.67 vs 4.38/4.63) and above glint on
electronic-256 — its fullband SVR model appears insensitive to MP3's
actual artifact classes here. ODG (PEAQ) and PESQ remain the trusted
external models; keep MOS as a gross-artifact tripwire only.
Mono-64k speech ladder standings: LAME ahead on BOTH perceptual models
(ODG −2.47 vs glint-normal −2.94, PESQ 4.44 vs 4.22) while glint wins
SNR/NMR — consistent with the 128k PESQ finding: below ~128k, speech
tuning wins measured only by our NMR are not to be trusted. At 96k
mono it's mixed (glint edges ODG −1.70 vs −1.74, LAME PESQ 4.58 vs
4.50).
Full joint table with ODG (2026-07): the perceptual models sharpen the
map. Quartet: glint clearly ahead on EVERY model (128k ODG −0.44 vs
LAME −0.88; 256k 0.02 vs −0.06). Electronic-128: our NMR says glint
decisively (−6.3 vs −3.1) but ODG gives LAME a slight edge (−0.22 vs
−0.31) — treat the electronic-128 NMR lead cautiously. Speech-128: the
two perceptual models DISAGREE with each other (ODG prefers glint
−1.24 vs −1.29; PESQ prefers LAME 4.57 vs 4.43) — call it a tie.
Castanets: LAME ahead perceptually at both rates (128k ODG −0.29 vs
−0.63) — glint's p95/audible-% advantages do not carry over to ODG,
so the castanets mean-NMR gap is the perceptually honest signal there.
256k everything-transparent (all ODG ~0). glint-speed's missing short
blocks show up as ODG −3.33 on castanets-128 (vs normal's −0.63).

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

Not queued: ViSQOL/PEAQ (needs external tooling — since done, §6b),
~~sfb21 partial-keep at high rates~~ (DONE 2026-07-04: content-aware
keep at >96 kbps/ch for LONG granules outside a post-transient window —
share>1e-3 of granule energy above the cut keeps it. Torture-sweep SNR
17.5→33.8 / ODG −0.45→−0.16, quartet ODG 0.02→0.08; castanets exactly
unchanged — keeping burst/decay HF re-created attack spray, hence the
block-type + transient-context gates), mode-dependent shaping target
for stereo (only if a stereo-SNR use case shows up).

## 10. Second follow-up queue (2026-07-04, prioritized)

Grounded in the league-table findings (PESQ+ODG both confirm the low-rate
speech gap; ODG confirms the castanets mean gap; ViSQOL is a tripwire
only). Gate: the standard battery PLUS — for anything targeting ≤128k
speech — PESQ and ODG must improve or hold; our in-house NMR alone is not
sufficient there (it shares the encoder's mask model).

1. **Bitrate-scaled lowpass** — DONE (merged). Cutoff by per-channel
   kbps (anchors PESQ/ODG-calibrated: 32/ch→9500 Hz, 48/ch→11000;
   ≥64/ch keeps the sfb21 band — a 12-13.5k cut there helped speech
   mildly but hurt music ODG), resolved once in glint_create to
   long/short wire start indices, snapping to the sample rate's sfb
   grid. 64k-stereo speech: ODG −3.76→−3.17 (LAME −3.32 — glint now
   AHEAD on ODG), PESQ 3.68→4.01 (LAME 4.21), audible 45→41%;
   96k-stereo: ODG −2.68→−2.45, PESQ 4.21→4.34, audible 30→22%.
   128k/256k outputs byte-identical (cut==sfb21 there); mono-64
   unchanged BY DESIGN (LAME also keeps ~full band at 64 kbps/ch — its
   remaining mono-64 PESQ lead is not bandwidth). Discovered en route:
   LAME's own low-rate lever is RESAMPLING (64k stereo → 24 kHz LSF,
   96k → 32 kHz); an auto-resample would be a CLI-level feature.
2. **Tonality-adaptive mask offset** — RESURRECTED and merged
   (2026-07-04), rate-gated. First measured as a dead end — but only
   because the ORIGINAL clips had no strongly tonal content. On the
   extended corpus it wins exactly where theory says: piano-128 ODG
   −0.79→−0.63 (overtaking LAME's −0.70), audible 5.5→3.3%; drums-128
   ODG −1.31→−1.18; quartet-256 +0.4 dB SNR. Elsewhere ≤±0.03 ODG, but
   it costs ~1 dB of in-house NMR at 256k (metric mismatch — our NMR
   uses the flat −14 dB offset) where everything is transparent anyway.
   Enabled at ≤96 kbps/channel (128k stereo/joint and below), same
   philosophy as the lowpass gate; 256k outputs unchanged. LESSON: a
   "dead end" verdict is only as good as the test corpus.
3. **Attack bit banking** — repurposed into a WIN (merged as
   post-transient reservoir banking). The original goal is dead: on
   castanets the trivial inter-burst bed keeps the reservoir AT CAP
   before every attack (banking variants were bit-identical), so the
   castanets mean gap is definitively NOT a bits-at-attack problem —
   scratch lookahead extension for that purpose. But the mechanism
   (fill target 70-90% + anchor clamp ema+24 for ~0.7 s after any
   short-block frame) is a measured win wherever transients neighbor
   hard content: electronic-256 NMR −15.9→−18.0 (now clearly ahead of
   LAME's −16.1) at +1.26 dB SNR, electronic-128 +1.41 dB / NMR −0.85,
   speech-128 +0.62 dB / audible 14.1→13.3%, speech-256 +0.43 dB.
   Quartet/castanets bit-identical (state never triggers / no
   headroom). Enabled by default — JOINT MODE ONLY: the final stereo
   canonical run caught −1.0 dB SNR / −1.15 NMR in plain stereo, where
   no unshaped side channel funds the forced-saving regime (softer
   variants — fill-gated, hold-only — either kept the stereo damage or
   eroded the joint wins). Stereo keeps baseline rate control and its
   pre-banking numbers exactly.
4. **VBR psy shaping** — DONE (merged). quantize_granule_vbr now routes
   through the full quantize_granule (factor search + NMR loops + region
   polish) at the VBR gain floor, with a VBR-specific shaping budget of
   unshaped-spend × 1.25 — CBR's spend+slack/2 formula let V9 frames
   balloon 2.4× under the max-rate budget. Bytes-honest results (48k
   basis A/B): V0 speech NMR −15.34→−15.90 at equal size; V4 speech
   +4.4% bytes for NMR −10.92→−13.50 (the old ladder interpolated to
   equal bytes gives only ≈−11.7) with ODG −0.10→+0.07; V9 +6.5% bytes,
   audible 60.5→57.9%; quartet V4 +10% bytes for NMR −2.1→−14.8. The
   M/S side channel stays excluded (allow_psy threaded through).
5. **Gapless LAME tag** — DONE (merged). The finalized Xing frame now
   carries a LAME info-tag extension ("LAMEglint" version — must start
   with "LAME" for ffmpeg to honor it; delay=1104 double path / 528
   fixed, matching the measured 1633-sample total minus the decoder's
   529; end padding 0 — unknowable in a streaming encoder; info-tag
   CRC-16 over the first 190 bytes for foobar2000). ffmpeg now decodes
   VBR output with a 0-sample alignment offset (was ~2785 incl. the
   placeholder frame). CBR files carry no Xing/Info frame at all —
   adding an "Info" frame for CBR gapless is a possible follow-up.
6. **CI (GitHub Actions)** — TODO. Build matrix (double/fixed/both) +
   ctest incl. the m2 decode test (ffmpeg installs in runners). The wire
   -format history is the argument.
7. **League regression gate** — TODO. compare_encoders.py --check mode
   with recorded per-clip baselines and thresholds, wired for nightly CI.
8. **ABX script** — TODO. Small terminal ABX tool for the model-contested
   cases (speech-128, electronic-128).
9. **Fixed-point short blocks** — DONE (merged). The fixed path now has
   the one-granule lookahead (held_sub_fp), the shared window scheduler,
   and short/start/stop transforms: MDCT_FP::process_short_and_convert
   plus a transition-window branch in process_and_convert, both running
   Q24→double + double window/cos math — the quantizer downstream is
   double anyway and those granules are rare; the hot LONG path keeps
   its byte-stable integer transform. reorder/scheduler helpers moved
   out of the double-only guard; glint_flush releases the fixed held
   granule; the gapless delay is 1104 for both paths now. Metrics:
   fixed==double to ≤0.06 dB on every config INCLUDING castanets (the
   fixed path previously had no shorts at all) and m2-64k. 28/28 +
   16/16 tests, 0 backstep, full quality suite green on --fixed.
10. **Psy-loop noise caching** — DEAD END as planned (2026-07). The
    -q normal profile shows compute_band_noise at 2% — caching it is
    pointless. The psy loop's real cost is its per-iteration gain
    re-searches (select_and_count + quantize_and_count ≈ 52% of the
    tier), already LUT-optimal since perf pass 3. A warm-started binary
    search (lower bound seeded from the previous iterate's gain, -4
    margin) was metrics-identical but measured 0% faster — the probe
    count barely shrinks and probe cost concentrates near the answer.
    Reverted. The remaining normal-tier cost is inherent to shaping;
    next real lever would be evaluating candidates on the thread pool
    (the -j machinery), not caching.
11. **Mixed blocks** — DESCOPED on evidence (2026-07). The feature buys
    LF resolution during short-block frames, but glint's castanets-128k
    0-1k band-SNR is already 31.8 dB vs LAME's 24.9 — 7 dB AHEAD in
    exactly the region mixed blocks improve (subblock_gain + short
    scalefactors cover it). The remaining perceptual gap (ODG −0.63 vs
    −0.29) is not an LF problem, so the largest wire-format surface in
    the format (per-subband window chains, dual scalefactor layout,
    switch_point regions) would target a non-problem. Revisit only if a
    clip ever shows an LF-during-transients deficit.
12. **Intensity stereo / ABR** — not attempted (the queue's measured
    priorities ran out before them; intensity stereo only matters below
    the rates glint targets, ABR is a rate-control mode without a user).

# AAC-LC track (started 2026-07-06)

One repo, two codecs: the AAC encoder lives in `src/aac_*.{hpp,cpp}` with
its own C API (`glint_aac_*`), sharing the repo's build, CLI and test
harness. The MP3 lessons carry over wholesale: decode-based gates from
day one (the m2 lesson), tables cross-checked from independent sources,
exact bit accounting, quality changes gated on metrics.

## A0. Phase 1 — wire-correct long-block CBR encoder — DONE (merged)

- **Tables** (`src/aac_tables.hpp`, generated by `tools/gen_aac_tables.py`):
  normative ISO 13818-7/14496-3 data — 11 spectral Huffman books, the
  scalefactor book, sfb offsets for all 12 rates (long + short) — extracted
  from BOTH vo-aacenc (Apache-2.0) and ffmpeg aactab.c, cross-checked
  bit-for-bit, each book verified prefix-free (Kraft), before emission.
- **MDCT** (`aac_mdct.cpp`): 2048-point per the ISO formula
  (X[k] = 2·Σ z[n]cos(2π/N(n+n0)(k+½))), computed as ±fold →
  DCT-IV via 512-pt FFT with e^{-iπ(n+1/8)/M} pre/post twiddles.
  Unit-tested against the direct O(N²) formula (<1e-10 rel). The
  fold/twiddle convention was locked with a numpy prototype first.
- **Coder** (`aac_coder.cpp`): pow-3/4 quantizer with the 0.4054 magic
  offset, flat scalefactors (= global_gain), OPTIMAL sectioning via DP
  over (band, book) with 9-bit section-header transitions, exact bit
  count by running the emitter in count-only mode (section-length
  escapes included). Book-11 escapes, sign bits, 4/2-tuple indexing all
  wire-validated. Binary-search gain fit like MP3's, with last-accepted
  state reuse.
- **Framing** (`aac_encoder.cpp`): ADTS (MPEG-4 ID, LC profile),
  SCE/CPE + common_window ics_info, bit-debt CBR controller (target
  minus spent, clamped to the 6144-bit/ch decoder buffer) — AAC frames
  are variable-length, so there is NO MP3-style backstep hazard; the
  average converges exactly (128.0/256.1/64.0/48.0 kbps measured).
  Encoder delay: exactly 1024 samples (verified by the decode test's
  cross-correlation alignment); `glint_aac_flush` emits the tail frame.
- **Validation**: ffmpeg AND CoreAudio (afconvert) decode all configs
  with zero errors and METRICALLY IDENTICAL output (25.83/28.00/10.16
  both decoders on speech-128) — the wire format is unambiguous.
  ctest gate `aac_decode_quality` (tests/test_aac.py): 5 configs
  (44.1/48/22.05 kHz, mono/stereo, 48-256 kbps), zero-decoder-error +
  SNR floors ~10 dB below introduction-day measurements (40/48/41/37/40 dB).
- **Speech-clip numbers** (1-min ref, flat quantization, NO psy model):
  128k stereo SNR 25.8 / segSNR 28.0 / NMR +0.5 (26% audible) — psy
  headroom is the whole phase-2 story; 256k stereo SNR 37.2 / segSNR
  38.7 / NMR −10.4 / 1.0% audible — ALREADY ahead of glint-MP3-256k
  SNR (36.1–36.7) on pure coding efficiency. ~120× realtime unoptimized.

## A1. Phase 2 — quality (roadmap)

Ordered by expected payoff, all gated on the standard clip set + league
table (add AAC rows to tests/compare_encoders.py; fdk-aac and Apple
afconvert are the yardsticks, vo-aacenc is the Shine-role baseline):
1. **Psy model + NMR outer loop — DONE (2026-07-06).** Per-band
   scalefactors (bidirectional offsets ±30 around the gain anchor, dpcm
   chain emission), metric-aligned Schroeder-Bark masks (aac_psy.cpp),
   outer loop at -q normal/best. Speech 128k stereo NMR +0.46 → −0.58
   (audible 26.1→23.6%, SNR 25.8→24.2), speech 256k −10.35 → −11.54
   (p95 −3.8→−5.7); electronic 128k 2.30→1.93; quartet 128k −0.55→−1.31,
   quartet 256k −9.52→−12.10 with audible 2.0→0.0%. ~30× realtime.
   **AAC-specific learnings (differ from the MP3 loop!):**
   - MP3's 1.25× total-noise guard FREEZES the AAC loop at 128k: AAC CBR
     has no reservoir slack, and one gain-anchor step is already ×1.41
     total noise. The working structure: bidirectional offsets (coarsen
     over-coded bands ≥6 dB below target to donate bits — MP3 could only
     amplify), a per-band ceiling at the MASK (r ≤ max(1, r0)·1.05 — a
     ceiling at the target instead froze everything), two amplification
     tiers (r>1 audible bands every round + near-worst masked bands),
     de-amp hysteresis (never coarsen a rescued band), and a loose 2.5×
     total-noise backstop chosen by measurement (8.0 buys +0.2 NMR for
     −0.8 SNR — not taken).
   - ATH floor must be calibrated against a RUNNING max band energy, not
     per-frame (per-frame gives quiet frames absurd floors; the loop
     burns budget on phantom violations — the metric calibrates
     file-globally for the same reason).
   - normal and best currently converge to the same fixed point (caps 16
     vs 40 iterations are both past convergence); differentiate later
     via tonality masks / finer steps, not more iterations.
   - Comparison points measured on the speech clip at 128k stereo:
     glint −0.58 / ffmpeg-native AAC −0.68 / LAME MP3 −2.65 / **Apple
     AAC −6.52, audible 5%** — the remaining gap to Apple is mostly
     M/S + TNS + short blocks, not loop tuning.
2. **Per-band M/S** (ms_mask_present=1) — finer than MP3's all-or-nothing
   joint stereo; mind the MP3 lesson: never shape the side channel.
3. **Window switching + short blocks** (8×128 MDCT, start/stop windows,
   scale_factor_grouping) — scheduler + lookahead ports from MP3.
4. **TNS** — biggest LC feature vo-aacenc barely uses; speech/transients.
5. **Bandwidth heuristic → psy-driven max_sfb** (current cutoffs are a
   placeholder table roughly tracking fdk defaults).

## A2. Phase 3 — fixed-point path + RAM diet (roadmap)

Q31 signal path, GLINT_SMALL_BUFFERS treatment, target: beat vo-aacenc's
"several hundred KB" footprint the way MP3-glint beat Shine's 96 KB.

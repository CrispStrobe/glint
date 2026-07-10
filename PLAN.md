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

## 13. MP3 no-FPU integer -q speed path — DONE (2026-07-06), GLINT_MP3_INT

Honest finding that triggered this: the MP3 "fixed" path was Q31 only
through the SUBBAND analysis — the 36-pt MDCT converts Q24->double and
the whole rate loop is double, so the old "no FPU" README claim really
meant "runs via soft-float". GLINT_MODE=fixed now also defines
GLINT_MP3_INT, making the -q speed CBR chain integer per-coefficient
end-to-end (subband Q31 -> int freq-inversion -> NEW integer 36-pt MDCT
(Q31 fused win*cos/288 table, int64 accumulation) -> existing integer
alias reduction -> int sfb21 lowpass (same adaptive content-keep rule,
int64 energies) -> log-domain integer quantizer (shared intmath.hpp Q16
log2/exp2 LUTs; MP3's gain step is exactly 12288 in Q16, the same
constant as AAC's sf step) -> the already-integer Huffman/bitstream).
Per-frame scalars (rate control, gain bounds) remain double = fine on
soft-float. The transient scheduler is quality>=1 only, so no
per-sample double sneaks in at speed.

Gates: -q speed metrics IDENTICAL to the old fixed build within
0.01-0.02 dB on every measure (128k/256k MPEG-1 and 64k MPEG-2), 0
backstep; normal/best/VBR and the double path BYTE-IDENTICAL
(untouched); all ctest suites green. M1 wall time unchanged (its FPU
made doubles free — the win is RP2040-class soft-float targets).
Scope note: -q speed + CBR only; normal/best (psy loops) and VBR keep
double math by design. intmath.hpp refactor left AAC INT output
byte-identical.

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
2. **Per-band M/S — DONE (2026-07-06).** ms_mask_present per-sfb flags
   (0/1/2 encoded), decision per band by the mask-relative product rule
   (t+eM)(t+eS) < (t+eL)(t+eR) with t = min(maskL, maskR); chosen bands
   transformed in place before fit/shaping, and BOTH M and S shape
   against t (the AAC form of "never shape the side channel unmasked" —
   noise in either lands in decoded L and R). Active at all qualities.
   The single biggest quality step so far — every metric improved on
   every clip at every rate: speech 128k best NMR −0.58→−3.05 (audible
   12.4%) with SNR UP 24.2→26.1; speech 256k −11.5→−16.0, SNR
   35.2→41.9; electronic 128k 1.93→−0.95; quartet 128k −1.31→−5.52,
   256k −15.0 / 0.0% audible, SNR 50.6. The AAC path now beats glint's
   MP3 path outright (speech 256k: 41.9/−16.0 vs MP3 joint 38.0/−13.8).
   Remaining Apple gap at 128k: −3.05 vs −6.52.
3. **Window switching + short blocks — DONE (2026-07-06).** All four
   window sequences; 8×256 MDCTs over the centre region [448,1600);
   attack-split grouping (≤3 groups: [0,s) [s,s+2) [s+2,8), s from the
   attack sub-block ±4 window offset); band-layout abstraction
   (AacBandLayout) makes short frames look like long ones to the
   quantizer/sectioner (coded-order interleave group→sfb→window, the
   MP3 reorder trick; sections never cross groups; sect_len esc 7/3-bit
   vs 31/5-bit). Transient detector: 8 sub-block first-difference
   energies vs rolling baseline (8×, fast-rise slow-fall), one-block
   lookahead hold → encoder delay 2048; flush emits two tail frames.
   Short frames are NOT psy-shaped and use the energy-only M/S rule
   (t=0 in the product rule); START/STOP not shaped (MP3 lesson).
   GLINT_AAC_FORCE_SHORT=1 forces all-short for diagnostics.
   **Castanets 128k best: NMR +17.7 → −3.46, audible 13.4→3.1%;
   256k +9.2 → −9.77.** Contenders on the same clip at 128k: LAME
   +1.15, ffmpeg-native AAC +7.14, Apple −6.70/1.9%. Steady clips
   improve-or-hold exactly (quartet byte-similar — scheduler never
   fires; electronic gains 2.7 dB NMR from real transients; speech
   +0.5 dB NMR, +0.4 SNR). Wire-validated: ffmpeg + CoreAudio decode
   all transition sequences with zero errors; burst-train config added
   to tests/test_aac.py.
4. **TNS — DONE (2026-07-06), selective activation.** Long-family
   windows, one filter over [~2.5 kHz band, min(max_sfb,
   tns_max_bands)), order ≤ 8, 4-bit arcsin-quantized reflection
   coefficients, forward direction, active only when LPC prediction
   gain ≥ 4.0. Measured (best): castanets 128k NMR −3.46 → **−8.38**
   (audible 3.1→1.0% — now ahead of Apple's −7.41, near FDK's −9.02);
   electronic 128k −3.64 → −4.30, 256k −14.61 → −15.39; speech and
   quartet hold (quartet: filter never fires on strings — the
   selectivity working as intended). GLINT_AAC_NO_TNS=1 disables
   (diagnostic).
   **Hard-won wire/DSP invariants (a full debugging afternoon):**
   - The transmitted values are REFLECTION coefficients; the ISO
     dequant map is +sin(idx/iqfac) (== vo-aacenc's tnsCoeff4, which
     the table generator cross-checks). ffmpeg stores its
     tns_tmp2_map PRE-NEGATED and negates again inside
     compute_lpc_coefs — the double negation cancels. Transmit k
     as-is; do NOT negate to "match" ffmpeg's r = -coef line.
   - tns length counts down from num_swb (the samplerate's FULL sfb
     count), NOT from max_sfb: length = num_swb - start_band. The
     decoder clips the line range to min(max_sfb, tns_max_bands).
   - Decoder tool order is M/S recombine THEN per-channel TNS
     synthesis → encoder must TNS-filter L/R BEFORE the M/S
     transform. Stereo speech cannot catch this bug (L≈R makes the
     filters identical, and identical filters commute with M/S) —
     MONO is the oracle for TNS correctness.
   - Naive activation (gain ≥ 1.4, order 12, from 1 kHz) REGRESSED
     every metric (ODG −0.87 → −1.09): with per-band scalefactor
     shaping already active, TNS's spectral whitening fights the
     allocator and only strongly-predictable frames profit. Judge
     TNS on ODG/PESQ + castanets NMR, not speech Bark-NMR.
   - Wire-correctness probe that settles "is the filter inverted
     right": encode at 640k with/without TNS — a real mismatch
     collapses SNR to ~15 dB; correct TNS costs ~1.5 dB at the 65 dB
     level. In-process round-trip vs the decoder recursion
     (scratch test) proved the math exact before the wire hunt.
5. **Tonality-aware masks — DONE (2026-07-06).** Per-masker offsets
   −(6 + 12·α) dB from band spectral flatness (α∈[0,1]) instead of the
   metric's flat −14 dB, ported from the MP3 nmr_outer_loop tonal
   path, gated at ≤96 kbps/ch like MP3. Judged on ODG/PESQ (the
   in-house NMR is expected to diverge slightly since the offsets no
   longer match the metric's): speech 128k ODG −0.87→−0.85 / PESQ
   4.51→4.54; 64k mono ODG −2.34→−2.23; **electronic 128k ODG
   −0.78→−0.49**; quartet −0.35→−0.32; castanets ≈0.01 (transparent
   boundary). NMR holds within 0.15 everywhere. 256k paths unchanged
   (gate off above 96 kbps/ch).
6. **Bandwidth heuristic → psy-driven max_sfb** (current cutoffs are a
   placeholder table roughly tracking fdk defaults).

## A1b. AAC league table — measured 2026-07-06 (post short blocks)

`python tests/compare_encoders.py --codec aac` (new `--codec` flag; ADTS
everywhere; lame-q2 rides along as MP3 anchor). Contenders: Apple
afconvert `-f adts -d aac -s 0` (CBR), fdkaac (`brew install
fdk-aac-encoder`, `-f 2` for ADTS), ffmpeg native aac, vo-aacenc
(`~/code/glint-tools/vo-aacenc/aac-enc`, built from mstorsjo/vo-aacenc:
autoreconf+configure for the lib, then
`cc -O2 -I. -Icommon/include -o aac-enc aac-enc.c wavreader.c
common/cmnMemory.c .libs/libvo-aacenc.a`; NB `-r` is BITRATE in bps).
Full table in the 2026-07-06 run log; NMR summary at 128k:

- glint-normal places 1st on quartet (−5.5 vs Apple −2.9 / fdk −2.6)
  and industrial (−1.6 vs Apple −1.1 / fdk −0.8); 2nd on piano (−8.7,
  between Apple −9.4 and fdk −8.5); 3rd on speech (−3.6 vs −6.9/−5.4),
  electronic (−3.6 vs −9.9/−11.4) and castanets (−3.5 vs −7.4/−9.0,
  but ODG −0.04 ≈ Apple/fdk's −0.08 — PEAQ says the transients are
  fine; the NMR gap is steady-state). ALWAYS ahead of ffmpeg-native,
  LAME-MP3 and vo-aacenc (vo: castanets NMR +18.6, ODG −3.x on tonal
  clips — the quality floor of the league).
- 256k: ODG ≈ 0 for Apple/fdk/glint/LAME on every clip. glint SNR
  highest of the AAC field on 4/6 clips; quartet NMR −15.0 = 1st.
  Remaining NMR gap to Apple/fdk at 128k on speech/electronic =
  the TNS + better-psy (tonality) work, not rate control.
- glint-speed sometimes beats normal on castanets NMR (−5.1 vs −3.5):
  shaping the long frames around transitions costs a little there —
  revisit when shorts get shaped (A1.4).
- Speed (this run, moderate load): glint 39-86x depending on tier/clip;
  Apple ~100x, fdk 53-110x, vo ~94-168x, ffmpeg-native 6-56x. Idle
  spot-check: glint-aac speed/normal/best = 85/41/37x. No AAC perf
  pass yet.
- RAM (method: sizeof(glint_aac_context) via a measurement TU that
  #includes aac_encoder.cpp; statics via `size -m` per object with
  -fno-lto): glint-aac double = 61.4 KB context + 55.5 KB BSS
  (33.4 mdct tables + 22.1 psy model) ≈ 117 KB. vo-aacenc = 48 KB
  heap total (measured by logging its cmnMemAlloc; zero BSS) — its
  "several hundred KB" reputation is Arduino-wrapper overhead, so the
  phase-3 target is UNDER 48 KB, which is genuinely tight (the 2x1024
  double MDCT lookback alone is 16 KB → int16/int32 input buffers and
  a Q31 path are mandatory, plus single-slot psy/mdct models).

## A2. Phase 3 — RAM diet — DONE (2026-07-06); Q31 arithmetic still open

Under GLINT_SMALL_BUFFERS (defined by GLINT_MODE=fixed builds):
**47.6 KB measured** (25.0 KB context + 22.6 KB BSS) vs vo-aacenc's
48.0 KB — and quality is METRICS-IDENTICAL to the desktop double build
(every SNR/NMR/aud figure matches to the printed precision on speech
128k/256k; full unit + decode suites green). Desktop double also
dropped 117 → 106.5 KB from the shared half-window tables.

What the diet is: STORAGE-only type changes — arithmetic stays double
throughout (SpecT/PcmT in aac_coder_types_fwd.hpp):
- spectra + masks stored as float, PCM lookback blocks as int16 (the
  float encode path clamps to int16 under the flag),
- MDCT twiddles/windows + psy spread/ath tables as float,
- sine windows stored half-length (symmetric, mirrored accessor),
- FFT bit-reversal computed inline (no rev table),
- output buffer 4 KB (two tail frames fit).

## A5. Distortion-controlled allocator — DONE (2026-07-06); PE rate control REJECTED

The 128k push. Per-Bark diagnosis vs Apple showed we were ~5-7 dB worse
ACROSS the whole spectrum at equal bits — the iterative amplify/walk
never reaches a noise~mask allocation (it stalls near its own target
from a flat start). Two experiments:
- **PE bit-demand rate control: REJECTED.** Scaling per-frame budgets
  by a perceptual-entropy proxy (EMA-normalized, [0.75,1.5]x) measured
  WORSE at strong settings and a no-op at gentle ones — bit quantity
  was not the binding constraint. Reverted; don't retry without a
  different difficulty signal.
- **aac_fit_channel_masked: SHIPPED** (normal/best CBR long frames).
  Per-band scalefactors in closed form from noise targets
  mask^alpha * k (uniform-noise model: sf = 100 + 2*alpha*log2(12m/w)),
  the loudness knob k bisected as an INTEGER sf offset (2 sf steps per
  factor 2 — deterministic), a min-SNR floor on CODED bands only
  (kSnrMinDb 10; applying it to masked bands force-coded inaudible
  junk and collapsed ODG to -3.3 — the sf_zero guard is essential),
  and one measure-and-correct pass. alpha swept by ODG/PESQ: 1.0
  (pure mask-following) LOSES on speech (PESQ 4.54->4.24 — the model
  trusts our crude masks too much); **alpha = 0.6 shipped**.
  Gating that made it safe: allocator only when frames_since_short >=
  26 (direct allocation on post/pre-transient long frames cost
  castanets ODG -0.01 -> -0.23; with the gate castanets is EXACTLY
  baseline) and >= 56 kbps/ch (the walk measured better at 64k mono).
  Everything else keeps the A1.1 walk.
  Results (best, vs the walk): speech 128k ODG -0.85 -> -0.82, p95
  2.42 -> 2.27, audible 12.6 -> 11.9%; electronic 128k ODG -0.49 ->
  -0.37, audible 9.2 -> 8.7; quartet audible 8.5 -> 6.9 (ODG -0.35 ->
  -0.39, within PEAQ noise); speech 256k NMR -16.62 -> -17.16, p95
  -10.33 -> -11.05, 0.0% audible; castanets/64k hold. Decode-gate SNR
  floors re-based (the allocator legitimately trades raw SNR on tonal
  synthetics for masked placement — floors are decode sanity, not
  quality gates).

## A6. Shaped short frames — DONE (2026-07-06)

Short frames were the last flat-quantized path. Added
aac_compute_masks_short (per-group Schroeder spreading over the short
sfbs, flat −14 dB offset — no tonality at 128-line resolution; energies
normalized per window inside the model, returned in the band domain;
SEPARATE emax_run_short domain) and ran the direct allocator on short
frames with its own tilt kAlphaS.
Measured decisions:
- kAlphaS = 0.2 (kAlphaL stays 0.6): 0.35+ dumped noise onto the tonal
  background INSIDE short frames — castanets ODG fine but the synthetic
  burst-train SNR collapsed 26→15 dB and electronic ODG dipped; 0.2
  keeps nearly all the NMR win with burst SNR 23.6.
- The masked M/S rule on shorts was suspected for an electronic ODG dip
  and DISENTANGLED: it was innocent (energy rule kept anyway — masks
  still flow to the allocator). The dip traced to the long-frame
  transient gate, and a graduated alpha derate (scale by fss/26) LOST
  everywhere — a derated allocator does nothing while the walk works,
  so the hard >= 26-frame gate stays with the walk as fallback.
Final (128k best vs pre-shorts): castanets NMR −8.40→−8.45, audible
0.9→0.8, ODG 0.00 (transparent); electronic −4.28→−4.34, aud 8.7→8.6;
speech −3.55→−3.64, p95 2.42→2.11, aud 12.6→11.8; quartet −5.84,
aud 6.9, ODG −0.38; speech-256 −17.03. All suites green.

## A2c. Embedded validation harness — DONE (2026-07-06)

embedded/: shared FPU-free bench core; QEMU mps2-an385 semihosted run
(both codecs encode under soft-float Cortex-M3, streams written back to
the host via semihosting, decode CLEAN in ffmpeg+CoreAudio, MP3 stream
BIT-EXACT vs a host run — integer determinism across architectures);
pico-sdk and ESP-IDF benchmark projects for real-silicon throughput.
tools/check_nofpu.sh = static proof: M0+ disassembly of every
per-coefficient function contains zero __aeabi_* soft-float calls
(lazy table builders moved to startup global ctors to make this
provable; byte-stable). QEMU is NOT cycle-accurate — throughput still
needs hardware. Toolchain: xPack arm-none-eabi (Homebrew's formula has
no libc); GLINT_NO_THREADS skips the std::thread pool on bare metal.

## A3. Perf pass 1 — DONE (2026-07-06): −52..54% encode time, byte-identical

Profile-driven (macOS `sample`, -q best): eval_gain was 66% of runtime
(quantize + per-band cost for up to 11 books via separate code_band
walks + DP) and the count-only aac_write_ics_body re-walk another 13%.
Three changes, all verified BYTE-IDENTICAL across
{speed,normal,best} x {128,256,64-mono} in BOTH build modes (double and
fixed/INT), plus castanets-best and 22.05k:
- band_costs(): all valid books' spectral bits in TWO passes over the
  band (one 4-tuple pass for books 1-4, one pair pass for 5-11 incl.
  book-11 escapes) instead of up to eleven code_band walks. Identical
  bit arithmetic; code_band stays for emission.
- Zero bands cost O(1): ntuples * zero-tuple bits (lazy table from
  kSpecBits — the old code walked the whole band for every book).
- Exact ICS count is now O(num_bands) ARITHMETIC: spectral bits summed
  from the cost table, sections/scf-dpcm/tns counted in closed form —
  no more count-only emission over the spectrum. A GLINT_AAC_COUNT_CHECK
  define cross-checks it against the emitter (used to validate; the
  count==emission unit test also gates it).
- getenv() calls cached out of per-frame/per-iteration paths.
Measured (300 s speech, interleaved min-of-5): speed −54%, normal −52%,
best −52%. Absolute: double 272x/168x realtime (speed/best), fixed/INT
241x/140x — glint-aac -q speed is now the fastest encoder in the league
(Apple ~104x). Byte-identity gate caveat for the future: zsh does NOT
word-split unquoted $args — a broken gate loop compared stale files and
reported false DIFFERs; use eval or arrays.

## A4. AAC VBR — DONE (2026-07-06)

Constant-quality VBR the way ADTS wants it: no debt controller, every
frame may spend to the 6144-bit/ch cap, and a per-quality ANCHOR-GAIN
FLOOR (aac_fit_channel gain_floor param) is what limits spend — the
direct analog of MP3's vbr_target_gain, minus all the Xing/placeholder
machinery (ADTS frames are self-describing; nothing to rewrite).
Config: glint_aac_config.vbr / vbr_quality (consumed two reserved
slots — same struct size, zero = CBR; the smoke test promptly proved
WHY the zero-init contract exists: a stack-constructed config had
garbage in .vbr). Shaping under VBR runs at spent+25% like MP3's VBR
shaping. Ladder kVbrGain[10] = 133..162 (~1.5 dB noise/step),
calibrated on the speech clip: V0 311 kbps / NMR −16.8 / 0.0% audible
(beats CBR-256's −16.6), V2 220k, V4 153k / −4.1, V6 100k, V9 42k;
electronic V4 lands at 134k (content-adaptive, as VBR should).
GLINT_AAC_VBR_GAIN overrides the floor for experiments. CLI -V works
for .aac outputs now; python binding takes vbr_quality=, rust/dart
struct mirrors updated. CBR paths byte-identical; all decode gates
green (a V4 config joined tests/test_aac.py).

## A2b. No-FPU integer hot path — DONE (2026-07-06), GLINT_AAC_INT

GLINT_MODE=fixed now also defines GLINT_AAC_INT: spectra are int32 (Q3,
spec_int = spec_true*8) and ALL per-coefficient work is integer —
targets RP2040/Cortex-M0+ class parts where soft-float at
per-coefficient rates is fatal:
- Integer MDCT: Q30 twiddles/windows, int64 butterflies with a rounded
  >>1 per FFT stage; measured transform SNR 131 dB vs the direct ISO
  formula. Scale note: natural output is Q(qin - log2 H) — the ISO 2x
  is part of the fold/DCT-IV algebra, NOT an extra bit (a wrong +1
  here read as a clean 6 dB "SNR" — scale bug, not noise).
- Integer quantizer: p34 cache holds log2(|x|^0.75) in Q16; the sf
  step is EXACT in Q16 (0.1875*65536 = 12288); ix = one add + one
  128-segment exp2 LUT. Parity vs the double formula: 0.04
  mismatched coefficients per frame.
- Integer band energies everywhere (psy accumulation, M/S decision,
  TNS autocorr, TNS mask compensation) — NO pre-shifts needed:
  Parseval bounds the whole frame's Q3^2 energy at ~2^59 (early >>8
  "safety" shifts zeroed quiet coefficients for nothing). Per-BAND
  math (spreading matmul, Levinson, rate control) stays double: a few
  k soft-float ops per frame is fine on M0+.
- Masks are float storage ALWAYS (SpecT-typed mask arrays truncated
  Q3^2 energies to int32 garbage and froze the shaping loop — subtle,
  found via GLINT_AAC_DEBUG traces).
- Measured (INT vs float small-buffers): speech 128k speed -1.67 vs
  -2.54, best -2.85 vs -3.52; 256k -14.57/-15.68 vs -16.05/-16.62;
  castanets 128k best -8.56 (BETTER than float's -8.38). Root cause
  of the stereo tax is PROVEN by the L==R experiment (identical
  channels -> INT == float exactly): integer M/S halving loses half
  an LSB on odd sums and the decoder's l=m+s cannot recover it.
  Counter-intuitively Q4 spec scale measured WORSE than Q3 overall
  (unresolved); the real fix if ever needed is a per-frame block
  exponent. Mono is bit-comparable to float.
- All suites green (unit incl. MDCT-vs-ISO at INT tolerances, decode
  gates, ffmpeg+CoreAudio). 112x realtime on M1 at -q speed. RAM
  unchanged at 47.6 KB (int32 == float width).
- Remaining honest gap to "runs realtime on RP2040": needs an actual
  cross-compile + on-target measurement; per-coefficient float is
  gone, per-frame float remains (spreading, Levinson, rate control).

Stack use (~30 KB transient in MDCT/fit) is not in the 47.6 KB figure;
neither is vo-aacenc's stack in its 48 KB (heap-only, same basis).

# Opus track (started 2026-07-10) — branch `feature/opus` ONLY

**Git workflow override for this track: commit to `feature/opus`, do NOT
merge to main** until explicitly cleared. (MP3/AAC work keeps the usual
merge-to-main rule.)

Scope assessment (2026-07-10): Opus (RFC 6716) = SILK (LPC speech coder,
8/12/16k internal) + CELT (MDCT, 2.5–20 ms at 48k) + hybrid mode, glued by
a shared range coder. Unlike MP3/AAC there are no scalefactors-as-side-info:
CELT's bit allocation is IMPLICIT — decoder re-derives it from the same
computation the encoder runs, keyed on tell_frac() at 1/8-bit precision, so
encoder/decoder must agree bit-for-bit on entropy-coder state. The normative
spec is the reference code (prose loses ties); decoder conformance = official
test vectors + opus_compare quality threshold, not bit-exactness. No
legal/patent reason for clean-room (BSD reference, royalty-free) — it's the
project ethos only, and reference implementations remain fair game as test
ORACLES (same rule as gen_aac_tables.py). Calibration: ffmpeg's independent
native Opus encoder is CELT-only and still clearly behind libopus after
years — "correct" is achievable, "league-competitive" is a long campaign.

Phases:
- **O0 — range coder — DONE (see below).**
- **O1 — CELT decoder**: mixed-radix FFT (MDCT sizes 120/240/480/960 =
  2^a·3·5, NOT power-of-two — the MP3/AAC FFTs don't cover this), coarse
  energy (2-D prediction + Laplace), fine energy, the implicit allocator,
  PVQ decode + spreading/folding + anti-collapse, TF resolution, intensity +
  mid/side, postfilter. Interim oracle before full conformance: CELT-only
  streams from `opus_demo` (restricted-lowdelay application forces CELT).
- **O2 — SILK decoder + hybrid**: LPC synthesis, LTP, stereo
  prediction/unmixing, resamplers. Gate: official test vectors 01–12 pass
  the opus_compare threshold (both rates of each vector).
- **O3 — Ogg Opus container** (RFC 7845): mux/demux, pre-skip, granule
  accounting; Opus packets are not self-framing (no ADTS analog).
- **O4 — CELT-only encoder**: reuses the decoder's allocator verbatim;
  PVQ search, coarse/fine energy quant, TF/transient decisions, VBR.
  Needs ec_enc_shrink + ec_enc_patch_initial_bits (not yet implemented in
  opus_ec). Validate: decode with libopus/ffmpeg, league via
  compare_encoders vs libopus + ffmpeg-native at matched rates.
- **O5 (optional) — SILK/hybrid encoder** for low-rate speech.

## O0. Range coder — DONE (2026-07-10), byte-identical to libopus

`src/opus_ec.{hpp,cpp}`: RangeEncoder/RangeDecoder per RFC 6716 §4.1 —
encode/encode_bin/bit_logp/icdf/uint/raw bits + tell/tell_frac, all-integer
(safe for GLINT_MODE=fixed), in the glint library build.

Verification, two layers:
- Unit tests (test_unit.cpp): randomized op-script round-trips (5 seeds ×
  2000 ops), per-op encoder-vs-decoder tell() parity, exact-size
  ceil(tell/8) buffer round-trip, carry stress (max-symbol runs), enc_uint
  edges around the 2^8 range/raw split. 260/260 pass.
- **Wire-compat gate: `tools/crosscheck_opus_ec.py`** builds libopus 1.5.2
  as an oracle in ~/code/glint-tools/opus-1.5.2 (static lib; ec_* symbols
  are linkable even though not public API), compiles
  `tools/opus_ec_crosscheck.cpp` twice (reference adapter vs glint), and
  requires byte-identical stdout: encoded buffers (full + exact-size),
  tell/tell_frac traces, decoded values — 8 seeds × 2000 ops × 6 op kinds.
  **PASS on first run.** Re-run after any opus_ec change.

Hard-won invariants:
- Decoder `val` is COMPLEMENTED (distance from the top of the range):
  init reads 127 − (b0>>1), renormalization inserts ~sym. Both sides
  report tell()==1 immediately after init.
- Range bits fill the buffer from the FRONT, raw bits (enc_bits) from the
  BACK; the two streams may legally share the final byte — done() ORs the
  raw-bit remainder into it. Total stream always fits ceil(tell/8) bytes.
- 0xFF chunks cannot be emitted eagerly (a later carry could ripple
  through); carry_out() buffers a run count (ext_) + one pending byte
  (rem_) and materializes on the next non-0xFF chunk.
- tell_frac refines ilog(rng) by 3 bits via repeated Q15 squaring of the
  top 16 bits of rng. CELT's allocator consumes this — it must match the
  reference EXACTLY (it does; traced per op in the cross-check).
- Still missing from opus_ec (needed by O4, trivial adds): ec_enc_shrink,
  ec_enc_patch_initial_bits.

## O1 progress (2026-07-10): primitives + energy + allocator + IMDCT DONE

All verified against libopus 1.5.2 oracles (custom-modes static build in
~/code/glint-tools/opus-1.5.2-custom — internal symbols are linkable, and
opus_custom_mode_create exposes the mode struct):

- **CELT mode tables**: src/opus_celt_tables.hpp GENERATED by
  tools/gen_opus_celt_tables.py (window matches the analytic formula at
  5.2e-9; note eband5ms/band_allocation live in celt/modes.c, not
  static_modes_float.h; eMeans is 25 entries, last 4 padding).
- **Laplace coder** (opus_laplace): byte-identical, incl. flat-tail clamp.
- **CWRS/PVQ enumeration** (opus_cwrs): table-free O(k) row recurrences
  (RAM-diet friendly vs libopus's 5 KB static table); index-for-index
  identical, cross-checked interleaved with laplace in one ec stream
  (tools/crosscheck_opus_celt_prims.py).
- **Energy envelope decode** (opus_celt_energy): coarse (2-D prediction +
  budget fallbacks <15/<2/<1 bits) / fine / finalise.
  tools/crosscheck_opus_energy.py — tells exact, energies within
  float-vs-double tolerance.
- **Bit allocator** (opus_celt_rate: compute_allocation_dec, bits2pulses,
  pulses2bits, init_caps): byte-identical over 200 fuzzed scenarios
  (tools/crosscheck_opus_alloc.py) incl. hybrid start=17, both channel
  counts, all LM, dynalloc boosts, negative budgets.
- **Inverse MDCT** (opus_mdct: CeltImdct + mixed-radix 2/3/4/5 FFT, all
  four shifts 960/480/240/120): matches clt_mdct_backward to <=1.3e-5
  (float oracle) incl. the interleaved-transient stride layout; O(S^2)
  direct-formula self-check at 1e-11. Contract documented in the header —
  NOTE: output is NOT plain overlap-add; out[0..ov/2) is a windowed TDAC
  rotation (read-modify-write), rest overwritten; in-place safe.

**KEY TESTING INSIGHT — fuzz oracles for decode-only components**: the
range decoder accepts ANY byte stream, so feeding both decoders identical
random buffers and comparing symbol/tell traces is a complete equivalence
gate without needing an encoder. All decode-side cross-checks use this.

GOTCHAS learned:
- Reference drivers need -DUSE_ALLOCA (stack_alloc.h errors otherwise) on
  top of -DOPUS_BUILD -DCUSTOM_MODES.
- pulses2bits/bits2pulses are only defined for pseudo-pulse indices
  <= cache.bits[index] (the row header); beyond that even libopus reads
  adjacent-row garbage. The allocator guarantees in-domain values.
- The pulse-cache bit costs saturate at 255 (uint8) on wide bands — cost
  curves plateau, so bits->pulses->bits round-trips only hold on the
  strictly monotonic part of the curve.
- ec_laplace_encode may CLAMP the value it codes (flat-tail overflow);
  any encoder must use the returned value, not the input.

NEXT (O1 remainder, in order):
1. bands.c decode side: quant_all_bands / quant_band recursion (theta
   splitting, lowband folding, spreading rotation, anti-collapse), TF
   decode, intensity/dual-stereo application. Fuzz-oracle against
   quant_all_bands with random streams; alg_unquant/exp_rotation are
   linkable for finer-grained checks.
2. celt_decoder.c top level: flags (silence/postfilter/transient/intra),
   dynalloc decode, denormalise, IMDCT+deemphasis chain, postfilter,
   anti-collapse hookup, PLC stub. Oracle: opus_custom_decoder or
   celt_decode_with_ec on CELT-only packets.
3. TOC/packet parse (code 0-3 framing) + opus_decode top level for
   CELT-only streams; oracle: opus_demo with restricted-lowdelay.
4. Then SILK (O2).

## O1 progress 2 (2026-07-10): band decoding DONE — the largest CELT piece

**opus_celt_bands.{hpp,cpp}** — decode side of bands.c/vq.c: quant_all_bands
(folding-source norm buffers, lowband_offset/update tracking, hybrid special
folding at start+1, dual-stereo->intensity switch), quant_band_stereo (theta
join, N==2 orthogonal-side sign trick), quant_band (tf recombine/subdivide
Haar chains, Hadamard reorder for transients, resynth un-do, sqrt(N) folding
scale), quant_partition (recursive splits, delta bit-split from bit-exact
integer log2tan, rebalance, PVQ leaf with budget walk-down, LCG noise/fold
fills), compute_theta (all three PDFs: step/uniform/triangular + isqrt32),
compute_qn, exp_rotation spreading, collapse masks, stereo_merge,
anti_collapse. Encode branches and theta-RDO dropped (decoder-only).

Gate: tools/crosscheck_opus_bands.py — fuzz oracle wired exactly like
celt_decode_with_ec (allocator -> quant_all_bands -> anti-collapse bit ->
anti_collapse) over 150 random frames covering all LM, mono/stereo, hybrid
start=17, transient+tf combinations, all spread modes, dual stereo,
disable_inv. Collapse masks / LCG seeds / tells byte-identical; spectra
match to 4e-6 (float-vs-double). PASS.

Learnings:
- The decode path's ec reads are 100% integer-driven (qn/theta PDFs, delta
  via bitexact_cos/log2tan, cache walk-downs) — signal values never touch
  the wire, so the fuzz oracle is complete for this layer too.
- tf_res values must stay in tf_select_table's domain per (LM, transient)
  when fuzzing — arbitrary values hit UB in both implementations.
- The reference decoder aliases lowband_scratch onto X_'s last-band region;
  a separate scratch buffer is output-identical (scratch is written before
  read within each quant_band call, and the last band never uses it).
- Verify print-precision actually exercises the tolerance: at %.4f the
  spectra compared EQUAL (deltas hid below the grid) — bumped to %.6f to
  prove real float-vs-double deltas flow through the comparison.

NEXT: celt_decoder.c top level (flags, dynalloc, tf_decode, postfilter,
denormalise + IMDCT + deemphasis, PLC stub, energy state rotation), then
TOC/packet framing, then decode real CELT-only packets vs opus_demo.

## O1 MILESTONE (2026-07-10): CELT-only Opus decoding WORKS end-to-end

glint now decodes REAL Opus streams (CELT-only / restricted-lowdelay,
fullband 48 kHz) identically to libopus:

- **opus_celt_decoder.{hpp,cpp}**: full frame decode — silence/postfilter/
  transient/intra flags, tf_decode, spread, dynalloc, trim, allocation,
  energy layers, quant_all_bands, anti-collapse, denormalise, IMDCT
  overlap chain, comb-filter postfilter (5-tap, cross-faded), de-emphasis,
  energy/state rotation. Mono decoders set disable_inv=1 (celt_decoder_init
  policy — easy to miss).
- **opus_decoder.{hpp,cpp}**: TOC + framing codes 0-3 (padding, VBR),
  pcm_soft_clip (the int16 API's ±1 overshoot parabola).
- Gates:
  - tools/crosscheck_opus_celt_dec.py — frame-level fuzz vs
    opus_custom_decode_float with state carryover; PASS. NOTE: garbage
    streams drive gains to ~2^32, so the float oracle's rounding noise
    scales with the sequence's PEAK sample (persists via overlap memory);
    tolerance must be peak-scaled or it false-fails.
  - **tools/test_opus_e2e.py — the real-stream gate**: opus_demo
    (restricted-lowdelay) encodes sweep+noise+bursts at 2.5/5/10/20 ms,
    mono+stereo, VBR+CBR; glint decodes every packet with the decoder
    final range EQUAL to the encoder's (Opus conformance identity, exact,
    2206/2206 packets) and PCM within 3 int16 LSB of opus_demo's decode.
- Debug war story: 4 configs showed up-to-646-LSB PCM diffs while ALL
  final ranges matched -> integer path proven right, suspicion on floats;
  frame-level A/B on the same stream showed ZERO diffs -> the difference
  was opus_demo's int16 API applying opus_pcm_soft_clip (codec overshoot
  beyond ±1 on low-rate bursty content). Ported pcm_soft_clip; all green.
  Lesson: "final range equal + PCM off" means look OUTSIDE the decoder.
- opus_demo lives at ~/code/glint-tools/opus_demo (built from the custom
  tree; needs -I silk for debug.h). .bit format: per packet 4B BE length +
  4B BE encoder final range + payload.

O1 remainder (small): CELT-only narrower bandwidths (end<21 — NB/WB/SWB
TOC configs 16..27), stream-channels != decoder-channels up/downmix paths,
PLC (celt_decode_lost). Then O2 (SILK), O3 (Ogg).

## O1 remainder (2026-07-10): endband + channel mixing DONE; PLC deferred

- decode_frame now takes (stream_channels, end_band): NB/WB/SWB/FB CELT
  configs map to end 13/17/19/21 (kEndband in opus_decoder.cpp), and
  mono<->stereo mismatches use the reference synthesis paths (mono->stereo:
  freq copy staged INSIDE ch1's output region, in-place IMDCT trick;
  stereo->mono: average denormalised spectra). Decode loops use stream C;
  memmove/postfilter/deemphasis use decoder CC.
- E2E gate now 18 configs (bandwidths + both mismatch directions +
  VBR/CBR + all frame sizes): ALL final ranges exact, PCM <= 4 LSB.
- Still open in O1: PLC (celt_decode_lost) — deferred to the O2 arc since
  opus_decode_native's PLC clock matters for SILK too.

## O2 SILK decoder — architecture survey (2026-07-10) + plan

SILK is FIXED-POINT throughout (int16/int32 exact) => every fuzz oracle
gives BYTE-IDENTICAL comparisons, no float tolerances anywhere until the
final resampler output (also integer). Decode call graph (silk/):

  opus_decode_native -> silk_Decode (dec_API.c, 486 l): TOC->internal rate
    (NB 8k / MB 12k / WB 16k), stereo weights + MS->LR, resampling to API
    rate, PLC hookup
  -> silk_decode_frame (169 l)
     -> silk_decode_indices (decode_indices.c): VAD/LBRR flags ->
        signalType/quantOffset; gains (independent: silk_gain_iCDF[sig]
        MSB<<3 + uniform8 LSB; conditional: delta_gain); NLSF stage-1 idx
        (CB1_iCDF by sig>>1) + per-coef stage-2 via ec_iCDF[ec_ix] with
        NLSF_EXT extension at 0 / 2*QUANT_MAX; interp factor (20 ms only);
        voiced: pitch lag abs(hi*fs/2+lo)/delta, contour, LTP PERIndex +
        per-subframe LTPIndex, LTP_scaleIndex (independent only); LCG seed
        (uniform4)
     -> silk_decode_pulses (decode_pulses.c): rate level (by sig>>1),
        per-16-sample-block sum_pulses with LSB-extension chain (>16 =>
        nLshifts++, table row 9 (+1 to forbid extension at 10 LSBs)),
        shell decoder = fixed binary-split tree (tables 3->2->1->0),
        LSB bits per sample, signs (icdf built from silk_sign_iCDF
        [7*(2*sig+qoffset) + min(p,6)], value map 2*a-1)
     -> silk_decode_parameters (115 l): gains dequant (log domain),
        NLSF_decode -> NLSF2A -> LPC coefs Q12 (both interp halves),
        LTP coef table lookup, pitch lags from contour CB
     -> silk_decode_core (237 l): LCG noise injection at quantOffset,
        LTP filter (5-tap) + LPC synthesis (order 10/16) per subframe,
        Q14 excitation -> Q10 output, rewhitening on signal-type change
  -> stereo: silk_stereo_decode_pred + MS->LR (3-tap lowpass mix)
  -> resamplers (resampler.c + private_*): IIR+FIR fixed-point, 8/12/16k
     -> 48k output for the Opus layer

Order of implementation (each with a byte-exact fuzz oracle):
1. opus_silk_tables.hpp (generator running/DONE — see below).
2. Excitation: shell decoder + decode_pulses + decode_signs.
3. decode_indices (needs SideInfoIndices struct + decoder-state stubs).
4. NLSF_decode + NLSF2A (+ NLSF_unpack, bwexpander, LPC_inv_pred_gain
   stabilization chain) — isolated, ideal agent piece.
5. Gains dequant (log2lin etc.) + decode_parameters + decode_core.
6. decode_frame + silk_Decode + stereo + resamplers; CNG/PLC.
7. opus_decode_native integration (mode switch, redundancy) -> hybrid
   with CELT -> RFC test vectors 01-12 via opus_compare (O2 exit gate).

Key constants: SHELL_CODEC_FRAME_LENGTH 16 (LOG2 4), SILK_MAX_PULSES 16,
N_RATE_LEVELS 10, MAX_NB_SHELL_BLOCKS 20, MAX_LPC_ORDER 16,
NLSF_QUANT_MAX_AMPLITUDE 4, silk_dec_map(a) = 2a-1.

## O2 first bricks (2026-07-10): SILK tables + fixed-point kit DONE

- **src/opus_silk_tables.hpp** GENERATED by tools/gen_opus_silk_tables.py
  (all decoder tables incl. NLSF codebooks NB/MB+WB, pitch contours,
  shell-code rows, LSF cos table, resampler ROM; encoder-only tables
  skipped with the list documented in the header banner; iCDF rows and
  codebook shapes verified at gen time).
- **src/opus_silk_math.hpp**: the exact fixed-point kit (SMULWB/SMLAWB/
  SMULWW/SMMUL family, saturating adds/shifts, RSHIFT_ROUND, CLZ/CLZ_FRAC,
  DIV32_varQ / INVERSE32_varQ, lin2log/log2lin, silk_RAND). Gate:
  tools/crosscheck_opus_silk_math.py — 2M randomized operand sets,
  BYTE-IDENTICAL hash vs the reference inline kit. Two reconstruction
  bugs the gate caught immediately: the varQ Newton steps use SMMUL
  (>>32) + a (1<<29)-residual, and LSHIFT_SAT32 is clamp-THEN-shift
  (low bits zeroed), not saturate-to-MAX.

## O2 progress 2 (2026-07-10): excitation decoder DONE

opus_silk_excitation.{hpp,cpp}: rate level, per-block pulse counts with
LSB-extension chains, the fixed binary-split shell tree, LSB refinement,
signs (PDF by 7*(2*type+offset)+min(p,6), map 2a-1). Gate:
tools/crosscheck_opus_silk_exc.py — 300 fuzzed frames across all frame
lengths (80..320 incl. the 10ms@12k partial block) and signal/offset
types, pulses[] and tells BYTE-IDENTICAL. Note the tables agent's extra
gate: every generated table memcmp'd against libopus.a symbols (76/76).

NEXT in O2: decode_indices (needs SideInfoIndices + state struct),
NLSF_decode + NLSF2A chain (agent-suitable, isolated), gains dequant,
decode_parameters, decode_core (LPC/LTP synthesis — studied, see survey),
decode_frame + silk_Decode + stereo + resamplers, then hybrid + PLC +
RFC test vectors.

## O2 progress 3 (2026-07-10): side-info decode + gains + pitch lags DONE

opus_silk_indices.{hpp,cpp}: DecoderState (set_fs derives subframe/frame/
ltp-mem lengths, per-rate pitch iCDFs, NLSF codebook + LPC order),
SideInfoIndices (ZERO-INIT matters: fields not decoded for a frame type
persist, mirroring the reference state — the fuzz gate caught stale
garbage immediately), decode_indices (types, gains MSB+LSB / delta, two-
stage NLSF indices with rail escapes, interp factor, pitch abs/delta +
contour, LTP per-index + per-subframe + scaling, seed), nlsf_unpack,
gains_dequant (chain-limited, double-step escape, log2lin), decode_pitch
(contour codebooks, clamped). Gate: tools/crosscheck_opus_silk_indices.py
— 400 fuzzed sequences x 3 chained frames (cond coding, prev lag/type
carry), byte-identical incl. tells. NLSF chain (dequant/stabilize/NLSF2A)
in flight with its own gate.

## O2 progress 4 (2026-07-10): NLSF -> LPC chain DONE

opus_silk_nlsf.{hpp,cpp}: residual dequant (backwards prediction, Q10
dead-zone 102), stage-1 combination, nlsf_stabilize (20-round min-delta
repair + sort/clamp fallback), nlsf2a (cos-table interp, P/Q convolution
QA=16, lpc_fit Q17->Q12 with up-to-16-round chirp repair), bwexpander_32,
lpc_inverse_pred_gain (QA=24). Gate:
tools/crosscheck_opus_silk_nlsf.py — 2400 index vectors per path,
byte-identical, with instrumented proof the repair paths actually ran
(stabilize fallback 2080x, LPC_fit expansion 9108x, chirp rounds 19496x).
Math kit gained add_sat16 (gate re-run PASS).
Constants worth knowing: LPC_fit chirp = 65470 (0.999 Q16, NOT 65471);
lpc_inverse_pred_gain thresholds 16773022 (0.99975 Q24) / 107374
(1e-4 Q30) — float-rounded reference expressions.

Remaining in O2: decode_parameters + decode_core (synthesis loop),
decode_frame/silk_Decode + stereo + resamplers, hybrid, PLC, RFC vectors.

## O2 MILESTONE (2026-07-10): full SILK frame decode byte-identical

opus_silk_frame.{hpp,cpp}: decode_parameters (gain chain, NLSF->LPC for
both interpolation halves, LTP codebook taps <<7, pitch lags, LTP scale;
unvoiced zeroes controls AND resets PERIndex), decode_core (Q14 excitation
with LCG sign dither + level adjust + offsets, per-subframe gain-step state
rescaling via DIV32_varQ, voiced rewhitening through lpc_analysis_filter
at k==0 / k==2-with-interp incl. the LTP-scale downscaling on independent
frames, 5-tap LTP with +2 bias counter, order-10/16 LPC synthesis with
+order/2 bias counter, SAT16(RSHIFT_ROUND(SMULWW(.,gain_q10),8)) output),
bwexpander (ROUNDED multiplies on purpose — SMULWB bias destabilizes),
lpc_analysis_filter (wrapping accumulation per reference), decode_frame
(clean path + outBuf history shuffle).

Gate: tools/crosscheck_opus_silk_frame.py — 250 sequences x 4 chained
frames vs the REAL silk_decode_frame (incl. its PLC/CNG upkeep, proving
those are output-neutral on clean streams): xq PCM and tells
BYTE-IDENTICAL. PLC/CNG state upkeep still stubbed (documented in code);
becomes real work in the PLC item.

Remaining in O2: silk_Decode top level (frame-size/rate dispatch, stereo
weights + MS->LR unmix, resamplers to the API rate, LBRR/VAD header
flags), opus_decode_native integration + hybrid, PLC/CNG, RFC vectors.

## O2 progress 5 (2026-07-10): stereo layer DONE; resamplers in flight

opus_silk_stereo.{hpp,cpp}: stereo predictor decode (joint 5x5 MSBs +
uniform3/uniform5 fine, Q13 dequant with half-substep 6554 Q16, pred[0]
-= pred[1] for cascaded application), mid-only flag, MS->LR unmix
(2-sample carry, 8 ms predictor interpolation, 3-tap smoothed mid into
side, saturated L/R). Gate: tools/crosscheck_opus_silk_stereo.py — 300
sequences x 4 chained frames, byte-identical. Resampler module being
built in background with its own byte-exact gate.

Remaining in O2 after resamplers: silk_Decode top level (dec_API.c:
header flags VAD/LBRR per frame, frame-size dispatch, stereo glue incl.
mid-only handling, resample to API rate), opus_decode_native integration
+ hybrid (SILK + CELT in one ec), PLC/CNG, RFC test vectors.

## O2 MILESTONE 2 (2026-07-10): top-level SILK decoder byte-identical

- opus_silk_resampler.{hpp,cpp} (agent-built, mutant-tested gate): the
  four decode-reachable kernels (copy, up2_HQ, IIR_FIR, down_FIR) over
  all 15 (8/12/16 -> 8/12/16/24/48 kHz) pairs, byte-identical across 220
  chained blocks each. Invariants: ratio math on Hz with ROUND-UP Q16,
  first 1 ms flows through the delay-equalization buffer, down_FIR's
  batch loop exits on inLen > 1 (reference quirk, replicated).
- opus_silk_decoder.{hpp,cpp}: silk_Decode equivalent — VAD/LBRR header
  flags + LBRR distribution + LBRR skip-on-normal-decode, stereo pred/
  mid-only glue with the side-reset on mid-only transitions, per-channel
  frame decode with the conditional-coding rules (INDEP_NO_LTP_SCALING
  after skipped side frames), MS->LR or mono carry, resample to API rate,
  stereo_to_mono collapse handling, channel-transition state resets.
  DecoderState::set_fs now mirrors decoder_set_fs faithfully (guarded
  resets incl. LastGainIndex=10, lagPrev=100 on rate change; returns
  "resampler needs reinit").
- Gate: tools/crosscheck_opus_silk_dec.py — 150 sequences x 3 packets
  with mono<->stereo/rate/duration changes at packet boundaries vs the
  reference silk_Decode: PCM and tells BYTE-IDENTICAL.
- Integration bug the gate caught instantly: passing api HZ where the
  resampler init takes KHZ — output lacked the resampler warm-up delay
  while tells still matched ("tell equal + PCM off from sample 1" =
  post-wire plumbing, same lesson family as the soft-clip story).

Remaining in O2: opus layer integration (SILK-only packets in
OpusDecoder; hybrid = SILK WB + CELT >=8k in ONE ec with the CELT start
band 17), PLC/CNG, then RFC test vectors 01-12 as the exit gate.

## O2 MILESTONE 3 (2026-07-10): full three-mode Opus decoding + 9/12 RFC vectors

- OpusDecoder now routes all TOC modes: SILK-only (0..11, internal rate
  from bandwidth, 40/60 ms via chained silk decodes), hybrid (12..15:
  SILK-WB + CELT start band 17 on the SAME range decoder), CELT-only.
  CELT endband by bandwidth incl. the MB->17 mapping.
- **Transition redundancy implemented** (the deferred machinery — real
  voip streams need it: the encoder appends a 5 ms CELT frame when
  switching modes). Hybrid flags it (bit_logp 12 + uint(256)+2 bytes);
  SILK-only IMPLIES it when >=17 bits remain after the SILK data (the
  tell-gate). RangeDecoder::shrink() cuts the redundant bytes from the
  main frame's budget. CELT->SILK: redundant frame continues the old CELT
  state and owns the first 2.5 ms + fade; SILK->CELT: fresh CELT state
  primed by the redundant frame + tail fade; final range = main ^
  redundant. CELT reset on mode switch is gated on !prev_redundancy.
  (Found via a single voip packet with a range mismatch — the last SILK
  packet before a CELT switch.)
- E2E gate now 38 configs incl. SILK NB/MB/WB 10-60 ms and hybrid
  SWB/FB: ALL final ranges exact; SILK-only PCM is BIT-EXACT (0 LSB),
  CELT/hybrid within 4 LSB.
- **Official RFC 6716/8251 test vectors (tools/test_opus_vectors.py):
  9/12 PASS via opus_compare, and 12/12 have ZERO range mismatches
  (23k+ packets)** — the whole wire decode is right; vectors 05/06/10
  fail only on PCM: they exercise NON-redundant mode transitions whose
  crossfades need PLC frames (SILK PLC + celt_decode_lost).

NEXT (the last O2 item): PLC/CNG (celt_decode_lost, silk PLC+CNG, DTX
zero-length frames, transition fades without redundancy) -> expect
12/12 vectors. Then O3 (Ogg) / O4 (CELT encoder).

## O2 COMPLETE (2026-07-10): RFC-CONFORMANT OPUS DECODER — 12/12 test vectors

**tools/test_opus_vectors.py: all 12 official RFC 6716/8251 test vectors
PASS the normative opus_compare procedure (97.1-100% quality), with ZERO
range mismatches across ~20k packets.** The clean-room decoder is
conformant.

Final pieces (this pass):
- SILK PLC + CNG (opus_silk_plc.{hpp,cpp}, agent-built): conceal/update/
  glue + comfort noise, byte-identical over 500 sequences with 605
  concealed frames. KEY reference corrections: PLC/CNG reset LAZILY on an
  fs mismatch inside silk_PLC/silk_CNG (decoder_set_fs does NOT reset
  them); init runs while frame_length==0 (pitch 0, gains 1.0, CNG seed
  3176576). decode_core's voiced-PLC->unvoiced patch writes into ctrl
  (the PLC update must see it).
- CELT PLC (in opus_celt_decoder.cpp, agent-built): noise/CNG branch +
  pitch-based branch (pitch_downsample/pitch_search/_celt_lpc ports;
  remove_doubling NOT needed), prefilter_and_fold, skip_plc, the
  loss-safety energy block. Two latent glint bugs found by its gate:
  background_log_e_ inits to 0 (NOT -28 — only oldLogE gets -28), and
  synthesis passed literal 0 for start (hybrid no-op until then).
- SilkDecoder::decode_lost (dec_API loss path: stored config, prev
  stereo predictors, has_side=!prev_decode_only_middle, LastGainIndex=10
  after loss, prev_decode_only_middle NOT updated).
- opus_decoder.cpp: full transition orchestration — concealment
  recursion for lost/DTX (F20 chunks, F10/F5 clamps), transition fade
  sources in the OLD mode, SILK reset on CELT->SILK, hybrid->SILK
  CELT-silence fade-out frame, gated celt_to_silk application, and the
  vector-10 lesson: **redundancy CANCELS the transition fade**
  (`if (redundancy) transition = 0`) and the SILK-side transition PLC
  decodes AFTER the redundancy parse. Found by bisecting vector 10 to 8
  diverging packets, all CELT->hybrid switches carrying redundancy.
- Final range convention: concealed frames report 0.

All gates green: 301/301 unit, every crosscheck_opus_*.py, E2E 38
configs, 12/12 vectors.

O2 leftovers for later polish: FEC (LBRR decode-on-request), DTX beyond
the per-frame conceal, decoder sample rates != 48k (API resampling
covers 8-48k in SILK; CELT downsample path unimplemented), OPUS_SET_GAIN.
NEXT: O3 (Ogg Opus demux -> decode .opus FILES) and/or O4 (CELT encoder).

## O3 DONE (2026-07-10): Ogg Opus — glint decodes .opus FILES

opus_ogg.{hpp,cpp}: Ogg page parse with CRC (0x04C11DB7 MSB-first, zero
init/xor, CRC field zeroed for the check), packet reassembly (lacing +
cross-page continuation, dangling-partial drop), OpusHead/OpusTags,
edit-list semantics (pre-skip front trim, last-granule end trim, Q7.8 dB
output gain). Mapping families 0 and 1 up to 2 channels (multistream
deferred). tools/opusfile_dec_cli.cpp = .opus -> int16 PCM.

Gate: tools/test_opus_ogg.py — 8 ffmpeg/libopus-encoded .opus configs
(SILK/hybrid/CELT, VBR+CBR, mono+stereo, 10-40 ms frames) decoded and
compared against ffmpeg FORCED to decode via libopus (its native opus
decoder differs at more LSBs): sample counts exact, PCM within 1 LSB.

MERGE CONDITION (user, 2026-07-10): decode + encode both proven correct
=> merge feature/opus to main. Decode side is done; O4 (CELT encoder)
is the remaining gate.

## O4 — CELT-only encoder (STARTED 2026-07-10): plan

This is the MERGE GATE: once the encoder is proven correct (its streams
decode with libopus AND glint's decoder, sane quality), feature/opus
merges to main.

Strategy (mirrors the MP3/AAC history: wire-correct first, quality
iterated later). CELT's implicit allocation means the encoder REUSES the
verified decoder machinery — the wire-coupled integer parts are already
byte-exact: RangeEncoder (O0), laplace_encode, encode_pulses/CWRS,
compute_allocation (needs an encode twin of the skip/intensity/dual
symbol I/O), tables, bits2pulses/caps.

Build order:
1. Forward MDCT (agent in flight): CeltImdct::forward vs
   clt_mdct_forward + TDAC round-trip gate.
2. EC-ref refactor: rate/bands take {RangeEncoder*, RangeDecoder*} so
   the same integer logic reads OR writes the skip/intensity/dual/theta
   symbols (reference uses an `encode` flag on one ec_ctx). Decode gates
   must stay green after the refactor.
3. Encoder-side energy: quant_coarse_energy (two-pass intra/inter with
   budget clamp), quant_fine_energy, quant_energy_finalise;
   amp2Log2/compute_band_energies/normalise_bands (float side).
4. Bands encode paths: alg_quant (op_pvq_search), stereo_split,
   intensity_stereo, stereo_itheta + theta encode, haar/collapse on the
   encode side of quant_band/partition/quant_all_bands.
5. celt_encode_with_ec top level, SIMPLE first: long blocks only
   (transient analysis later), spread normal, trim 5, no dynalloc, no
   prefilter (gain 0), CBR via padding/ec_enc_shrink (needs the O0
   leftovers ec_enc_shrink + ec_enc_patch_initial_bits). Output is a
   VALID stream at any quality.
6. Opus packetization (TOC + code 0) + Ogg mux (opus_ogg writer:
   OpusHead/OpusTags, lacing, granule, CRC) -> glint writes .opus.
7. Gates: (a) every stream decodes with libopus with zero errors +
   glint-decoder/libopus PCM agreement; (b) SNR/quality floor vs source;
   (c) league entry vs libopus/ffmpeg-native via tests/compare_encoders
   once quality work starts.
8. Quality iteration: transients+TF, pitch prefilter (search ported in
   PLC already), dynalloc, trim/spread analysis, VBR. League target:
   beat ffmpeg's native CELT encoder (realistic), approach libopus.

## O4 progress (2026-07-10): energy encoder + Ogg writer + allocator twin

- opus_celt_enc_energy.{hpp,cpp} (agent, byte-identical over 300
  scenarios/617 frames): band energies/amp2Log2/normalise + the full
  two-pass quant_coarse_energy (intra-vs-inter with badness metric +
  biased tell tiebreak, encoder-state snapshot + front-byte restore via
  the new RangeEncoder::buffer()), fine + finalise with error feedback.
- **CRITICAL BUILD INSIGHT: the prebuilt libopus.a is NOT
  float-reproducible** (FLOAT_APPROX celt_log2, NEON inner products, and
  clang FMA contraction inside quant_coarse_energy_impl). Encoder float-
  exactness gates must recompile the reference decision files with
  -ffp-contract=off and pin the glint side the same way. Corollary:
  glint's encoder under default flags is a VALID encoder that is not
  byte-identical to any particular libopus binary — validity (decodes
  correctly everywhere) never depends on flags; only the GATES do.
- Reference subtleties recorded: badness = sum|qi_ideal - qi_coded|,
  intra bias (budget*delayedIntra*loss_rate)/(C*512); max_decay =
  min(16, nbAvailableBytes/8) only when end-start>10 (3.0 for lfe),
  decay bound reads UNCLAMPED oldEBands while prediction uses the
  -9-clamped copy; error[] undefined outside [start,end); encoder budget
  must equal len*8 exactly or fallback thresholds desync.
- Ogg Opus WRITER (opus_ogg.cpp) + allocator encode twin: see commits.

## O4 MILESTONE (2026-07-10): glint ENCODES conformant Opus — MERGE GATE MET

- opus_celt_enc_bands.{hpp,cpp}: encoder-only float32 band coder
  (resynth=0 semantics: masks untransformed, no folding) —
  BYTE-IDENTICAL with libopus quant_all_bands(encode=1) over 150 fuzzed
  frames incl. transients/TF/stereo/hybrid.
- opus_celt_encoder.{hpp,cpp}: the simple-first top level (long blocks,
  no transient/tf/dynalloc/prefilter/intensity, trim 5, CBR), symbol
  sequence mirroring glint's own conformant decoder; preemphasis +
  forward MDCT (double, narrowed to float spectra — validity never
  depends on transform rounding) + the byte-exact layer stack.
- tools/opus_enc_cli.cpp writes opus_demo .bit with TOC + final ranges.
- **Gate (tools/test_opus_encoder.py): 8 configs (mono/stereo, 64-192k,
  2.5-20 ms) — libopus's own decoder VERIFIES our final ranges on every
  stream (the reference certifying our conformance), glint's decoder
  matches libopus within 3 LSB, SNR 20-29 dB.** Gotcha: the SNR must
  align for the 120-sample codec delay (a flat -4.5 dB smells like
  misalignment, not coding).
- Quality is intentionally untuned (analysis decisions are the knobs:
  transients+TF, dynalloc, trim/spread analysis, prefilter, intensity,
  VBR) — the league work comes after the merge.

**MERGE CONDITION SATISFIED: decode (12/12 RFC vectors) + encode
(libopus-certified streams) both correct.**

## O4 quality campaign — baseline (2026-07-10, post-merge)

Listening pack: /Users/christianstrobele/Downloads/glint_samples/
listening_opus/ (glint .opus files at 96/192k + libopus A/B + a
glint-decoded libopus stream + README with regeneration commands).

Delay-aligned SNR vs source, 60 s stereo clips, CBR (ffmpeg-native
column pending a fixed invocation — its output didn't decode):

| clip        | rate | glint | libopus |
|-------------|------|-------|---------|
| electronic  |  96k | 30.0  | 24.4    |
| electronic  | 192k | 34.9  | 26.9    |
| quartet     |  96k | 25.9  | 24.1    |
| quartet     | 192k | 30.4  | 27.5    |
| industrial  |  96k | 15.1  | 14.5    |
| industrial  | 192k | 21.7  | 20.1    |
| piano       |  96k | 18.5  | 22.9    |
| piano       | 192k | 24.0  | 28.1    |

Read: glint's untuned encoder is waveform-faithful (higher raw SNR on
3/4 clips — libopus spends bits perceptually, so judge by EAR/ODG, not
SNR; the MP3/AAC lesson applies). **Piano is the measured loss (-4.4/-4.1
dB): tonal content with note onsets — the pitch PREFILTER (search
already ported for PLC) + transient/TF analysis are the first two
campaign items.** Then: dynalloc, trim/spread analysis, intensity
stereo, VBR, and wiring `tests/compare_encoders.py --codec opus`
(ODG/PEAQ) for the league.

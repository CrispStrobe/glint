# Quality improvement plan

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

## 3. Outer-loop noise shaping (NMR-driven) — TODO, biggest lever for 0–1 kHz noise

Classic LAME-style inner/outer loop. Per band: measure quantization noise vs
the masking threshold (25-Bark spreading + ATH already in `psycho.cpp`, only
used by the VBR path today); amplify scalefactors of bands where noise > mask;
re-run the gain search so global_gain coarsens to pay for it. This is the
mechanism both failed attempts lacked: `feature/iterative-sf-amplify` and
`feature/smr-sf-amplify` kept the gain fixed, so with a full budget every
boost got reverted (see CLAUDE.md history). Tools to use once here:
`scalefac_scale=1` when the 0–7 sf range is too narrow, `preflag` for the
standard HF preemphasis pattern. Also fix the perceptual inversion in the CBR
energy-based scalefactor assignment (loud bands get extra precision today —
backwards; loud bands self-mask).

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

## 6. Perceptual measurement — TODO (prerequisite for honest 3/4 evaluation)

SNR/rolloff/centroid don't register masking-aware improvements (properly
shaped noise can lower SNR while sounding better). Add a per-Bark-band NMR
metric to `tests/measure_audio.py`; optionally wire ViSQOL/PEAQ for a MOS-like
score. A/B against LAME at 256 kbps as the reference target.

## Smaller dials (experiment-sized)

- Per-frame M/S vs L/R decision (estimate bits/NMR both ways, pick per frame).
- Region boundary search (`region0_count`/`region1_count`) via per-sfb
  cumulative bit costs + DP over splits (LAME-style); the fixed heuristic
  wastes bits. Too hot to brute-force inside the gain search.
- Adaptive rounding offset (fixed 0.4054 dead-zone today; LAME adapts by
  tonality).
- VBR: replace the fixed `vbr_target_gain` table with NMR-driven allocation.

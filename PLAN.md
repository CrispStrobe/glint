# Quality improvement plan

Prioritized quality (not speed) work. Anchor facts from the 256 kbps speech
measurements: 65–74% of all error power sits in 0–1 kHz, and several standard
MP3 bitstream tools are unused (`preflag`/`scalefac_scale` never set, Huffman
tables chosen by max-value only, M/S all-or-nothing with a fixed 50/50 channel
bit split).

Gate for every item: `tests/measure_audio.py` on the canonical speech clip
(SNR/seg-SNR/rolloff/centroid must improve or hold), `ffmpeg` backstep check
(zero), unit tests, `double`==`fixed` output identity. Quality changes are
*not* byte-identical to the previous main by design — that's what the metrics
gate is for.

## 1. Real Huffman table selection — DONE (merged)

`choose_huff_table` picked one fixed table per max-value class (always 7 never
8/9, always 10 never 11/12, always 13 never 15, first ESC table with enough
linbits). Count actual bits for every candidate in the same-range group and
pick the minimum, using the pass-3 pair-cost LUTs; every saved bit lets the
gain search land on a finer global_gain at the same bitrate.
Result: −1.7% Huffman bits on speech; SNR +0.05..0.1 dB all tiers; ~1% slower.

## 2. Smarter bit distribution — DONE (merged)

- M/S channel bit split: mid gets bits proportional to energy (clamped
  55–80%), side gets the rest. Was a fixed 50/50; the side channel of
  correlated material needs far fewer bits than mid.
  Result (joint, best): SNR 15.1 → 16.5 dB, seg-SNR +1.5 dB.
- Inter-granule redistribution now uses both channels' energy (was ch 0 only).
  Kept at `-q best` only, 30/70 clamp unchanged: extending it to speed/normal
  measured a small seg-SNR regression, so it stays best-only.

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

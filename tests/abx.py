#!/usr/bin/env python3
"""Terminal ABX listening test.

The objective metrics disagree on some cases (e.g. speech@128: our NMR
prefers glint, PESQ prefers LAME, PEAQ-ODG prefers glint) — ears are the
tiebreaker. This runs a proper double-blind ABX: per trial X is randomly
A or B; you may replay a/b/x as often as you like, then answer. Reports
the one-sided binomial p-value (p < 0.05 over >= 12 trials is the usual
bar for "audibly different").

Usage:
    python tests/abx.py fileA fileB [--trials 12] [--start 10 --dur 8]

Files may be anything ffmpeg decodes. Playback uses afplay (macOS) or
ffplay. Segments are decoded once, time-aligned via cross-correlation and
peak-normalized to remove level/offset tells.
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import tempfile
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_audio as ma  # noqa: E402


def binom_p(correct, n):
    """One-sided P(X >= correct | p=0.5)."""
    from math import comb
    return sum(comb(n, k) for k in range(correct, n + 1)) / 2.0 ** n


def write_wav(path, x, sr):
    pcm = np.clip(x * 32767, -32767, 32767).astype(np.int16)
    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(sr)
    w.writeframes(pcm.tobytes())
    w.close()


def player():
    for cand in (["afplay"], ["ffplay", "-nodisp", "-autoexit",
                              "-loglevel", "quiet"]):
        if shutil.which(cand[0]):
            return cand
    print("no audio player found (afplay/ffplay)")
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file_a")
    ap.add_argument("file_b")
    ap.add_argument("--trials", type=int, default=12)
    ap.add_argument("--start", type=float, default=5.0,
                    help="segment start in seconds")
    ap.add_argument("--dur", type=float, default=8.0,
                    help="segment length in seconds")
    ap.add_argument("--sr", type=int, default=48000)
    ap.add_argument("--prepare-only", action="store_true",
                    help="decode/align/normalize and exit (self-test)")
    args = ap.parse_args()

    sr = args.sr
    a = ma.load_mono(args.file_a, sr)
    b = ma.load_mono(args.file_b, sr)
    # Align b to a so a switch mid-comparison has no timing tell.
    d = ma.align(a, b)
    b = b[d:] if d >= 0 else np.concatenate([np.zeros(-d), b])
    n = min(len(a), len(b))
    s0 = int(args.start * sr)
    s1 = min(int((args.start + args.dur) * sr), n)
    if s1 - s0 < sr:
        print("segment too short — adjust --start/--dur")
        sys.exit(1)
    seg_a, seg_b = a[s0:s1].copy(), b[s0:s1].copy()
    # Peak-normalize both to remove a loudness tell.
    for seg in (seg_a, seg_b):
        pk = np.max(np.abs(seg))
        if pk > 0:
            seg *= 0.9 / pk

    tmp = tempfile.mkdtemp(prefix="abx_")
    pa, pb = os.path.join(tmp, "a.wav"), os.path.join(tmp, "b.wav")
    write_wav(pa, seg_a, sr)
    write_wav(pb, seg_b, sr)
    if args.prepare_only:
        print(f"prepared {pa} and {pb} ({(s1-s0)/sr:.1f}s @ {sr} Hz)")
        return

    play = player()
    print(f"A = {os.path.basename(args.file_a)}")
    print(f"B = {os.path.basename(args.file_b)}")
    print(f"{args.trials} trials, segment {args.start:.0f}s +{args.dur:.0f}s.")
    print("Keys: a/b/x = play, then answer 'a' or 'b' for what X is; q quits.\n")

    rng = random.SystemRandom()
    correct = 0
    done = 0
    for t in range(1, args.trials + 1):
        x_is = rng.choice("ab")
        px = pa if x_is == "a" else pb
        answer = None
        while answer is None:
            k = input(f"[trial {t}] play (a/b/x) or answer (=a/=b), q: ").strip().lower()
            if k == "q":
                answer = "quit"
            elif k in ("a", "b", "x"):
                subprocess.run(play + [{"a": pa, "b": pb, "x": px}[k]])
            elif k in ("=a", "=b"):
                answer = k[1]
        if answer == "quit":
            break
        done += 1
        if answer == x_is:
            correct += 1
            print("  correct\n")
        else:
            print(f"  wrong (X was {x_is.upper()})\n")
    if done:
        p = binom_p(correct, done)
        print(f"{correct}/{done} correct — one-sided binomial p = {p:.4f}")
        print("audibly different (p<0.05)" if p < 0.05 else
              "no significant difference detected")


if __name__ == "__main__":
    main()

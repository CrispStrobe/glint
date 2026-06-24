#!/usr/bin/env python3
"""Deterministic glint speed/quality benchmark.

Generates a stereo speech-like WAV, encodes it at 256 kbps for each quality
tier, validates the MP3s with ffmpeg, prints encoder speed, then delegates
objective quality reporting to tests/measure_audio.py.

Usage:
    python tests/benchmark_encoder.py [path/to/glint_cli] [--path double|fixed]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import wave

import numpy as np


SR = 44100


def generate_stereo_wav(path, seconds):
    rng = np.random.default_rng(7)
    t = np.arange(SR * seconds) / SR

    env = (0.35 + 0.65 * (0.5 + 0.5 * np.sin(2 * np.pi * 2.7 * t)) *
           (0.7 + 0.3 * np.sin(2 * np.pi * 5.3 * t + 0.4)))
    voiced = sum((1.0 / (i + 1)) *
                 np.sin(2 * np.pi * (155 * (i + 1)) * t + i * 0.37)
                 for i in range(12))
    noise = np.convolve(rng.normal(0, 1, len(t)), np.ones(9) / 9, mode="same")
    sig = (voiced * 0.12 + noise * 0.035) * env
    sig += 0.018 * np.sin(2 * np.pi * 4200 * t) * (
        0.5 + 0.5 * np.sin(2 * np.pi * 3.1 * t))

    left = sig
    right = 0.92 * np.roll(sig, 17) + 0.012 * np.sin(2 * np.pi * 820 * t)
    stereo = np.column_stack([left, right])
    stereo = np.clip(stereo / np.max(np.abs(stereo)) * 0.82, -1, 1)
    pcm = (stereo * 32767).astype(np.int16)

    with wave.open(path, "w") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("encoder", nargs="?", default="build/glint_cli")
    parser.add_argument("--path", choices=["double", "fixed"], default=None,
                        help="Signal path for GLINT_MODE=both builds")
    parser.add_argument("--seconds", type=int, default=60)
    args = parser.parse_args()

    if not os.path.isfile(args.encoder):
        print(f"ERROR: encoder not found: {args.encoder}", file=sys.stderr)
        return 1

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    measure = os.path.join(root, "tests", "measure_audio.py")

    with tempfile.TemporaryDirectory() as tmpdir:
        wav_path = os.path.join(tmpdir, "bench_stereo.wav")
        generate_stereo_wav(wav_path, args.seconds)

        mp3s = []
        print("=== Encode Speed ===", flush=True)
        for quality in ["speed", "normal", "best"]:
            mp3 = os.path.join(tmpdir, f"{quality}.mp3")
            cmd = [args.encoder, wav_path, mp3, "-b", "256", "-m", "stereo",
                   "-q", quality]
            if args.path:
                cmd += ["-p", args.path]
            result = run(cmd)
            speed_line = next((line for line in result.stderr.splitlines()
                               if line.startswith("Speed:")), "")
            match = re.search(r"Speed: ([0-9.]+)x", speed_line)
            speed = match.group(1) if match else "?"
            run(["ffmpeg", "-v", "error", "-i", mp3, "-f", "null", "-"])
            print(f"{quality:>6}: {speed}x realtime", flush=True)
            mp3s.append(mp3)

        print("\n=== Objective Quality ===", flush=True)
        subprocess.run([sys.executable, measure, wav_path] + mp3s, check=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

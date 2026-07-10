#!/usr/bin/env python3
"""End-to-end .opus FILE decoding: glint's Ogg demux + decoder vs ffmpeg
decoding through libopus. Covers SILK-only, hybrid and CELT streams, VBR
and CBR, mono and stereo, several frame durations; lengths must match
exactly (pre-skip + granule trimming) and PCM within a few int16 LSB.

Usage: python3 tools/test_opus_ogg.py
"""

import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRCS = ["opus_ec.cpp", "opus_laplace.cpp", "opus_cwrs.cpp",
        "opus_celt_energy.cpp", "opus_celt_rate.cpp", "opus_celt_bands.cpp",
        "opus_mdct.cpp", "opus_celt_decoder.cpp", "opus_decoder.cpp",
        "opus_silk_excitation.cpp", "opus_silk_indices.cpp",
        "opus_silk_nlsf.cpp", "opus_silk_plc.cpp", "opus_silk_frame.cpp",
        "opus_silk_stereo.cpp", "opus_silk_resampler.cpp",
        "opus_silk_decoder.cpp", "opus_ogg.cpp"]
MAX_LSB = 4


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, **kw)


def gen_wav(path, channels, seconds=2):
    n = 48000 * seconds
    rng = 999
    frames = bytearray()
    for i in range(n):
        t = i / 48000.0
        s = 0.4 * math.sin(2 * math.pi * (200 + 80 * t) * t)
        s += 0.2 * math.sin(2 * math.pi * 2333 * t)
        rng = (1664525 * rng + 1013904223) & 0xFFFFFFFF
        s += (rng / 2**31 - 1.0) * 0.04
        for c in range(channels):
            v = max(-0.9, min(0.9, s * (1 if c == 0 else -0.8)))
            frames += struct.pack("<h", int(v * 32767))
    hdr = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + len(frames),
                      b"WAVE", b"fmt ", 16, 1, channels, 48000,
                      48000 * channels * 2, channels * 2, 16, b"data",
                      len(frames))
    with open(path, "wb") as f:
        f.write(hdr + frames)


def main():
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        sys.exit("needs ffmpeg with libopus")
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        cli = os.path.join(tmp, "opusfile_dec_cli")
        run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
             os.path.join(REPO, "tools", "opusfile_dec_cli.cpp")] +
            [os.path.join(REPO, "src", s) for s in SRCS] + ["-o", cli])

        cases = ((1, ["-b:a", "16k", "-application", "voip"]),
                 (1, ["-b:a", "64k", "-application", "audio"]),
                 (1, ["-b:a", "96k", "-vbr", "off"]),
                 (1, ["-b:a", "48k", "-frame_duration", "40"]),
                 (2, ["-b:a", "24k", "-application", "voip"]),
                 (2, ["-b:a", "128k", "-application", "audio"]),
                 (2, ["-b:a", "96k", "-frame_duration", "10"]),
                 (2, ["-b:a", "64k", "-vbr", "off",
                      "-application", "lowdelay"]))
        failures = 0
        for channels, extra in cases:
            wav = os.path.join(tmp, f"in{channels}.wav")
            if not os.path.exists(wav):
                gen_wav(wav, channels)
            opus = os.path.join(tmp, "a.opus")
            ref = os.path.join(tmp, "ref.raw")
            mine = os.path.join(tmp, "mine.raw")
            run([ffmpeg, "-y", "-v", "error", "-i", wav, "-c:a", "libopus"]
                + extra + [opus])
            # Force ffmpeg to DECODE via libopus too (its native decoder
            # differs at more LSBs).
            run([ffmpeg, "-y", "-v", "error", "-c:a", "libopus", "-i",
                 opus, "-f", "s16le", ref])
            r = run([cli, opus, mine])
            msg = r.stdout.decode().strip()

            a = open(ref, "rb").read()
            b = open(mine, "rb").read()
            if len(a) != len(b):
                print(f"FAIL ch={channels} {extra}: length {len(a)//2} vs "
                      f"{len(b)//2}")
                failures += 1
                continue
            worst = 0
            for i in range(0, len(a), 2):
                va = struct.unpack_from("<h", a, i)[0]
                vb = struct.unpack_from("<h", b, i)[0]
                worst = max(worst, abs(va - vb))
            ok = worst <= MAX_LSB
            failures += 0 if ok else 1
            print(f"{'OK' if ok else 'FAIL'} ch={channels} "
                  f"{' '.join(extra)}: {msg}, max |pcm diff| = {worst} LSB")
        if failures:
            sys.exit(f"FAIL: {failures} cases")
        print("PASS: .opus file decoding matches libopus (via ffmpeg)")


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Cross-check glint's multistream (surround) decoder against libopus
opus_multistream_decode_float on ffmpeg-encoded mapping-family-1 .opus
files (5.1 and quad). Ranges (XOR across streams) must match exactly;
PCM within a few LSB (float MDCT). The packet stream is extracted with
glint's conformance-tested OggOpusReader.

Usage: python3 tools/crosscheck_opus_ms.py
"""

import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
PLAIN_SRC = os.path.join(TOOLS_DIR, "opus-1.5.2")
PLAIN_LIB = os.path.join(PLAIN_SRC, ".libs", "libopus.a")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(REPO, "tools", "opus_ms_crosscheck.cpp")
EXTRACT = os.path.join(REPO, "tools", "opus_ms_extract.cpp")
SRCS = ["opus_ec.cpp", "opus_laplace.cpp", "opus_cwrs.cpp",
        "opus_celt_energy.cpp", "opus_celt_rate.cpp", "opus_celt_bands.cpp",
        "opus_celt_decoder.cpp", "opus_decoder.cpp", "opus_ms_decoder.cpp",
        "opus_mdct.cpp", "opus_silk_excitation.cpp",
        "opus_silk_indices.cpp", "opus_silk_nlsf.cpp", "opus_silk_plc.cpp",
        "opus_silk_frame.cpp", "opus_silk_stereo.cpp",
        "opus_silk_resampler.cpp", "opus_silk_decoder.cpp"]


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def gen_multich(path, channels, seconds=4):
    """Distinct tone + envelope per channel so mapping mistakes are loud."""
    n = 48000 * seconds
    with open(path, "wb") as f:
        for i in range(n):
            t = i / 48000.0
            for c in range(channels):
                freq = 220.0 * (c + 1)
                env = 0.5 + 0.5 * math.sin(2 * math.pi * (0.31 + 0.13 * c) * t)
                v = int(9000 * env * math.sin(2 * math.pi * freq * t))
                f.write(struct.pack("<h", v))


def main():
    if not os.path.exists(PLAIN_LIB):
        sys.exit("missing libopus.a in ~/code/glint-tools/opus-1.5.2")
    ff = shutil.which("ffmpeg")
    if not ff:
        sys.exit("ffmpeg required")
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        ref_bin = os.path.join(tmp, "ms_ref")
        glint_bin = os.path.join(tmp, "ms_glint")
        extract_bin = os.path.join(tmp, "ms_extract")
        run([cxx, "-std=c++17", "-O2", "-DUSE_LIBOPUS",
             "-I", os.path.join(PLAIN_SRC, "include"),
             DRIVER, PLAIN_LIB, "-o", ref_bin])
        run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
             DRIVER] + [os.path.join(REPO, "src", s) for s in SRCS] +
            ["-o", glint_bin])
        run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
             EXTRACT, os.path.join(REPO, "src", "opus_ogg.cpp"),
             "-o", extract_bin])

        failures = 0
        cases = (("5.1", 6, "5.1"), ("quad", 4, "quad"))
        for name, channels, layout in cases:
            raw = os.path.join(tmp, f"{name}.raw")
            oga = os.path.join(tmp, f"{name}.opus")
            pkts = os.path.join(tmp, f"{name}.pkts")
            gen_multich(raw, channels)
            run([ff, "-y", "-v", "error", "-f", "s16le", "-ar", "48000",
                 "-ac", str(channels), "-channel_layout", layout,
                 "-i", raw, "-c:a", "libopus", "-b:a", "256k", oga])
            run([extract_bin, oga, pkts], stdout=subprocess.DEVNULL)
            ref = subprocess.run([ref_bin, pkts], check=True,
                                 capture_output=True).stdout
            gl = subprocess.run([glint_bin, pkts], check=True,
                                capture_output=True).stdout
            if ref == gl:
                print(f"OK {name}: byte-identical "
                      f"({len(ref.splitlines())} packets)")
                continue
            worst = 0
            bad = None
            for i, (a, b) in enumerate(
                    zip(ref.splitlines(), gl.splitlines()), 1):
                fa, fb = a.split(), b.split()
                if fa[:5] != fb[:5]:  # idx n <n> rng <rng>
                    bad = (i, a, b)
                    break
                for va, vb in zip(fa[6:], fb[6:]):
                    worst = max(worst, abs(int(va) - int(vb)))
            if bad:
                print(f"FAIL {name} line {bad[0]} (count/range):\n"
                      f"  ref:   {bad[1].decode()[:130]}\n"
                      f"  glint: {bad[2].decode()[:130]}")
                failures += 1
            elif worst > 8:
                print(f"FAIL {name}: worst PCM diff {worst} > 8")
                failures += 1
            else:
                print(f"OK {name}: ranges identical, worst PCM diff "
                      f"{worst} <= 8 ({len(ref.splitlines())} packets)")
        if failures:
            sys.exit(f"FAIL: {failures} cases")
        print("PASS: multistream decode matches libopus "
              "(5.1 + quad, mapping family 1)")


if __name__ == "__main__":
    main()

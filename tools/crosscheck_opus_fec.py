#!/usr/bin/env python3
"""Cross-check glint's SILK in-band FEC decode (OpusDecoder::decode_fec)
against libopus's opus_decode(..., decode_fec=1) over a deterministic 20%
loss pattern: byte-identical int16 PCM + final ranges on SILK-only (WB)
mono and stereo streams; identical ranges + near-exact PCM on a
mode-switching (hybrid) stream. Streams from opus_demo voip -inbandfec.

Usage: python3 tools/crosscheck_opus_fec.py
"""

import math
import os
import struct
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
PLAIN_SRC = os.path.join(TOOLS_DIR, "opus-1.5.2")
PLAIN_LIB = os.path.join(PLAIN_SRC, ".libs", "libopus.a")
OPUS_DEMO = os.path.join(TOOLS_DIR, "opus_demo")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(REPO, "tools", "opus_fec_crosscheck.cpp")
SRCS = ["opus_ec.cpp", "opus_laplace.cpp", "opus_cwrs.cpp",
        "opus_celt_energy.cpp", "opus_celt_rate.cpp", "opus_celt_bands.cpp",
        "opus_celt_decoder.cpp", "opus_decoder.cpp", "opus_mdct.cpp",
        "opus_silk_excitation.cpp", "opus_silk_indices.cpp",
        "opus_silk_nlsf.cpp", "opus_silk_plc.cpp", "opus_silk_frame.cpp",
        "opus_silk_stereo.cpp", "opus_silk_resampler.cpp",
        "opus_silk_decoder.cpp"]


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def gen_speechish(path, channels, seconds=6):
    """Voiced-sounding stimulus (pitch pulses through a resonant filter +
    pauses) so the SILK encoder codes LBRR on active frames."""
    n = 48000 * seconds
    y = [0.0] * n
    state = [0.0, 0.0]
    period = 160
    for i in range(n):
        t = i / 48000.0
        # Syllable-ish envelope with pauses.
        env = max(0.0, math.sin(2 * math.pi * 2.3 * t)) ** 2
        if int(t * 0.7) % 2:
            env *= 0.1
        x = 1.0 if i % period < 2 else 0.0
        period = 140 + int(40 * math.sin(2 * math.pi * 0.31 * t))
        # Two-pole resonator around 500 Hz.
        w = 2 * math.pi * 500 / 48000
        r = 0.97
        yv = x + 2 * r * math.cos(w) * state[0] - r * r * state[1]
        state[1] = state[0]
        state[0] = yv
        y[i] = 0.25 * env * yv
    with open(path, "wb") as f:
        for i in range(n):
            v = max(-32768, min(32767, int(y[i] * 12000)))
            for c in range(channels):
                s = v if c == 0 else int(v * 0.8)
                f.write(struct.pack("<h", s))


def main():
    if not os.path.exists(PLAIN_LIB):
        sys.exit("missing libopus.a in ~/code/glint-tools/opus-1.5.2")
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        ref_bin = os.path.join(tmp, "fec_ref")
        glint_bin = os.path.join(tmp, "fec_glint")
        run([cxx, "-std=c++17", "-O2", "-DUSE_LIBOPUS",
             "-I", os.path.join(PLAIN_SRC, "include"),
             DRIVER, PLAIN_LIB, "-o", ref_bin])
        run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
             DRIVER] + [os.path.join(REPO, "src", s) for s in SRCS] +
            ["-o", glint_bin])

        failures = 0
        # WB forces SILK-only: exact integer end to end, so PCM and
        # ranges must be BYTE-IDENTICAL. The unconstrained-bandwidth case
        # mixes in hybrid frames whose CELT half is float (never
        # byte-exact) and whose mode-switch concealment noise floor may
        # differ around -44 dBFS (PLC/CNG state after SILK<->hybrid
        # switches); there the ranges must still be identical and PCM
        # within a small tolerance — a real FEC desync errs by thousands
        # AND breaks the ranges.
        cases = ((1, 24000, "WB", 0), (2, 32000, "WB", 0),
                 (1, 24000, None, 256))
        for channels, rate, bw, tol in cases:
            raw = os.path.join(tmp, f"s{channels}.raw")
            bit = os.path.join(tmp, f"s{channels}_{bw}.bit")
            if not os.path.exists(raw):
                gen_speechish(raw, channels)
            cmd = [OPUS_DEMO, "-e", "voip", "48000", str(channels),
                   str(rate)]
            if bw:
                cmd += ["-bandwidth", bw]
            cmd += ["-inbandfec", "-loss", "20", raw, bit]
            run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            ref = subprocess.run([ref_bin, bit, str(channels), "960"],
                                 check=True, capture_output=True).stdout
            gl = subprocess.run([glint_bin, bit, str(channels), "960"],
                                check=True, capture_output=True).stdout
            fec_lines = sum(1 for l in ref.splitlines() if b" fec " in l)
            label = f"ch={channels} rate={rate} bw={bw or 'auto'}"
            if fec_lines == 0:
                print(f"FAIL {label}: no FEC frames exercised")
                failures += 1
                continue
            if ref == gl:
                print(f"OK {label}: byte-identical "
                      f"({len(ref.splitlines())} packets, "
                      f"{fec_lines} FEC-recovered)")
                continue
            if tol == 0:
                failures += 1
                for i, (a, b) in enumerate(
                        zip(ref.splitlines(), gl.splitlines()), 1):
                    if a != b:
                        print(f"FAIL {label} line {i}:\n"
                              f"  ref:   {a.decode()[:140]}\n"
                              f"  glint: {b.decode()[:140]}")
                        break
                continue
            # Tolerant compare: ranges exact, PCM within tol.
            worst = 0
            bad = None
            for i, (a, b) in enumerate(
                    zip(ref.splitlines(), gl.splitlines()), 1):
                fa, fb = a.split(), b.split()
                if fa[:6] != fb[:6]:  # idx kind n <n> rng <rng>
                    bad = (i, "header/range", a, b)
                    break
                for va, vb in zip(fa[7:], fb[7:]):
                    worst = max(worst, abs(int(va) - int(vb)))
            if bad:
                print(f"FAIL {label} line {bad[0]} ({bad[1]}):\n"
                      f"  ref:   {bad[2].decode()[:140]}\n"
                      f"  glint: {bad[3].decode()[:140]}")
                failures += 1
            elif worst > tol:
                print(f"FAIL {label}: worst PCM diff {worst} > {tol}")
                failures += 1
            else:
                print(f"OK {label}: ranges identical, worst PCM diff "
                      f"{worst} <= {tol} ({fec_lines} FEC-recovered, "
                      f"hybrid/mode-switch stream)")
        if failures:
            sys.exit(f"FAIL: {failures} cases")
        print("PASS: SILK in-band FEC decode matches libopus "
              "(byte-identical on SILK-only; range-exact + near-exact "
              "PCM on mode-switching streams)")


if __name__ == "__main__":
    main()

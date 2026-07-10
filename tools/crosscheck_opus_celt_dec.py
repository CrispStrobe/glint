#!/usr/bin/env python3
"""Cross-check glint's full CELT frame decoder against libopus.

Fuzz oracle at celt_decode level: persistent decoder instances fed
sequences of random packets (state carryover across frames); return codes
exact, PCM within relative float-vs-double tolerance.

Usage: python3 tools/crosscheck_opus_celt_dec.py
"""

import os
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
CUSTOM_SRC = os.path.join(TOOLS_DIR, "opus-1.5.2-custom")
CUSTOM_LIB = os.path.join(CUSTOM_SRC, ".libs", "libopus.a")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(REPO, "tools", "opus_celt_dec_crosscheck.cpp")
ATOL = 2e-4   # absolute, for near-zero samples
RTOL = 2e-3   # relative, for the wild gains random streams can produce
# Garbage streams drive band gains to ~2^32, so the float oracle's rounding
# noise scales with the LARGEST sample in the sequence (it persists through
# the decoder's overlap memory into later frames). Tolerance must too.
PEAK_TOL = 1e-4

SRCS = ["opus_ec.cpp", "opus_laplace.cpp", "opus_cwrs.cpp",
        "opus_celt_energy.cpp", "opus_celt_rate.cpp", "opus_celt_bands.cpp",
        "opus_mdct.cpp", "opus_celt_decoder.cpp"]


def run(cmd):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def main():
    if not os.path.exists(CUSTOM_LIB):
        sys.exit("missing custom-modes libopus; run "
                 "tools/crosscheck_opus_celt_prims.py once to build it")
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        ref_bin = os.path.join(tmp, "cdec_ref")
        glint_bin = os.path.join(tmp, "cdec_glint")
        run([cxx, "-std=c++17", "-O2", "-DUSE_LIBOPUS", "-DOPUS_BUILD",
             "-DCUSTOM_MODES", "-DUSE_ALLOCA",
             "-I", os.path.join(CUSTOM_SRC, "celt"),
             "-I", os.path.join(CUSTOM_SRC, "include"),
             DRIVER, CUSTOM_LIB, "-o", ref_bin])
        run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
             DRIVER] + [os.path.join(REPO, "src", s) for s in SRCS] +
            ["-o", glint_bin])
        ref = subprocess.run([ref_bin], check=True,
                             capture_output=True).stdout.decode()
        gl = subprocess.run([glint_bin], check=True,
                            capture_output=True).stdout.decode()

    ref_lines = ref.splitlines()
    gl_lines = gl.splitlines()
    if len(ref_lines) != len(gl_lines):
        sys.exit(f"FAIL: line counts differ ({len(ref_lines)} vs "
                 f"{len(gl_lines)})")
    worst = 0.0
    running_peak = 0.0
    for ln, (a, b) in enumerate(zip(ref_lines, gl_lines), 1):
        ta, tb = a.split(), b.split()
        if "pcm" not in ta:
            if a != b:
                sys.exit(f"FAIL line {ln}:\n  ref:   {a[:130]}\n"
                         f"  glint: {b[:130]}")
            running_peak = 0.0  # new decoder instance / seed
            continue
        cut = ta.index("pcm")
        if ta[:cut + 1] != tb[:cut + 1]:
            sys.exit(f"FAIL (header/ret) line {ln}:\n  ref:   {a[:130]}\n"
                     f"  glint: {b[:130]}")
        vals = [(float(sa), float(sb))
                for sa, sb in zip(ta[cut + 1:], tb[cut + 1:])]
        for va, vb in vals:
            running_peak = max(running_peak, abs(va), abs(vb))
        for va, vb in vals:
            d = abs(va - vb)
            lim = max(ATOL, RTOL * max(abs(va), abs(vb)),
                      PEAK_TOL * running_peak)
            worst = max(worst, d / lim)
            if d > lim:
                sys.exit(f"FAIL (pcm) line {ln}: {va} vs {vb} "
                         f"(peak {running_peak:.1f})")
    print(f"PASS: CELT frame decoder matches libopus over 60 fuzzed "
          f"sequences x 6 frames (worst delta at {worst:.2f} of tolerance)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

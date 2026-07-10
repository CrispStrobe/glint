#!/usr/bin/env python3
"""Cross-check glint's CELT energy-envelope decoder against libopus.

Fuzz-based decode equivalence: both decoders consume identical pseudo-random
byte streams (the range decoder accepts anything); every tell must match
EXACTLY, and decoded energies must match within float-vs-double tolerance.

Usage: python3 tools/crosscheck_opus_energy.py
"""

import os
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
CUSTOM_SRC = os.path.join(TOOLS_DIR, "opus-1.5.2-custom")
CUSTOM_LIB = os.path.join(CUSTOM_SRC, ".libs", "libopus.a")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(REPO, "tools", "opus_energy_crosscheck.cpp")
TOL = 2e-3  # accumulated float-vs-double drift through the predictor


def run(cmd):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def main():
    if not os.path.exists(CUSTOM_LIB):
        sys.exit("missing custom-modes libopus; run "
                 "tools/crosscheck_opus_celt_prims.py once to build it")
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        ref_bin = os.path.join(tmp, "energy_ref")
        glint_bin = os.path.join(tmp, "energy_glint")
        run([cxx, "-std=c++17", "-O2", "-DUSE_LIBOPUS", "-DOPUS_BUILD",
             "-DCUSTOM_MODES", "-DUSE_ALLOCA",
             "-I", os.path.join(CUSTOM_SRC, "celt"),
             "-I", os.path.join(CUSTOM_SRC, "include"),
             DRIVER, CUSTOM_LIB, "-o", ref_bin])
        run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
             DRIVER,
             os.path.join(REPO, "src", "opus_ec.cpp"),
             os.path.join(REPO, "src", "opus_laplace.cpp"),
             os.path.join(REPO, "src", "opus_celt_energy.cpp"),
             "-o", glint_bin])
        ref = subprocess.run([ref_bin], check=True,
                             capture_output=True).stdout.decode()
        gl = subprocess.run([glint_bin], check=True,
                            capture_output=True).stdout.decode()

    ref_lines = ref.splitlines()
    gl_lines = gl.splitlines()
    if len(ref_lines) != len(gl_lines):
        sys.exit(f"FAIL: line counts differ ({len(ref_lines)} vs "
                 f"{len(gl_lines)})")
    max_delta = 0.0
    for ln, (a, b) in enumerate(zip(ref_lines, gl_lines), 1):
        ta, tb = a.split(), b.split()
        if "E" not in ta:
            if a != b:
                sys.exit(f"FAIL line {ln}:\n  ref:   {a}\n  glint: {b}")
            continue
        cut = ta.index("E")
        if ta[:cut + 1] != tb[:cut + 1]:  # stage name + tell: exact
            sys.exit(f"FAIL (tell/stage) line {ln}:\n  ref:   "
                     f"{a[:100]}\n  glint: {b[:100]}")
        for va, vb in zip(ta[cut + 1:], tb[cut + 1:]):
            d = abs(float(va) - float(vb))
            max_delta = max(max_delta, d)
            if d > TOL:
                sys.exit(f"FAIL (energy) line {ln}: {va} vs {vb}")
    print(f"PASS: energy decode matches libopus over 40 fuzzed frames "
          f"(tells exact, max energy delta {max_delta:.2e})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

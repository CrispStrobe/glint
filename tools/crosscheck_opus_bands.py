#!/usr/bin/env python3
"""Cross-check glint's CELT band decoder (quant_all_bands + anti_collapse)
against libopus.

Fuzz oracle wired like celt_decoder.c: allocator -> quant_all_bands ->
anti-collapse over identical random streams. Integer lines (masks, seed,
tells, coded/intensity/dual) must match byte-for-byte; the sampled
normalized spectra within float-vs-double tolerance.

Usage: python3 tools/crosscheck_opus_bands.py
"""

import os
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
CUSTOM_SRC = os.path.join(TOOLS_DIR, "opus-1.5.2-custom")
CUSTOM_LIB = os.path.join(CUSTOM_SRC, ".libs", "libopus.a")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(REPO, "tools", "opus_bands_crosscheck.cpp")
TOL = 5e-3  # folding cascades accumulate float-vs-double drift


def run(cmd):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def main():
    if not os.path.exists(CUSTOM_LIB):
        sys.exit("missing custom-modes libopus; run "
                 "tools/crosscheck_opus_celt_prims.py once to build it")
    cxx = os.environ.get("CXX", "c++")
    srcs = ["opus_ec.cpp", "opus_laplace.cpp", "opus_cwrs.cpp",
            "opus_celt_energy.cpp", "opus_celt_rate.cpp",
            "opus_celt_bands.cpp", "opus_celt_enc_bands.cpp",
            "opus_celt_enc_vq.cpp"]
    with tempfile.TemporaryDirectory() as tmp:
        ref_bin = os.path.join(tmp, "bands_ref")
        glint_bin = os.path.join(tmp, "bands_glint")
        run([cxx, "-std=c++17", "-O2", "-DUSE_LIBOPUS", "-DOPUS_BUILD",
             "-DCUSTOM_MODES", "-DUSE_ALLOCA",
             "-I", os.path.join(CUSTOM_SRC, "celt"),
             "-I", os.path.join(CUSTOM_SRC, "include"),
             DRIVER, CUSTOM_LIB, "-o", ref_bin])
        run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
             DRIVER] + [os.path.join(REPO, "src", s) for s in srcs] +
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
    max_delta = 0.0
    for ln, (a, b) in enumerate(zip(ref_lines, gl_lines), 1):
        if not a.startswith("X"):
            if a != b:
                sys.exit(f"FAIL line {ln}:\n  ref:   {a[:150]}\n"
                         f"  glint: {b[:150]}")
            continue
        va, vb = a.split()[1:], b.split()[1:]
        if len(va) != len(vb):
            sys.exit(f"FAIL line {ln}: spectra lengths differ")
        for sa, sb in zip(va, vb):
            d = abs(float(sa) - float(sb))
            max_delta = max(max_delta, d)
            if d > TOL:
                sys.exit(f"FAIL (spectrum) line {ln}: {sa} vs {sb}")
    nenc = sum(1 for l in ref_lines if l.startswith("encb"))
    print(f"PASS: band decode matches libopus over 150 fuzzed frames "
          f"(masks/seeds/tells exact, max spectrum delta {max_delta:.2e}); "
          f"band ENCODE byte-identical over {nenc} fuzzed frames")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Cross-check glint's SILK excitation decoder against libopus.

Fuzz oracle over random streams x all frame lengths/signal types: decoded
pulse vectors and tells must be byte-identical (SILK is exact integer).

Usage: python3 tools/crosscheck_opus_silk_exc.py
"""

import os
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
CUSTOM_SRC = os.path.join(TOOLS_DIR, "opus-1.5.2-custom")
CUSTOM_LIB = os.path.join(CUSTOM_SRC, ".libs", "libopus.a")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(REPO, "tools", "opus_silk_exc_crosscheck.cpp")


def run(cmd):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def main():
    if not os.path.exists(CUSTOM_LIB):
        sys.exit("missing custom-modes libopus; run "
                 "tools/crosscheck_opus_celt_prims.py once to build it")
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        ref_bin = os.path.join(tmp, "se_ref")
        glint_bin = os.path.join(tmp, "se_glint")
        run([cxx, "-std=c++17", "-O2", "-DUSE_LIBOPUS", "-DOPUS_BUILD",
             "-DCUSTOM_MODES", "-DUSE_ALLOCA",
             "-I", os.path.join(CUSTOM_SRC, "silk"),
             "-I", os.path.join(CUSTOM_SRC, "celt"),
             "-I", os.path.join(CUSTOM_SRC, "include"),
             DRIVER, CUSTOM_LIB, "-o", ref_bin])
        run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
             DRIVER, os.path.join(REPO, "src", "opus_ec.cpp"),
             os.path.join(REPO, "src", "opus_silk_excitation.cpp"),
             "-o", glint_bin])
        ref = subprocess.run([ref_bin], check=True,
                             capture_output=True).stdout
        gl = subprocess.run([glint_bin], check=True,
                            capture_output=True).stdout
    if ref == gl:
        print(f"PASS: SILK excitation byte-identical with libopus "
              f"({ref.count(b'seed')} fuzzed frames)")
        return 0
    for i, (a, b) in enumerate(zip(ref.splitlines(), gl.splitlines()), 1):
        if a != b:
            print(f"FAIL line {i}:\n  ref:   {a.decode()[:140]}\n"
                  f"  glint: {b.decode()[:140]}")
            return 1
    print("FAIL: length mismatch")
    return 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Cross-check glint's CELT primitives (Laplace coder + CWRS/PVQ enumeration)
against reference libopus, byte-for-byte.

Compiles tools/opus_celt_prims_crosscheck.cpp twice — once against the
custom-modes libopus build (reference), once against src/opus_{ec,laplace,
cwrs}.cpp (glint) — runs both over an identical randomized script that
interleaves Laplace symbols and PVQ pulse vectors in one range-coder stream,
and requires byte-identical stdout (all values, per-op tells, full buffers).
Wire-compatibility gate for PLAN § O1 primitives.

Usage: python3 tools/crosscheck_opus_celt_prims.py
"""

import os
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
CUSTOM_SRC = os.path.join(TOOLS_DIR, "opus-1.5.2-custom")
CUSTOM_LIB = os.path.join(CUSTOM_SRC, ".libs", "libopus.a")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(REPO, "tools", "opus_celt_prims_crosscheck.cpp")


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def ensure_custom_libopus():
    if os.path.exists(CUSTOM_LIB):
        return
    tarball = os.path.join(TOOLS_DIR, "opus-1.5.2.tar.gz")
    if not os.path.exists(tarball):
        sys.exit("missing libopus tarball; run tools/crosscheck_opus_ec.py "
                 "once to download it")
    tmp = os.path.join(TOOLS_DIR, "opus-tmp-extract")
    run(["rm", "-rf", tmp, CUSTOM_SRC])
    os.makedirs(tmp)
    run(["tar", "xzf", tarball, "-C", tmp])
    os.rename(os.path.join(tmp, "opus-1.5.2"), CUSTOM_SRC)
    os.rmdir(tmp)
    run(["./configure", "--enable-custom-modes", "--disable-shared",
         "--disable-doc", "--disable-extra-programs"], cwd=CUSTOM_SRC)
    run(["make", f"-j{os.cpu_count()}"], cwd=CUSTOM_SRC)


def main():
    ensure_custom_libopus()
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        ref_bin = os.path.join(tmp, "prims_ref")
        glint_bin = os.path.join(tmp, "prims_glint")
        run([cxx, "-std=c++17", "-O2", "-DUSE_LIBOPUS", "-DOPUS_BUILD",
             "-DCUSTOM_MODES", "-DUSE_ALLOCA",
             "-I", os.path.join(CUSTOM_SRC, "celt"),
             "-I", os.path.join(CUSTOM_SRC, "include"),
             DRIVER, CUSTOM_LIB, "-o", ref_bin])
        run([cxx, "-std=c++17", "-O2",
             "-I", os.path.join(REPO, "src"),
             DRIVER,
             os.path.join(REPO, "src", "opus_ec.cpp"),
             os.path.join(REPO, "src", "opus_laplace.cpp"),
             os.path.join(REPO, "src", "opus_cwrs.cpp"),
             "-o", glint_bin])
        ref = subprocess.run([ref_bin], capture_output=True)
        glint = subprocess.run([glint_bin], capture_output=True)
    if ref.returncode or glint.returncode:
        sys.exit(f"FAIL: driver exit codes ref={ref.returncode} "
                 f"glint={glint.returncode} (internal round-trip mismatch)")
    if ref.stdout == glint.stdout:
        nlines = ref.stdout.count(b"\n")
        print(f"PASS: laplace+cwrs byte-identical with libopus "
              f"({nlines} trace lines, 8 seeds x 600 interleaved ops)")
        return 0
    for i, (a, b) in enumerate(zip(ref.stdout.splitlines(),
                                   glint.stdout.splitlines())):
        if a != b:
            print(f"FAIL: first difference at line {i + 1}:")
            print(f"  ref:   {a[:140]!r}")
            print(f"  glint: {b[:140]!r}")
            return 1
    print("FAIL: output length mismatch")
    return 1


if __name__ == "__main__":
    sys.exit(main())

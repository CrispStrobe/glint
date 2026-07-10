#!/usr/bin/env python3
"""Cross-check glint's Opus range coder against the reference libopus one.

Builds libopus (static) in ~/code/glint-tools/ if not present, compiles
tools/opus_ec_crosscheck.cpp twice (reference vs glint), runs both over an
identical randomized op script, and requires BYTE-IDENTICAL stdout: encoded
buffers (full-size and exact-size), per-op tell()/tell_frac() traces, and all
decoded values. This is the wire-compatibility gate for PLAN § O0 — the same
oracle pattern gen_aac_tables.py uses for the AAC tables.

Usage: python3 tools/crosscheck_opus_ec.py
"""

import os
import subprocess
import sys
import tempfile

OPUS_VERSION = "1.5.2"
OPUS_URLS = [
    f"https://downloads.xiph.org/releases/opus/opus-{OPUS_VERSION}.tar.gz",
    f"https://github.com/xiph/opus/releases/download/v{OPUS_VERSION}/opus-{OPUS_VERSION}.tar.gz",
]
TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(REPO, "tools", "opus_ec_crosscheck.cpp")


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def ensure_libopus():
    src_dir = os.path.join(TOOLS_DIR, f"opus-{OPUS_VERSION}")
    lib = os.path.join(src_dir, ".libs", "libopus.a")
    if os.path.exists(lib):
        return src_dir, lib
    os.makedirs(TOOLS_DIR, exist_ok=True)
    tarball = os.path.join(TOOLS_DIR, f"opus-{OPUS_VERSION}.tar.gz")
    if not os.path.exists(tarball):
        for url in OPUS_URLS:
            try:
                run(["curl", "-fL", "-o", tarball, url])
                break
            except subprocess.CalledProcessError:
                continue
        else:
            sys.exit("could not download libopus source")
    run(["tar", "xzf", tarball, "-C", TOOLS_DIR])
    run(["./configure", "--disable-shared", "--disable-doc",
         "--disable-extra-programs"], cwd=src_dir)
    run(["make", f"-j{os.cpu_count()}"], cwd=src_dir)
    if not os.path.exists(lib):
        sys.exit("libopus.a did not build")
    return src_dir, lib


def main():
    src_dir, lib = ensure_libopus()
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        ref_bin = os.path.join(tmp, "ec_ref")
        glint_bin = os.path.join(tmp, "ec_glint")
        run([cxx, "-std=c++17", "-O2", "-DUSE_LIBOPUS", "-DOPUS_BUILD",
             "-I", os.path.join(src_dir, "celt"),
             "-I", os.path.join(src_dir, "include"),
             DRIVER, lib, "-o", ref_bin])
        run([cxx, "-std=c++17", "-O2",
             "-I", os.path.join(REPO, "src"),
             DRIVER, os.path.join(REPO, "src", "opus_ec.cpp"),
             "-o", glint_bin])
        ref_out = subprocess.run([ref_bin], check=True,
                                 capture_output=True).stdout
        glint_out = subprocess.run([glint_bin], check=True,
                                   capture_output=True).stdout
    if ref_out == glint_out:
        nlines = ref_out.count(b"\n")
        print(f"PASS: byte-identical with libopus {OPUS_VERSION} "
              f"({nlines} trace lines, 8 seeds x 2000 ops, "
              f"full + exact-size buffers)")
        return 0
    ref_lines = ref_out.splitlines()
    glint_lines = glint_out.splitlines()
    for i, (a, b) in enumerate(zip(ref_lines, glint_lines)):
        if a != b:
            print(f"FAIL: first difference at line {i + 1}:")
            print(f"  ref:   {a[:120]!r}")
            print(f"  glint: {b[:120]!r}")
            return 1
    print(f"FAIL: length mismatch ({len(ref_lines)} vs {len(glint_lines)} "
          f"lines)")
    return 1


if __name__ == "__main__":
    sys.exit(main())

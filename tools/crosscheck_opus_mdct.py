#!/usr/bin/env python3
"""Cross-check glint's CELT MDCT (backward AND forward) against libopus.

Uses the custom-modes libopus build in ~/code/glint-tools/opus-1.5.2-custom
(built from the release tarball with --enable-custom-modes if missing),
compiles tools/opus_mdct_crosscheck.cpp twice (oracle vs glint), runs both,
and asserts:

  * per config (shift 0..3 stride 1, plus shift 3 / B=8 interleaved),
    for BOTH directions (backward vs clt_mdct_backward_c, forward vs
    clt_mdct_forward_c in the compute_mdcts layout):
    max |glint - libopus| < 2e-4 over every output sample (the oracle is
    FLOAT, so byte identity is impossible; inputs are unit-RMS-scaled)
  * the glint build's built-in O(S^2) direct-formula self-checks PASS
    (< 1e-9) for every config in both directions  [SELFCHECK = backward,
    FWDCHECK = forward; glint output only]
  * glint forward -> glint backward reconstructs the overlapped time
    signal at unit gain (< 1e-9) for every config  [ROUNDTRIP lines,
    glint output only]
  * glint's analytic window matches the mode's window table to < 1e-6
    [WINDOW line, oracle output only]

This is the wire-semantics gate for the PLAN § O1 inverse MDCT and the
§ O4 encoder-side forward MDCT — the same oracle pattern as
crosscheck_opus_ec.py.

Usage: python3 tools/crosscheck_opus_mdct.py
"""

import os
import re
import subprocess
import sys
import tempfile

OPUS_VERSION = "1.5.2"
TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
CUSTOM_DIR = os.path.join(TOOLS_DIR, f"opus-{OPUS_VERSION}-custom")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(REPO, "tools", "opus_mdct_crosscheck.cpp")
GLINT_MDCT = os.path.join(REPO, "src", "opus_mdct.cpp")

MAX_DIFF = 2e-4      # glint double vs libopus float
WINDOW_TOL = 1e-6    # analytic window vs mode->window (float table)


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def ensure_libopus_custom():
    lib = os.path.join(CUSTOM_DIR, ".libs", "libopus.a")
    if os.path.exists(lib):
        return CUSTOM_DIR, lib
    tarball = os.path.join(TOOLS_DIR, f"opus-{OPUS_VERSION}.tar.gz")
    if not os.path.exists(tarball):
        sys.exit(f"missing {tarball} (see crosscheck_opus_ec.py for the "
                 f"download URLs)")
    os.makedirs(CUSTOM_DIR, exist_ok=True)
    run(["tar", "xzf", tarball, "-C", CUSTOM_DIR,
         "--strip-components", "1"])
    run(["./configure", "--enable-custom-modes", "--disable-shared",
         "--disable-doc", "--disable-extra-programs"], cwd=CUSTOM_DIR)
    run(["make", f"-j{os.cpu_count()}"], cwd=CUSTOM_DIR)
    if not os.path.exists(lib):
        sys.exit("custom-modes libopus.a did not build")
    return CUSTOM_DIR, lib


def parse_configs(text):
    """Split output into {config header: [float samples]}."""
    configs = {}
    header = None
    for line in text.splitlines():
        if line.startswith("config "):
            header = line
            configs[header] = []
        elif line.startswith("o ") and header is not None:
            configs[header].append(float(line.split()[2]))
    return configs


def main():
    src_dir, lib = ensure_libopus_custom()
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        ref_bin = os.path.join(tmp, "mdct_ref")
        glint_bin = os.path.join(tmp, "mdct_glint")
        # Oracle: internal celt headers + the static custom-modes lib. The
        # glint window/transform sources are linked in too, for the
        # analytic-window comparison against mode->window.
        run([cxx, "-std=c++17", "-O2", "-DUSE_LIBOPUS", "-DOPUS_BUILD",
             "-DCUSTOM_MODES", "-DUSE_ALLOCA",
             "-I", os.path.join(src_dir, "celt"),
             "-I", os.path.join(src_dir, "include"),
             "-I", os.path.join(REPO, "src"),
             DRIVER, GLINT_MDCT, lib, "-o", ref_bin])
        run([cxx, "-std=c++17", "-O2",
             "-I", os.path.join(REPO, "src"),
             DRIVER, GLINT_MDCT, "-o", glint_bin])
        ref_out = subprocess.run([ref_bin], check=True, capture_output=True,
                                 text=True).stdout
        glint_out = subprocess.run([glint_bin], check=True,
                                   capture_output=True, text=True).stdout

    ok = True

    # 1) Analytic window vs the mode's table (oracle output).
    m = re.search(r"WINDOW maxdelta (\S+)", ref_out)
    if not m:
        print("FAIL: no WINDOW line in oracle output")
        ok = False
    else:
        wd = float(m.group(1))
        status = "PASS" if wd < WINDOW_TOL else "FAIL"
        print(f"{status}: window maxdelta {wd:.3e} (tol {WINDOW_TOL:g})")
        ok &= wd < WINDOW_TOL

    # 2) Direct-formula self-checks + round trip (glint output).
    for tag, what in (("SELFCHECK", "backward vs direct O(S^2)"),
                      ("FWDCHECK", "forward vs direct O(S^2)"),
                      ("ROUNDTRIP", "forward->backward TDAC round trip")):
        checks = re.findall(tag + r" config=(\d+) maxdiff=(\S+) (\w+)",
                            glint_out)
        if len(checks) != 5:
            print(f"FAIL: expected 5 {tag} lines, got {len(checks)}")
            ok = False
        for idx, md, status in checks:
            print(f"{status}: {what}, config {idx}, "
                  f"maxdiff {float(md):.3e} (tol 1e-9)")
            ok &= status == "PASS"

    # 3) Sample-by-sample glint vs libopus.
    ref_cfg = parse_configs(ref_out)
    glint_cfg = parse_configs(glint_out)
    if list(ref_cfg.keys()) != list(glint_cfg.keys()) or not ref_cfg:
        print(f"FAIL: config sets differ "
              f"({list(ref_cfg)} vs {list(glint_cfg)})")
        return 1
    for header in ref_cfg:
        a, b = ref_cfg[header], glint_cfg[header]
        if len(a) != len(b):
            print(f"FAIL: {header}: sample count {len(a)} vs {len(b)}")
            ok = False
            continue
        md = max(abs(x - y) for x, y in zip(a, b))
        status = "PASS" if md < MAX_DIFF else "FAIL"
        print(f"{status}: {header}: {len(a)} samples, "
              f"max |glint - libopus| = {md:.3e} (tol {MAX_DIFF:g})")
        ok &= md < MAX_DIFF

    if ok:
        print(f"PASS: glint CELT MDCT (backward + forward + round trip) "
              f"matches libopus {OPUS_VERSION} "
              f"(all shifts + interleaved transient layout)")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())

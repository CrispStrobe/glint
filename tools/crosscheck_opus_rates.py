#!/usr/bin/env python3
"""Cross-check glint's non-48k decoder output rates against libopus
(opus_decoder_create at 8/12/16/24/48 kHz) using the FEC crosscheck
driver with no losses in the way (the loss pattern still runs — rate
handling must survive PLC/FEC too). SILK-only streams are exact integer
end to end at EVERY api rate (the resampler chain is integer), so PCM +
ranges must be BYTE-IDENTICAL. CELT-only streams go through the float
(glint: double) MDCT: ranges must match exactly, PCM within a few LSB.

Usage: python3 tools/crosscheck_opus_rates.py
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from crosscheck_opus_fec import (PLAIN_LIB, PLAIN_SRC, OPUS_DEMO, REPO,
                                 DRIVER, SRCS, gen_speechish, run)


def main():
    if not os.path.exists(PLAIN_LIB):
        sys.exit("missing libopus.a in ~/code/glint-tools/opus-1.5.2")
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        ref_bin = os.path.join(tmp, "rate_ref")
        glint_bin = os.path.join(tmp, "rate_glint")
        run([cxx, "-std=c++17", "-O2", "-DUSE_LIBOPUS",
             "-I", os.path.join(PLAIN_SRC, "include"),
             DRIVER, PLAIN_LIB, "-o", ref_bin])
        run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
             DRIVER] + [os.path.join(REPO, "src", s) for s in SRCS] +
            ["-o", glint_bin])

        raw = os.path.join(tmp, "s1.raw")
        gen_speechish(raw, 1)
        silk_bit = os.path.join(tmp, "silk.bit")
        run([OPUS_DEMO, "-e", "voip", "48000", "1", "24000",
             "-bandwidth", "WB", "-inbandfec", "-loss", "20", raw,
             silk_bit], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        celt_bit = os.path.join(tmp, "celt.bit")
        run([OPUS_DEMO, "-e", "restricted-lowdelay", "48000", "1",
             "64000", "-framesize", "20", raw, celt_bit],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        failures = 0
        # CELT tolerance is per frame KIND: decoded frames are float-MDCT
        # noise (<= 8 LSB); concealed frames (plc, and fec calls which
        # fall back to PLC on CELT-only streams) run the pitch-PLC's LPC
        # feedback on band-limited history where double-vs-float drift
        # amplifies (measured <= 71 at ds=3) — real bugs break the ranges
        # and the decoded frames, not just concealment tails.
        for name, bit, tol in (("silk-wb", silk_bit, 0),
                               ("celt", celt_bit, 8)):
            for fs in (8000, 12000, 16000, 24000, 48000):
                frame = fs // 50  # 20 ms
                ref = subprocess.run(
                    [ref_bin, bit, "1", str(frame), str(fs)],
                    check=True, capture_output=True).stdout
                gl = subprocess.run(
                    [glint_bin, bit, "1", str(frame), str(fs)],
                    check=True, capture_output=True).stdout
                if ref == gl:
                    print(f"OK {name} fs={fs}: byte-identical "
                          f"({len(ref.splitlines())} packets)")
                    continue
                if tol == 0:
                    failures += 1
                    for i, (a, b) in enumerate(
                            zip(ref.splitlines(), gl.splitlines()), 1):
                        if a != b:
                            print(f"FAIL {name} fs={fs} line {i}:\n"
                                  f"  ref:   {a.decode()[:130]}\n"
                                  f"  glint: {b.decode()[:130]}")
                            break
                    continue
                worst_norm = worst_plc = 0
                bad = None
                since_loss = 99
                for i, (a, b) in enumerate(
                        zip(ref.splitlines(), gl.splitlines()), 1):
                    fa, fb = a.split(), b.split()
                    if fa[:6] != fb[:6]:
                        bad = (i, a, b)
                        break
                    d = max((abs(int(va) - int(vb))
                             for va, vb in zip(fa[7:], fb[7:])),
                            default=0)
                    since_loss = 0 if fa[1] != b"norm" else since_loss + 1
                    # The first good frame after a loss still overlaps
                    # the concealment's diverged synthesis memory.
                    if fa[1] == b"norm" and since_loss > 1:
                        worst_norm = max(worst_norm, d)
                    else:
                        worst_plc = max(worst_plc, d)
                if bad:
                    print(f"FAIL {name} fs={fs} line {bad[0]} "
                          f"(header/range):\n"
                          f"  ref:   {bad[1].decode()[:130]}\n"
                          f"  glint: {bad[2].decode()[:130]}")
                    failures += 1
                elif worst_norm > tol or worst_plc > 128:
                    print(f"FAIL {name} fs={fs}: worst norm diff "
                          f"{worst_norm} > {tol} or plc diff "
                          f"{worst_plc} > 128")
                    failures += 1
                else:
                    print(f"OK {name} fs={fs}: ranges identical, worst "
                          f"decoded diff {worst_norm} <= {tol}, "
                          f"concealment diff {worst_plc} <= 128")
        if failures:
            sys.exit(f"FAIL: {failures} cases")
        print("PASS: decoder output rates match libopus at "
              "8/12/16/24/48 kHz (SILK byte-identical, CELT within LSBs)")


if __name__ == "__main__":
    main()

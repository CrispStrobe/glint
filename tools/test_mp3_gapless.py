#!/usr/bin/env python3
"""MP3 GAPLESS gate for the whole-file decode C ABI (glint_decode_audio).

A LAME/Lavc-encoded MP3 starts with a Xing/Info tag frame that records the
encoder delay and end padding. A decoder that honours it — ffmpeg, Apple's —
returns exactly the samples that were encoded; one that does not plays a
frame of tag silence plus ~1105 samples of priming first (~25 ms late at
44.1 kHz) and runs long at the end. Lined up against something else, that
flams.

Requires sample-exact agreement with ffmpeg, with NO alignment search, on
CBR and VBR MPEG-1 stereo and an MPEG-2 mono stream, and that a stream
without a tag is left untouched.

Usage: python3 tools/test_mp3_gapless.py path/to/libglint.{dylib,so}
"""
import ctypes
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np

CASES = [
    # name, ffmpeg encode args
    ("cbr_stereo_44k", ["-ac", "2", "-ar", "44100", "-b:a", "192k"]),
    ("vbr_stereo_44k", ["-ac", "2", "-ar", "44100", "-q:a", "2"]),
    ("lsf_mono_22k", ["-ac", "1", "-ar", "22050", "-b:a", "64k"]),
    ("no_tag", ["-ac", "2", "-ar", "44100", "-b:a", "128k",
                "-write_xing", "0"]),
]


def glint_decode(lib, data):
    sr, ch, fr = ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
    p = lib.glint_decode_audio(data, len(data), ctypes.byref(sr),
                               ctypes.byref(ch), ctypes.byref(fr))
    if not p:
        return None, 0
    n = fr.value * ch.value
    out = np.ctypeslib.as_array(p, shape=(n,)).copy()
    lib.glint_free(p)
    return out, ch.value


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if not shutil.which("ffmpeg"):
        print("SKIP: ffmpeg not found")
        return 0
    lib = ctypes.CDLL(sys.argv[1])
    lib.glint_decode_audio.restype = ctypes.POINTER(ctypes.c_float)
    lib.glint_decode_audio.argtypes = [ctypes.c_char_p, ctypes.c_int] + \
        [ctypes.POINTER(ctypes.c_int)] * 3
    lib.glint_free.argtypes = [ctypes.c_void_p]

    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, args in CASES:
            path = os.path.join(tmp, name + ".mp3")
            enc = subprocess.run(
                ["ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                 "sine=frequency=440:duration=3", *args, "-c:a",
                 "libmp3lame", path], capture_output=True)
            if enc.returncode != 0:
                print(f"SKIP {name}: no libmp3lame in this ffmpeg")
                continue
            data = open(path, "rb").read()
            g, ch = glint_decode(lib, data)
            ref_raw = subprocess.run(
                ["ffmpeg", "-loglevel", "error", "-i", path, "-f", "f32le",
                 "-"], capture_output=True).stdout
            r = np.frombuffer(ref_raw, dtype="<f4")
            if g is None or ch == 0:
                print(f"FAIL {name}: glint decoded nothing")
                failures += 1
                continue
            if len(g) != len(r):
                print(f"FAIL {name}: glint {len(g) // ch} frames, ffmpeg "
                      f"{len(r) // ch} ({(len(g) - len(r)) // ch:+d})")
                failures += 1
                continue
            err = float(np.sum((g - r) ** 2))
            sig = float(np.sum(r ** 2))
            snr = 10 * np.log10(sig / max(err, 1e-30))
            ok = snr >= 100
            print(f"{'PASS' if ok else 'FAIL'} {name}: {len(g) // ch} "
                  f"frames, unaligned SNR {snr:.1f} dB")
            failures += 0 if ok else 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

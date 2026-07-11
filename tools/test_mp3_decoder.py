#!/usr/bin/env python3
"""MP3 decoder gate (PLAN § D1): decode with glint's Mp3Decoder and with
ffmpeg, require high SNR agreement (both are float ISO decoders; ~90+ dB
is normal, anything under the floor means a real defect). Streams cover
glint's own encoder (all qualities, stereo modes, VBR, MPEG-2, castanets
short blocks) and LAME if available (foreign encoder: different table
choices, scfsi use, MS switching).

Usage: python3 tools/test_mp3_decoder.py [path/to/glint_cli]
"""

import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRCS = ["mp3_decoder.cpp"]


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, **kw)


def gen_wav(path, sr=44100, seconds=3, channels=2, castanet=False):
    n = sr * seconds
    frames = []
    for i in range(n):
        t = i / sr
        if castanet:
            # Click train through a resonance: forces short blocks.
            burst = 1.0 if (i % (sr // 3)) < 40 else 0.0
            v = burst * math.sin(2 * math.pi * 3000 * t)
            v += 0.2 * math.sin(2 * math.pi * 300 * t)
            v *= 0.7
        else:
            v = 0.5 * math.sin(2 * math.pi * 440 * t)
            v += 0.2 * math.sin(2 * math.pi * 1730 * t)
            v *= 0.5 + 0.5 * math.sin(2 * math.pi * 0.9 * t)
        s = int(20000 * v)
        if channels == 2:
            s2 = int(15000 * v * math.cos(2 * math.pi * 0.25 * t))
            frames.append(struct.pack("<hh", s, s2))
        else:
            frames.append(struct.pack("<h", s))
    data = b"".join(frames)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, channels, sr,
                            sr * 2 * channels, 2 * channels, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def strip_xing(mp3, tmp):
    """Remove a leading Xing/Info frame so both decoders see the same
    audio: ffmpeg applies gapless trims from the tag that a raw frame
    decoder cannot reproduce."""
    buf = open(mp3, "rb").read()
    off = 0
    if buf[:3] == b"ID3":
        sz = ((buf[6] & 0x7F) << 21) | ((buf[7] & 0x7F) << 14) | \
             ((buf[8] & 0x7F) << 7) | (buf[9] & 0x7F)
        off = 10 + sz
    if off + 4 <= len(buf) and buf[off] == 0xFF:
        ver = (buf[off + 1] >> 3) & 3
        bidx = (buf[off + 2] >> 4) & 15
        sridx = (buf[off + 2] >> 2) & 3
        pad = (buf[off + 2] >> 1) & 1
        if bidx not in (0, 15) and sridx != 3:
            v1 = ver == 3
            kbps = ([0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192,
                     224, 256, 320] if v1 else
                    [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
                     144, 160])[bidx]
            sr = [44100, 48000, 32000][sridx]
            sr = sr if v1 else (sr // 2 if ver == 2 else sr // 4)
            flen = (144000 if v1 else 72000) * kbps // sr + pad
            frame = buf[off:off + flen]
            if b"Xing" in frame or b"Info" in frame or b"VBRI" in frame:
                out = os.path.join(tmp, "stripped.mp3")
                open(out, "wb").write(buf[off + flen:])
                return out
    return mp3


def snr_vs_ffmpeg(dec_bin, mp3, tmp):
    mp3 = strip_xing(mp3, tmp)
    ours = os.path.join(tmp, "ours.f32")
    ref = os.path.join(tmp, "ref.f32")
    r = run([dec_bin, mp3, ours])
    run(["ffmpeg", "-y", "-v", "error", "-i", mp3, "-f", "f32le", ref])
    fa = np.fromfile(ours, dtype=np.float32).astype(np.float64)
    fb = np.fromfile(ref, dtype=np.float32).astype(np.float64)
    n = min(len(fa), len(fb))
    # Skip warm-up frames and search a small alignment window: ffmpeg
    # honors LAME gapless tags (encoder-delay trim) that a raw frame
    # decoder cannot know about.
    start = 4608 * 2
    stop = min(n, start + 44100 * 4)
    best = -1.0
    seg = fa[start:stop]
    for d in range(-3000, 3001, 2):
        j0, j1 = start + d, stop + d
        if j0 < 0 or j1 > n:
            continue
        ref_seg = fb[j0:j1]
        den = float(np.dot(ref_seg, ref_seg))
        if den <= 0:
            continue
        num = float(np.sum((seg - ref_seg) ** 2))
        best = max(best, 10 * math.log10(den / max(num, 1e-30)))
    return best, r.stdout.decode().strip()


def main():
    glint_cli = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(REPO, "build", "glint_cli")
    cxx = os.environ.get("CXX", "c++")
    lame = shutil.which("lame")
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        dec_bin = os.path.join(tmp, "mp3dec")
        run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
             os.path.join(REPO, "tools", "mp3_dec_cli.cpp")] +
            [os.path.join(REPO, "src", s) for s in SRCS] +
            ["-o", dec_bin])

        tone = os.path.join(tmp, "tone.wav")
        cast = os.path.join(tmp, "cast.wav")
        mono = os.path.join(tmp, "mono.wav")
        gen_wav(tone)
        gen_wav(cast, castanet=True)
        gen_wav(mono, channels=1)
        m2 = os.path.join(tmp, "m2.wav")
        gen_wav(m2, sr=22050)

        cases = []
        for q in ("speed", "normal", "best"):
            for mode in ("stereo", "joint"):
                cases.append((f"glint-{q}-{mode}", tone,
                              [glint_cli, "-b", "128", "-m", mode, "-q", q],
                              85))
        cases.append(("glint-best-joint-castanets", cast,
                      [glint_cli, "-b", "128", "-m", "joint", "-q",
                       "best"], 85))
        cases.append(("glint-vbr", tone,
                      [glint_cli, "-b", "128", "-m", "joint", "-q",
                       "best", "-V", "4"], 85))
        cases.append(("glint-mono", mono,
                      [glint_cli, "-b", "96", "-m", "mono", "-q",
                       "normal"], 85))
        cases.append(("glint-m2-22k", m2,
                      [glint_cli, "-b", "64", "-m", "joint", "-q",
                       "normal"], 85))
        if lame:
            cases.append(("lame-128-joint", tone,
                          [lame, "--quiet", "-b", "128", "-q", "2"], 80))
            cases.append(("lame-castanets", cast,
                          [lame, "--quiet", "-b", "128", "-q", "2"], 80))
            cases.append(("lame-v4-vbr", tone,
                          [lame, "--quiet", "-V", "4"], 80))
            cases.append(("lame-m2-22k", m2,
                          [lame, "--quiet", "-b", "64"], 80))

        for name, wav, enc_cmd, floor in cases:
            mp3 = os.path.join(tmp, name + ".mp3")
            try:
                run(enc_cmd + [wav, mp3])
            except subprocess.CalledProcessError as e:
                print(f"SKIP {name}: encoder failed "
                      f"({e.stderr.decode()[:80]})")
                continue
            try:
                snr, stats = snr_vs_ffmpeg(dec_bin, mp3, tmp)
            except subprocess.CalledProcessError as e:
                print(f"FAIL {name}: decoder crashed/errored: "
                      f"{(e.stdout or b'').decode()[:100]}")
                failures += 1
                continue
            ok = snr >= floor
            failures += 0 if ok else 1
            print(f"{'OK' if ok else 'FAIL'} {name}: SNR vs ffmpeg "
                  f"{snr:.1f} dB (floor {floor}) [{stats}]")

        # Intensity stereo: no encoder emits it, so hand-build valid
        # IS frames and check glint == ffmpeg (the reference for the ISO
        # intensity formulas). Covers whole-spectrum + mid-band bounds,
        # MS+intensity, and the is_pos=7 "no intensity" fallback.
        gen = os.path.join(REPO, "tools", "gen_mp3_intensity.py")
        is_cases = [
            ("is-bound0", ["0"]),
            ("is-bound4", ["4"]),
            ("is-bound8", ["8"]),
            ("is-ms+is", ["4", "--ms-is"]),
            ("is-pos7", ["4", "--pos7"]),
        ]
        for name, gen_args in is_cases:
            mp3 = os.path.join(tmp, name + ".mp3")
            run([sys.executable, gen, mp3] + gen_args)
            try:
                snr, stats = snr_vs_ffmpeg(dec_bin, mp3, tmp)
            except subprocess.CalledProcessError as e:
                print(f"FAIL {name}: decoder errored: "
                      f"{(e.stdout or b'').decode()[:80]}")
                failures += 1
                continue
            ok = snr >= 100
            failures += 0 if ok else 1
            print(f"{'OK' if ok else 'FAIL'} {name}: SNR vs ffmpeg "
                  f"{snr:.1f} dB (floor 100) [{stats}]")

    if failures:
        sys.exit(f"FAIL: {failures} cases")
    print("PASS: glint MP3 decoder matches ffmpeg on every stream "
          "(incl. hand-built intensity-stereo frames)")


if __name__ == "__main__":
    main()

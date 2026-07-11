#!/usr/bin/env python3
"""Live CLI gate (PLAN buckets A+B): exercise glint's codec Swiss-army-
knife end to end — encode, decode, transcode, --info, --rate resample,
--gain/--norm, and stdin/stdout piping — and check the results are valid
(ffmpeg decodes glint's outputs) and faithful (round-trip SNR).

Usage: python3 tools/test_cli.py [path/to/glint_cli]
"""

import math
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


def gen_wav(path, sr=44100, seconds=3, channels=2):
    n = sr * seconds
    t = np.arange(n) / sr
    L = 0.4 * np.sin(2 * math.pi * 440 * t) + 0.1 * np.sin(2 * math.pi * 2000 * t)
    R = 0.4 * np.sin(2 * math.pi * 440 * t) * np.cos(2 * math.pi * 0.3 * t)
    pcm = np.empty(n * channels)
    if channels == 2:
        pcm[0::2] = np.clip(L, -1, 1)
        pcm[1::2] = np.clip(R, -1, 1)
    else:
        pcm[:] = np.clip(L, -1, 1)
    data = (pcm * 20000).astype("<i2").tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, channels, sr,
                            sr * 2 * channels, 2 * channels, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def read_wav(path):
    d = open(path, "rb").read()
    i = d.find(b"data")
    n = struct.unpack("<I", d[i + 4:i + 8])[0]
    a = np.frombuffer(d[i + 8:i + 8 + n], dtype=np.int16).astype(np.float64)
    return a / 32768.0


def wav_info(path):
    d = open(path, "rb").read()
    ch = struct.unpack("<H", d[22:24])[0]
    sr = struct.unpack("<I", d[24:28])[0]
    return sr, ch


def roundtrip_snr(orig, dec, ch):
    """Delay-aligned time-domain SNR of channel 0 (codecs add a delay;
    the round-trip of a clean signal at 128k should be well above the
    quantization floor)."""
    a = orig[::ch] if ch > 1 else orig
    b = dec[::ch] if ch > 1 else dec
    n = min(len(a), len(b))
    s = 8192
    win = min(44100, n - s - 4096)
    if win < 8192:
        return -1.0
    best = -1.0
    seg = a[s:s + win]
    for d in range(-3000, 3001, 2):
        j = s + d
        if j < 0 or j + win > n:
            continue
        ref = b[j:j + win]
        den = float(np.dot(seg, seg))
        if den <= 0:
            continue
        num = float(np.sum((seg - ref) ** 2))
        best = max(best, 10 * math.log10(den / max(num, 1e-30)))
    return best


def main():
    cli = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(REPO, "build", "glint_cli")
    fails = 0

    def check(cond, name):
        nonlocal fails
        print(f"{'OK' if cond else 'FAIL'} {name}")
        if not cond:
            fails += 1

    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, "in.wav")
        gen_wav(wav)
        orig = read_wav(wav)

        # --info
        r = run([cli, "--info", wav])
        info = r.stderr.decode()
        check("44100 Hz" in info and "2 ch" in info and r.returncode == 0,
              "--info reports rate/channels")

        # A 48 kHz twin so the Opus round-trip (Opus is 48 kHz only)
        # compares like-for-like without a resampling-rate mismatch.
        wav48 = os.path.join(tmp, "in48.wav")
        gen_wav(wav48, sr=48000)
        orig48 = read_wav(wav48)

        # encode to each format + decode back + round-trip fidelity
        for fmt, br, src, ref, floor in [
                ("mp3", "128", wav, orig, 25.0),
                ("aac", "128", wav, orig, 25.0),
                ("opus", "96", wav48, orig48, 20.0)]:
            enc = os.path.join(tmp, f"a.{fmt}")
            r = run([cli, "-b", br, "-q", "normal", src, enc])
            check(r.returncode == 0 and os.path.getsize(enc) > 1000,
                  f"encode wav -> {fmt}")
            # ffmpeg must decode glint's output (validity)
            ff = os.path.join(tmp, f"ff_{fmt}.wav")
            rf = run(["ffmpeg", "-y", "-v", "error", "-i", enc, ff])
            check(rf.returncode == 0 and os.path.getsize(ff) > 1000,
                  f"ffmpeg decodes glint's {fmt}")
            # glint decode back to wav + fidelity
            dec = os.path.join(tmp, f"dec_{fmt}.wav")
            r = run([cli, enc, dec])
            check(r.returncode == 0, f"decode {fmt} -> wav")
            back = read_wav(dec)
            snr = roundtrip_snr(ref, back, 2)
            check(snr >= floor,
                  f"{fmt} round-trip SNR {snr:.1f} dB >= {floor}")

        # transcode mp3 -> aac and opus -> mp3
        r = run([cli, "-b", "128", os.path.join(tmp, "a.mp3"),
                 os.path.join(tmp, "x.aac")])
        check(r.returncode == 0 and
              os.path.getsize(os.path.join(tmp, "x.aac")) > 1000,
              "transcode mp3 -> aac")
        r = run([cli, "-b", "128", os.path.join(tmp, "a.opus"),
                 os.path.join(tmp, "x.mp3")])
        check(r.returncode == 0, "transcode opus -> mp3")

        # --rate resample
        r48 = os.path.join(tmp, "r48.wav")
        r = run([cli, "--rate", "48000", wav, r48])
        sr, ch = wav_info(r48)
        check(r.returncode == 0 and sr == 48000 and ch == 2,
              "--rate 48000 resamples")
        # frame count scales by the ratio
        n48 = read_wav(r48)
        check(abs(len(n48) / 2 - len(orig) / 2 * 48000 / 44100) < 100,
              "resampled length correct")

        # --gain -6 dB halves amplitude (~0.501x)
        rg = os.path.join(tmp, "g.wav")
        run([cli, "--gain", "-6", wav, rg])
        g = read_wav(rg)
        ratio = np.sqrt(np.mean(g ** 2)) / np.sqrt(np.mean(orig ** 2))
        check(0.45 < ratio < 0.55, f"--gain -6 dB -> {ratio:.3f}x (~0.50)")

        # --norm peak-normalizes to ~-1 dBFS
        rn = os.path.join(tmp, "n.wav")
        run([cli, "--norm", wav, rn])
        peak = np.max(np.abs(read_wav(rn)))
        target = 10 ** (-1.0 / 20.0)
        check(abs(peak - target) < 0.02,
              f"--norm peak {peak:.3f} ~ {target:.3f}")

        # stdin/stdout pipe: wav | encode mp3 | decode wav
        p1 = subprocess.run([cli, "-F", "mp3", "-b", "128", "-", "-"],
                            stdin=open(wav, "rb"), stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
        p2 = subprocess.run([cli, "-F", "wav", "-", "-"],
                            input=p1.stdout, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
        check(len(p2.stdout) > 100000 and p2.stdout[:4] == b"RIFF",
              "stdin/stdout pipe (wav|mp3|wav)")

    if fails:
        sys.exit(f"FAIL: {fails} checks")
    print("PASS: glint CLI encode/decode/transcode/resample/gain/pipe")


if __name__ == "__main__":
    main()

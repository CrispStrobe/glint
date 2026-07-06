#!/usr/bin/env python3
"""Decode-based AAC regression test.

Encodes a deterministic synthetic signal at several configs, decodes with
ffmpeg (a real, independent decoder), and asserts:
  1. zero decoder errors/warnings on stderr,
  2. time-aligned SNR above per-config floors.

This is the AAC counterpart of the m2_decode_quality gate: unit tests verify
internal consistency, but only a real decoder catches wire bugs.

Usage: test_aac.py <glint_cli> [--keep]
"""

import os
import subprocess
import sys
import tempfile
import wave

import numpy as np

FFMPEG = "ffmpeg"


def write_wav(path, rate, data):
    """data: float array (n, ch) in [-1, 1]."""
    pcm = np.clip(data * 32767.0, -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(pcm.shape[1])
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())


def gen_signal(rate, seconds, stereo, transient=False):
    """Deterministic tones + filtered noise, moderately hard to encode.
    transient=True adds a click/burst train (exercises the short-block
    scheduler and START/SHORT/STOP transitions on the wire)."""
    n = int(rate * seconds)
    t = np.arange(n) / rate
    rng = np.random.default_rng(20260706)
    sig = (0.30 * np.sin(2 * np.pi * 440.0 * t)
           + 0.15 * np.sin(2 * np.pi * 1320.0 * t + 0.3)
           + 0.08 * np.sin(2 * np.pi * 3737.0 * t + 1.1))
    # AM sweep adds spectral movement
    sig += 0.10 * np.sin(2 * np.pi * (300 + 1500 * t / seconds) * t)
    noise = rng.standard_normal(n) * 0.02
    # crude lowpass on the noise
    noise = np.convolve(noise, np.ones(8) / 8.0, mode="same")
    if transient:
        sig = sig * 0.25
        period = int(0.19 * rate)
        for start in range(period // 2, n - 600, period):
            burst = rng.standard_normal(500) * np.exp(-np.arange(500) / 60.0)
            sig[start:start + 500] += 0.6 * burst
    left = sig + noise
    if not stereo:
        return left[:, None] * 0.9
    right = 0.8 * sig + np.roll(noise, 1234)
    return np.stack([left, right], axis=1) * 0.9


def decode(aac_path, wav_path):
    r = subprocess.run(
        [FFMPEG, "-y", "-v", "error", "-i", aac_path, wav_path],
        capture_output=True, text=True)
    return r.stderr.strip()


def read_wav(path):
    with wave.open(path, "rb") as w:
        n = w.getnframes()
        data = np.frombuffer(w.readframes(n), dtype="<i2").astype(np.float64)
        data = data.reshape(-1, w.getnchannels())
        return w.getframerate(), data / 32768.0


def aligned_snr(ref, dec):
    """Align by cross-correlation of the first channel, then SNR over overlap."""
    a = ref[: min(len(ref), 200000), 0]
    b = dec[: min(len(dec), 200000), 0]
    corr = np.correlate(b, a[: len(a) // 2], mode="valid")
    delay = int(np.argmax(corr))
    dec_al = dec[delay:]
    n = min(len(ref), len(dec_al))
    # trim edges (window ramp-in/out)
    lo, hi = 2048, n - 2048
    r = ref[lo:hi]
    d = dec_al[lo:hi, : ref.shape[1]]
    err = r - d
    ps = float(np.sum(r * r))
    pe = float(np.sum(err * err))
    return 10 * np.log10(ps / pe) if pe > 0 else 99.0, delay


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cli = sys.argv[1]
    keep = "--keep" in sys.argv

    configs = [
        # (rate, stereo, kbps, extra_args, snr_floor, transient)
        # Floors are decode-sanity bounds, not quality gates (that's the
        # metrics suite): the distortion-controlled allocator legitimately
        # trades raw SNR on tonal synthetics for masked-noise placement,
        # so floors sit well below the measured values of 2026-07.
        (44100, True, 128, [], 28.0, False),
        (44100, True, 256, [], 37.0, False),
        (44100, False, 64, ["-m", "mono"], 23.0, False),
        (22050, True, 48, [], 25.0, False),
        (48000, True, 128, [], 25.0, False),
        # burst train: exercises the short-block scheduler (START/SHORT/STOP).
        # Low floor: the integer (GLINT_MODE=fixed) profile plus compiler
        # fast-math variance lands this deliberately SNR-hostile config
        # anywhere in the 16-27 dB range across platforms; the floor only
        # guards against outright wire breakage.
        (44100, True, 192, [], 12.0, True),
        # AAC VBR V4 (constant-quality; -b ignored as rate target)
        (44100, True, 128, ["-V", "4"], 25.0, False),
    ]

    tmpdir = tempfile.mkdtemp(prefix="glint_aac_test_")
    failures = []
    for rate, stereo, kbps, extra, floor, transient in configs:
        tag = f"{rate}Hz_{'st' if stereo else 'mo'}_{kbps}k{'_tr' if transient else ''}"
        src = os.path.join(tmpdir, f"src_{tag}.wav")
        enc = os.path.join(tmpdir, f"enc_{tag}.aac")
        dec_wav = os.path.join(tmpdir, f"dec_{tag}.wav")

        ref = gen_signal(rate, 6.0, stereo, transient)
        write_wav(src, rate, ref)

        r = subprocess.run([cli, "-b", str(kbps)] + extra + [src, enc],
                           capture_output=True, text=True)
        if r.returncode != 0:
            failures.append(f"{tag}: encoder failed: {r.stderr[-300:]}")
            continue

        errs = decode(enc, dec_wav)
        if errs:
            failures.append(f"{tag}: decoder errors: {errs[:300]}")
            continue

        _, dec_data = read_wav(dec_wav)
        if stereo and dec_data.shape[1] != 2:
            failures.append(f"{tag}: expected stereo, decoded {dec_data.shape[1]} ch")
            continue
        mono_ref = ref if stereo else ref
        snr, delay = aligned_snr(mono_ref, dec_data)
        status = "ok" if snr >= floor else "FAIL"
        print(f"{tag}: SNR {snr:6.2f} dB (floor {floor}), delay {delay} [{status}]")
        if snr < floor:
            failures.append(f"{tag}: SNR {snr:.2f} < floor {floor}")

    if not keep:
        for f in os.listdir(tmpdir):
            os.unlink(os.path.join(tmpdir, f))
        os.rmdir(tmpdir)

    if failures:
        print("\nFAILURES:")
        for f in failures:
            print("  " + f)
        return 1
    print("\nall AAC decode tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

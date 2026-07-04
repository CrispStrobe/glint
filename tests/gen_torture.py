#!/usr/bin/env python
"""Deterministic synthetic torture clip for codec testing (SQAM-spirit).

60 s, 44.1 kHz stereo: log sweep, dense multitone with AM, glockenspiel-like
decaying tone bursts (sharp onsets + HF harmonics), and stepped band-noise.
Regenerate: python tests/gen_torture.py [out.wav]
"""
import sys
import numpy as np
import wave

sr = 44100
def seg(n): return np.arange(n) / sr

out = []
# 0-15 s: log sweep 40 Hz -> 18 kHz
t = seg(15 * sr)
f0, f1 = 40.0, 18000.0
phase = 2 * np.pi * f0 * (15 * (np.power(f1 / f0, t / 15) - 1) / np.log(f1 / f0))
out.append(0.4 * np.sin(phase))
# 15-30 s: multitone (odd partials of 110 Hz up to 15 kHz) with 4 Hz AM
t = seg(15 * sr)
sig = sum(np.sin(2 * np.pi * f * t + 0.7 * k) / (k + 1)
          for k, f in enumerate(np.arange(110, 15000, 660)))
sig *= (0.55 + 0.45 * np.sin(2 * np.pi * 4 * t))
out.append(0.35 * sig / np.max(np.abs(sig)))
# 30-45 s: glockenspiel-like bursts (sharp onset, inharmonic partials, decay)
t = seg(15 * sr)
sig = np.zeros_like(t)
rng = np.random.RandomState(99)
for pos in np.arange(0.25, 14.5, 0.75):
    i0 = int(pos * sr)
    n = int(0.6 * sr)
    tt = seg(n)
    f = rng.choice([784, 1046, 1568, 2093, 3136])
    burst = sum(a * np.sin(2 * np.pi * f * r * tt) for a, r in
                [(1.0, 1.0), (0.5, 2.76), (0.25, 5.4), (0.12, 8.9)])
    burst *= np.exp(-tt / 0.18)
    sig[i0:i0 + n] += burst[:len(sig) - i0]
out.append(0.5 * sig / np.max(np.abs(sig)))
# 45-60 s: stepped band-limited noise (widening bands)
t = seg(15 * sr)
sig = np.zeros_like(t)
noise = rng.randn(len(t))
spec = np.fft.rfft(noise)
fr = np.fft.rfftfreq(len(t), 1 / sr)
for k, (lo, hi) in enumerate([(100, 500), (500, 2000), (2000, 8000), (8000, 16000), (100, 16000)]):
    m = np.zeros_like(spec)
    sel = (fr >= lo) & (fr < hi)
    m[sel] = spec[sel]
    band = np.fft.irfft(m)
    a, b = int(k * 3 * sr), int((k + 1) * 3 * sr)
    sig[a:b] = band[a:b] / (np.std(band) * 5 + 1e-12)
out.append(0.4 * sig)

mono = np.concatenate(out)
mono = np.clip(mono, -0.99, 0.99)
pcm = (mono * 32767).astype(np.int16)
stereo = np.column_stack([pcm, pcm]).ravel()
path = sys.argv[1] if len(sys.argv) > 1 else "/Users/christianstrobele/Downloads/glint_samples/08_torture_60s.wav"
w = wave.open(path, "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(stereo.tobytes()); w.close()
print("wrote", path)

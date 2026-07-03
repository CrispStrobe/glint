#!/usr/bin/env python
"""Regenerate /tmp/castanet.wav: 20 s click train (0.35 s spacing,
exp-decay bursts) over a quiet 220 Hz bed. 44.1k stereo."""
import numpy as np
import wave

sr = 44100
dur = 20.0
n = int(sr * dur)
t = np.arange(n) / sr
rng = np.random.RandomState(1234)

# quiet 220 Hz bed
sig = 0.05 * np.sin(2 * np.pi * 220.0 * t)

# click train: exp-decay noise bursts every 0.35 s
spacing = 0.35
burst_len = int(0.030 * sr)  # 30 ms decay
decay = np.exp(-np.arange(burst_len) / (0.004 * sr))  # 4 ms time constant
pos = 0.1
while pos + 0.05 < dur:
    i0 = int(pos * sr)
    burst = rng.randn(burst_len) * decay * 0.7
    sig[i0:i0 + burst_len] += burst[:min(burst_len, n - i0)]
    pos += spacing

sig = np.clip(sig, -0.99, 0.99)
pcm = (sig * 32767).astype(np.int16)
stereo = np.column_stack([pcm, pcm]).ravel()

with wave.open("/tmp/castanet.wav", "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(sr)
    w.writeframes(stereo.tobytes())
print("wrote /tmp/castanet.wav")

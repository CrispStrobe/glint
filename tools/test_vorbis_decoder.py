#!/usr/bin/env python3
"""Ogg-Vorbis I DECODER gate: glint vs ffmpeg AND sox(libvorbis).

Vorbis decode is deterministic, so a correct decoder matches a correct
reference to the float32-rounding floor. We build a corpus with sox
(libvorbis) across quality/rate/channel settings, decode each clip with
glint, ffmpeg and sox, align, and require:

  * glint-vs-ffmpeg: raw SNR >= 100 dB and best-scale (shape) SNR >= 120 dB.
    glint reconstructs the exact waveform; a sub-2 ppm global gain (the
    floor1_inverse_dB table-constant precision limit) is removed by the
    best-scale metric, after which the residual sits at the float32 floor.
  * glint-vs-sox: glint agrees with the second reference at least as well as
    ffmpeg does (sox writes reduced-precision f32, so this bar is lower).

usage: test_vorbis_decoder.py <vorbis_dec_cli path>
Skips (exit 0) if sox/ffmpeg/numpy are unavailable.
"""
import os
import shutil
import subprocess
import sys
import tempfile

try:
    import numpy as np
except ImportError:
    print("SKIP: numpy unavailable")
    sys.exit(0)


def have(prog):
    return shutil.which(prog) is not None


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


# Wall-clock guard: a correct decoder is ~realtime*hundreds. A regression to
# an O(N^2) transform hangs on long/large-block clips (the FluidR3Mono.sf3
# low-piano case) — bound every decode so that class of bug fails the gate.
DECODE_TIMEOUT_S = 20


def decode_glint(cli, ogg):
    out = ogg + ".g.f32"
    try:
        r = run([cli, ogg, out], timeout=DECODE_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        raise RuntimeError(
            f"glint decode exceeded {DECODE_TIMEOUT_S}s (hang?)")
    if r.returncode != 0:
        raise RuntimeError("glint decode failed: " + r.stderr.decode())
    sr, ch, frames = (int(x) for x in r.stderr.decode().split()[:3])
    data = np.fromfile(out, dtype=np.float32).astype(np.float64)
    return sr, ch, data.reshape(-1, ch) if ch > 1 else data.reshape(-1, 1)


def decode_ffmpeg(ogg, ch):
    r = run(["ffmpeg", "-v", "error", "-i", ogg, "-f", "f32le",
             "-ac", str(ch), "-"])
    if r.returncode != 0:
        raise RuntimeError("ffmpeg decode failed")
    d = np.frombuffer(r.stdout, dtype=np.float32).astype(np.float64)
    return d.reshape(-1, ch) if ch > 1 else d.reshape(-1, 1)


def decode_sox(ogg, ch):
    r = run(["sox", ogg, "-t", "f32", "-"])
    if r.returncode != 0:
        return None
    d = np.frombuffer(r.stdout, dtype=np.float32).astype(np.float64)
    return d.reshape(-1, ch) if ch > 1 else d.reshape(-1, 1)


def best_lag(a, b, maxlag=6000):
    n = min(len(a), len(b))
    mid = n // 3
    w = min(6000, n // 4)
    if w < 256:
        return 0
    seg = a[mid:mid + w]
    best = (1e18, 0)
    for lag in range(-maxlag, maxlag):
        j = mid + lag
        if j < 0 or j + w > len(b):
            continue
        d = seg - b[j:j + w]
        e = float(np.dot(d, d))
        if e < best[0]:
            best = (e, lag)
    return best[1]


def snr(ref, test):
    noise = test - ref
    p = float(np.dot(ref, ref))
    e = float(np.dot(noise, noise))
    return 999.0 if e == 0 else 10.0 * np.log10(p / e)


def compare(g, r, trim=3000):
    """Align channel 0, return (raw_dB, scaled_dB) over all channels."""
    lag = best_lag(g[:, 0], r[:, 0])
    if lag >= 0:
        a, b = g, r[lag:lag + len(g)]
    else:
        a, b = g[-lag:], r[:len(g) + lag]
    n = min(len(a), len(b))
    if n <= 2 * trim + 512:
        return None
    a = a[trim:n - trim].reshape(-1)
    b = b[trim:n - trim].reshape(-1)
    sc = float(np.dot(a, b) / np.dot(a, a)) if np.dot(a, a) > 0 else 0
    return snr(b, a), snr(b, sc * a)


def main():
    if len(sys.argv) < 2:
        print("usage: test_vorbis_decoder.py <vorbis_dec_cli>")
        return 2
    cli = sys.argv[1]
    if not (have("sox") and have("ffmpeg")):
        print("SKIP: sox and ffmpeg required")
        return 0

    tmp = tempfile.mkdtemp(prefix="glint_vorbis_")
    # (rate, channels, quality, duration_s). Short clips at every setting,
    # plus LONG clips (~10 s) that exercise hundreds of long (n1) blocks —
    # the case a short corpus misses and an O(N^2) iMDCT hangs on.
    configs = []
    for rate in (22050, 44100):
        for ch in (1, 2):
            for q in (0, 3, 6, 10):
                configs.append((rate, ch, q, 0.4))
    configs.append((44100, 1, 6, 10.0))
    configs.append((44100, 2, 6, 10.0))
    configs.append((22050, 1, 3, 10.0))

    failures = 0
    tested = 0
    for rate, ch, q, dur in configs:
        wav = os.path.join(tmp, f"src_{rate}_{ch}_{dur}.wav")
        if not os.path.exists(wav):
            # Non-periodic source (pink noise) -> unambiguous alignment.
            r = run(["ffmpeg", "-y", "-v", "error", "-f", "lavfi", "-i",
                     f"anoisesrc=d={dur}:c=pink:r={rate}:a=0.35",
                     "-ac", str(ch), wav])
            if r.returncode != 0:
                continue
        ogg = os.path.join(tmp, f"c_{rate}_{ch}_{q}_{dur}.ogg")
        if run(["sox", wav, "-C", str(q), ogg]).returncode != 0:
            continue
        try:
            gsr, gch, g = decode_glint(cli, ogg)
            f = decode_ffmpeg(ogg, ch)
        except RuntimeError as e:
            print(f"FAIL {rate}/{ch}ch/q{q}/{dur}s: {e}")
            failures += 1
            continue
        if gsr != rate or gch != ch:
            print(f"FAIL {rate}/{ch}ch/q{q}/{dur}s: sr/ch mismatch "
                  f"({gsr}/{gch})")
            failures += 1
            continue
        res = compare(g, f)
        if res is None:
            continue
        raw, scaled = res
        tested += 1
        # Second reference: glint and ffmpeg vs sox should agree equally.
        s = decode_sox(ogg, ch)
        sox_note = ""
        if s is not None:
            gs = compare(g, s)
            fs = compare(f, s)
            if gs and fs:
                sox_note = (f"  sox: glint={gs[0]:.0f} ffmpeg={fs[0]:.0f} dB")
                if gs[0] < fs[0] - 3.0:
                    print(f"FAIL {rate}/{ch}ch/q{q}: glint-vs-sox {gs[0]:.1f} "
                          f"< ffmpeg-vs-sox {fs[0]:.1f} - 3")
                    failures += 1
        ok = raw >= 100.0 and scaled >= 120.0
        print(f"{'ok ' if ok else 'FAIL'} {rate}/{ch}ch/q{q:2d}/{dur:.0f}s: "
              f"raw={raw:.1f} scaled={scaled:.1f} dB{sox_note}")
        if not ok:
            failures += 1

    print(f"\n{tested} configs compared, {failures} failures")
    return 1 if (failures or tested == 0) else 0


if __name__ == "__main__":
    sys.exit(main())

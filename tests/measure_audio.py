#!/usr/bin/env python
"""Objective audio-quality measurements for glint output.

Compares one or more encoded files against a reference (the original), reporting
dynamics, spectral shape, and fidelity metrics that surface differences raw SNR
misses (HF bandwidth, dynamic range, where quantization noise sits).

Usage:
    python measure_audio.py REFERENCE FILE [FILE ...]

REFERENCE and FILE may be any format ffmpeg can decode (wav, mp3, ...). All
inputs are decoded to the reference's sample rate, mono-mixed for analysis.
Requires: numpy, scipy, ffmpeg/ffprobe on PATH.
"""
import sys, os, subprocess, tempfile, wave
import numpy as np
import scipy.signal as sig


def probe_sr(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a:0",
         "-show_entries", "stream=sample_rate", "-of", "csv=p=0", path],
        capture_output=True, text=True, check=True).stdout.strip()
    return int(out)


def load_mono(path, sr):
    """Decode any input to mono float64 at the given sample rate."""
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tf:
        tmp = tf.name
    try:
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", path,
                        "-acodec", "pcm_s16le", "-ar", str(sr), "-ac", "2", tmp],
                       check=True)
        w = wave.open(tmp, "rb")
        a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
        a = a.astype(np.float64) / 32768.0
        w.close()
        return a.reshape(-1, 2).mean(axis=1)
    finally:
        os.unlink(tmp)


def align(ref, test, search=4000, win=200000, off=None):
    if off is None:
        off = min(len(ref), len(test)) // 4
    rseg = ref[off:off+win]
    best, bd = -1e9, 0
    for d in range(-search, search + 1):
        ts = test[off+d:off+d+win]
        if len(ts) < win:
            continue
        c = float(np.dot(rseg, ts))
        if c > best:
            best, bd = c, d
    return bd


def st_rms_db(x, win, hop):
    nf = max(0, (len(x) - win) // hop)
    out = np.empty(nf)
    for i in range(nf):
        seg = x[i*hop:i*hop+win]
        out[i] = 10*np.log10(np.mean(seg*seg) + 1e-12)
    return out


def dynamics(x, sr):
    rms = 10*np.log10(np.mean(x*x) + 1e-12)
    peak = 20*np.log10(np.max(np.abs(x)) + 1e-12)
    st = st_rms_db(x, int(0.05*sr), int(0.025*sr))
    active = st[st > (np.max(st) - 60)] if len(st) else np.array([rms])
    dr = np.percentile(active, 95) - np.percentile(active, 10)
    return rms, peak, peak - rms, dr, float(np.std(active))


def spectrum_feats(x, sr):
    f, _, X = sig.stft(x, sr, nperseg=2048)
    psd = (np.abs(X)**2).mean(axis=1) + 1e-15
    centroid = float(np.sum(f*psd)/np.sum(psd))
    hf = 100*psd[f >= 10000].sum()/psd.sum()
    rolloff = float(f[np.searchsorted(np.cumsum(psd)/psd.sum(), 0.95)])
    return centroid, hf, rolloff


def fidelity(ref, test, sr):
    d = align(ref, test)
    t = test[d:] if d >= 0 else test
    n = min(len(ref), len(t))
    trim = int(0.5*sr)
    a = ref[:n][trim:n-trim]
    b = t[:n][trim:n-trim]
    err = a - b
    snr = 10*np.log10(np.sum(a*a)/np.sum(err*err) + 1e-12)
    win = int(0.02*sr); ss = []
    for i in range(0, len(a)-win, win):
        s = np.sum(a[i:i+win]**2); e = np.sum(err[i:i+win]**2)
        if s < 1e-7:
            continue
        ss.append(np.clip(10*np.log10(s/(e+1e-12)), -10, 50))
    seg = float(np.mean(ss)) if ss else float('nan')
    f, _, A = sig.stft(a, sr, nperseg=2048)
    _, _, E = sig.stft(err, sr, nperseg=2048)
    _, _, B = sig.stft(b, sr, nperseg=2048)
    PA = (np.abs(A)**2).sum(axis=1); PE = (np.abs(E)**2).sum(axis=1)
    lsd = float(np.mean(np.sqrt(np.mean(
        (20*np.log10(np.abs(A)+1e-9) - 20*np.log10(np.abs(B)+1e-9))**2, axis=0))))
    bands = [(0,1000),(1000,4000),(4000,8000),(8000,16000),(16000,sr//2)]
    bsnr, nsh = [], []
    tot = PE.sum() + 1e-12
    for lo, hi in bands:
        m = (f >= lo) & (f < hi)
        bsnr.append(10*np.log10(PA[m].sum()/(PE[m].sum()+1e-12)+1e-12))
        nsh.append(100*PE[m].sum()/tot)
    return snr, seg, lsd, bsnr, nsh


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    ref_path, test_paths = sys.argv[1], sys.argv[2:]
    sr = probe_sr(ref_path)
    print("analysis sample rate: %d Hz" % sr)
    ref = load_mono(ref_path, sr)
    items = [(os.path.basename(ref_path), ref)] + \
            [(os.path.basename(p), load_mono(p, sr)) for p in test_paths]

    print("\n=== DYNAMICS (self) ===")
    print("%-22s %7s %7s %7s %7s %7s" %
          ("file", "RMS dB", "Peak dB", "Crest", "DR dB", "Loud s"))
    for name, x in items:
        rms, pk, cr, dr, ls = dynamics(x, sr)
        print("%-22s %7.2f %7.2f %7.2f %7.2f %7.2f" % (name, rms, pk, cr, dr, ls))

    print("\n=== SPECTRUM (self) ===")
    print("%-22s %12s %12s %12s" %
          ("file", "Centroid Hz", "%E>10kHz", "95% rolloff"))
    for name, x in items:
        c, hf, ro = spectrum_feats(x, sr)
        print("%-22s %12.0f %12.2f %12.0f" % (name, c, hf, ro))

    print("\n=== FIDELITY vs reference ===")
    print("%-22s %7s %7s %7s | bandSNR 0-1k 1-4k 4-8k 8-16k 16k+" %
          ("file", "SNR", "segSNR", "LSD"))
    for name, x in items[1:]:
        snr, seg, lsd, bsnr, _ = fidelity(ref, x, sr)
        print("%-22s %7.2f %7.2f %7.2f | %5.1f %5.1f %5.1f %5.1f %5.1f" %
              (name, snr, seg, lsd, *bsnr))

    print("\n=== NOISE SPECTRUM (%% of total error power per band) ===")
    print("%-22s %6s %6s %6s %6s %6s" %
          ("file", "0-1k", "1-4k", "4-8k", "8-16k", "16k+"))
    for name, x in items[1:]:
        _, _, _, _, nsh = fidelity(ref, x, sr)
        print("%-22s %6.1f %6.1f %6.1f %6.1f %6.1f" % (name, *nsh))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""League-table comparison of MP3 encoders on the standard quality clips.

Encodes each clip with every available contender at the given CBR bitrates,
decodes with ffmpeg, and reports SNR / seg-SNR / NMR (the same Bark-band
noise-to-mask metric the project gates on, imported from measure_audio.py),
plus PESQ-WB and STOI for clips flagged as speech, plus encode wall time.

Contenders (auto-skipped when the binary is absent):
    glint-speed / glint-normal / glint-best   (joint stereo, double path)
    lame-q2 / lame-q0                         (the `lame` binary; -q 2 is
                                               near the ffmpeg default, -q 0
                                               is LAME's best effort)
    shine                                     (`shineenc` — fixed-point,
                                               no psymodel: a useful floor)

Usage:
    python tests/compare_encoders.py [--glint build/glint_cli]
        [--shine /tmp/shine/shineenc] [--bitrates 128 256]
        [--mode joint|mono] [--clips name=path[:speech] ...]

    # low-rate mono ladder (PESQ/STOI are most discriminative there):
    python tests/compare_encoders.py --mode mono --bitrates 64 96

Defaults use the canonical clips from CLAUDE.md if present.
Requires: numpy, scipy, ffmpeg; optional: pesq, pystoi, lame, shineenc,
peaqb (PEAQB env or /tmp/peaqb-fast/src/peaqb; build from
github.com/akinori-ito/peaqb-fast), visqol (VISQOL env or
/tmp/visqol/bazel-bin/visqol; bazel build from github.com/google/visqol).
"""

import argparse
import os
import shutil
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_audio as ma  # noqa: E402

try:
    from pesq import pesq as pesq_fn
except ImportError:
    pesq_fn = None
try:
    from pystoi import stoi as stoi_fn
except ImportError:
    stoi_fn = None

PEAQB = os.environ.get("PEAQB", "/tmp/peaqb-fast/src/peaqb")
# The Rust port (`cargo install visqol`) — the upstream bazel build of
# google/visqol v3.3.3 no longer compiles against current Xcode SDKs
# (2022-era TF/zlib pins). The port is conformance-tested against v3.1;
# it needs google/visqol's libsvm model file for fullband mode.
VISQOL = os.environ.get("VISQOL", os.path.expanduser("~/.cargo/bin/visqol"))
VISQOL_MODEL = os.environ.get(
    "VISQOL_MODEL", "/tmp/visqol/model/libsvm_nu_svr_model.txt")

DEFAULT_CLIPS = [
    ("speech", "/tmp/glint_ref.wav", True),
    ("electronic", "/Users/christianstrobele/Downloads/glint_samples/01_music_electronic_60s.wav", False),
    ("quartet", "/Users/christianstrobele/Downloads/glint_samples/02_music_quartet_60s.wav", False),
    ("castanets", "/tmp/castanet.wav", False),
]


def have(binary):
    return shutil.which(binary) or (os.path.isfile(binary) and os.access(binary, os.X_OK))


def contenders(args):
    out = []
    g = args.glint
    ch = args.mode  # "joint" or "mono"
    if have(g):
        for mode in ("speed", "normal", "best"):
            out.append((f"glint-{mode}",
                        lambda i, o, k, m=mode: [g, "-b", str(k), "-m", ch,
                                                 "-q", m, "-p", "double", i, o]))
    lame = shutil.which("lame")
    if lame:
        lm = ["-m", "m"] if ch == "mono" else []
        out.append(("lame-q2", lambda i, o, k: [lame, "--quiet", "-b", str(k),
                                                "-q", "2"] + lm + [i, o]))
        out.append(("lame-q0", lambda i, o, k: [lame, "--quiet", "-b", str(k),
                                                "-q", "0"] + lm + [i, o]))
    sh = args.shine
    if have(sh):
        sm = ["-m"] if ch == "mono" else []
        out.append(("shine", lambda i, o, k: [sh, "-b", str(k)] + sm + [i, o]))
    return out


def write_wav48(path, x):
    import wave
    pcm = np.clip(x * 32767, -32767, 32767).astype(np.int16)
    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(48000)
    w.writeframes(pcm.tobytes())
    w.close()


def aligned_pair48(ref48, mp3, tmpdir, seconds=20):
    """Decode mp3 at 48 kHz, align to ref48, write a trimmed pair."""
    dec = ma.load_mono(mp3, 48000)
    d = ma.align(ref48, dec)
    t = dec[d:] if d >= 0 else dec
    n = min(len(ref48), len(t), 48000 * seconds)
    rp = os.path.join(tmpdir, "pq_ref.wav")
    tp = os.path.join(tmpdir, "pq_test.wav")
    write_wav48(rp, ref48[:n])
    write_wav48(tp, t[:n])
    return rp, tp


def peaq_odg(ref48, mp3, tmpdir):
    """PEAQ basic-model ODG via peaqb-fast. peaqb does NOT time-align, so
    feed it an aligned 48 kHz pair or every score saturates at -4."""
    import re
    rp, tp = aligned_pair48(ref48, mp3, tmpdir)
    out = subprocess.run([PEAQB, "-r", rp, "-t", tp],
                         capture_output=True, text=True, timeout=600).stdout
    m = re.findall(r"ODG: (-?[\d.]+)", out)
    return float(m[-1]) if m else float("nan")


def visqol_mos(ref48, mp3, tmpdir):
    """ViSQOL fullband (audio-mode) MOS-LQO at 48 kHz via visqol-rs."""
    import re
    rp, tp = aligned_pair48(ref48, mp3, tmpdir)
    out = subprocess.run([VISQOL, "--reference_file", rp,
                          "--degraded_file", tp, "fullband",
                          "--similarity_to_quality_model", VISQOL_MODEL],
                         capture_output=True, text=True, timeout=600).stdout
    m = re.search(r"MOS-LQO:\s+([\d.]+)", out)
    return float(m.group(1)) if m else float("nan")


def wav_duration(path):
    import wave
    w = wave.open(path, "rb")
    d = w.getnframes() / w.getframerate()
    w.close()
    return d


def speech_scores(ref16, dec16):
    """PESQ-WB and STOI on an aligned 20 s slice at 16 kHz."""
    d = ma.align(ref16, dec16, search=2000, win=100000)
    t = dec16[d:] if d >= 0 else dec16
    n = min(len(ref16), len(t), 16000 * 20)
    a, b = ref16[:n], t[:n]
    p = s = float("nan")
    if pesq_fn:
        try:
            p = pesq_fn(16000, a, b, "wb")
        except Exception:
            pass
    if stoi_fn:
        try:
            s = float(stoi_fn(a, b, 16000, extended=False))
        except Exception:
            pass
    return p, s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--glint", default="build/glint_cli")
    ap.add_argument("--shine", default="/tmp/shine/shineenc")
    ap.add_argument("--bitrates", type=int, nargs="+", default=[128, 256])
    ap.add_argument("--mode", choices=["joint", "mono"], default="joint")
    ap.add_argument("--clips", nargs="*", default=None,
                    help="name=path[:speech] entries; default = canonical set")
    args = ap.parse_args()

    if args.clips:
        clips = []
        for c in args.clips:
            name, rest = c.split("=", 1)
            speech = rest.endswith(":speech")
            path = rest[:-7] if speech else rest
            clips.append((name, path, speech))
    else:
        clips = [c for c in DEFAULT_CLIPS if os.path.isfile(c[1])]

    encs = contenders(args)
    if not encs:
        print("no encoders found")
        sys.exit(1)
    print("contenders:", ", ".join(n for n, _ in encs))
    print("channel mode:", args.mode)
    has_peaq = have(PEAQB)
    has_visqol = have(VISQOL) and os.path.isfile(VISQOL_MODEL)
    if pesq_fn is None:
        print("(pesq not installed — PESQ column skipped)")
    if stoi_fn is None:
        print("(pystoi not installed — STOI column skipped)")
    if not has_peaq:
        print("(peaqb not found — ODG column skipped)")
    if not has_visqol:
        print("(visqol not found — MOS column skipped)")

    import tempfile
    tmpdir = tempfile.mkdtemp(prefix="enc_cmp_")

    for cname, cpath, is_speech in clips:
        sr = ma.probe_sr(cpath)
        ref = ma.load_mono(cpath, sr)
        ref16 = ma.load_mono(cpath, 16000) if is_speech else None
        ref48 = ma.load_mono(cpath, 48000) if (has_peaq or has_visqol) else None
        dur = wav_duration(cpath)
        for kbps in args.bitrates:
            rows = []
            for ename, argv_fn in encs:
                mp3 = os.path.join(tmpdir, f"{cname}_{kbps}_{ename}.mp3")
                t0 = time.time()
                r = subprocess.run(argv_fn(cpath, mp3, kbps),
                                   capture_output=True, timeout=600)
                dt = time.time() - t0
                if r.returncode != 0 or not os.path.isfile(mp3):
                    rows.append((ename, None))
                    continue
                dec = ma.load_mono(mp3, sr)
                snr, seg, _lsd, _bsnr, _nsh = ma.fidelity(ref, dec, sr)
                nmr_mean, nmr_p95, nmr_pos = ma.nmr_metrics(ref, dec, sr)
                extra = ()
                if is_speech:
                    dec16 = ma.load_mono(mp3, 16000)
                    extra = speech_scores(ref16, dec16)
                odg = peaq_odg(ref48, mp3, tmpdir) if has_peaq else None
                mos = visqol_mos(ref48, mp3, tmpdir) if has_visqol else None
                rows.append((ename, (snr, seg, nmr_mean, nmr_p95, nmr_pos,
                                     dur / dt, extra, odg, mos)))
            hdr = f"{'encoder':<14}{'SNR':>7}{'segSNR':>8}{'NMR':>8}{'p95':>7}{'aud%':>6}{'enc x':>8}"
            if has_peaq:
                hdr += f"{'ODG':>7}"
            if has_visqol:
                hdr += f"{'MOS':>6}"
            if is_speech:
                hdr += f"{'PESQ':>7}{'STOI':>7}"
            print(f"\n=== {cname} @ {kbps} kbps CBR ===")
            print(hdr)
            for ename, m in sorted(rows, key=lambda r: (r[1] is None, r[1][2] if r[1] else 0)):
                if m is None:
                    print(f"{ename:<14}   encode FAILED")
                    continue
                snr, seg, nm, p95, pos, spd, extra, odg, mos = m
                line = f"{ename:<14}{snr:>7.2f}{seg:>8.2f}{nm:>8.2f}{p95:>7.2f}{pos:>6.1f}{spd:>7.0f}x"
                if has_peaq:
                    line += f"{odg:>7.2f}"
                if has_visqol:
                    line += f"{mos:>6.2f}"
                if is_speech and extra:
                    p, s = extra
                    line += f"{p:>7.2f}{s:>7.3f}"
                print(line)


if __name__ == "__main__":
    main()

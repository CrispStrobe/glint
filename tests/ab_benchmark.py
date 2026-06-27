#!/usr/bin/env python3
"""A/B benchmark harness for glint encoder optimization testing.

Compares two encoder binaries (or git refs) with statistical rigor:
  - Pins to a single CPU core via taskset for isolation
  - Runs N iterations per configuration
  - Reports mean, stddev, min, max, and relative speedup
  - Mann-Whitney U test for significance (p-value)
  - Optional quality regression check

Usage:
    # Compare two binaries directly:
    python3 tests/ab_benchmark.py --a build-a/glint_cli --b build-b/glint_cli

    # Compare two git refs (builds each in isolated dirs):
    python3 tests/ab_benchmark.py --ref-a main --ref-b glint-main

    # Customize runs, core, duration:
    python3 tests/ab_benchmark.py --a ./a --b ./b -n 10 --core 3 --seconds 30

    # Include quality comparison:
    python3 tests/ab_benchmark.py --a ./a --b ./b --quality
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path

import numpy as np

SR = 44100
ROOT = Path(__file__).resolve().parent.parent


def generate_test_wav(path, seconds):
    """Generate a deterministic stereo speech-like WAV (same as benchmark_encoder.py)."""
    rng = np.random.default_rng(7)
    t = np.arange(SR * seconds) / SR

    env = (0.35 + 0.65 * (0.5 + 0.5 * np.sin(2 * np.pi * 2.7 * t)) *
           (0.7 + 0.3 * np.sin(2 * np.pi * 5.3 * t + 0.4)))
    voiced = sum((1.0 / (i + 1)) *
                 np.sin(2 * np.pi * (155 * (i + 1)) * t + i * 0.37)
                 for i in range(12))
    noise = np.convolve(rng.normal(0, 1, len(t)), np.ones(9) / 9, mode="same")
    sig = (voiced * 0.12 + noise * 0.035) * env
    sig += 0.018 * np.sin(2 * np.pi * 4200 * t) * (
        0.5 + 0.5 * np.sin(2 * np.pi * 3.1 * t))

    left = sig
    right = 0.92 * np.roll(sig, 17) + 0.012 * np.sin(2 * np.pi * 820 * t)
    stereo = np.column_stack([left, right])
    stereo = np.clip(stereo / np.max(np.abs(stereo)) * 0.82, -1, 1)
    pcm = (stereo * 32767).astype(np.int16)

    with wave.open(path, "w") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())


def build_from_ref(ref, build_dir):
    """Build glint_cli from a git ref into build_dir. Returns path to binary."""
    src_dir = ROOT
    binary = os.path.join(build_dir, "glint_cli")

    # Create a temporary worktree, build, copy binary, clean up
    wt_dir = os.path.join(build_dir, "_worktree")
    try:
        subprocess.run(
            ["git", "worktree", "add", "--detach", wt_dir, ref],
            cwd=src_dir, check=True, capture_output=True, text=True)

        cmake_build = os.path.join(wt_dir, "build")
        os.makedirs(cmake_build, exist_ok=True)
        subprocess.run(
            ["cmake", "..", "-DCMAKE_BUILD_TYPE=Release"],
            cwd=cmake_build, check=True, capture_output=True, text=True)
        subprocess.run(
            ["cmake", "--build", ".", "--parallel", "--target", "glint_cli"],
            cwd=cmake_build, check=True, capture_output=True, text=True)

        src_bin = os.path.join(cmake_build, "glint_cli")
        if not os.path.isfile(src_bin):
            raise FileNotFoundError(f"Build succeeded but binary not found: {src_bin}")
        subprocess.run(["cp", src_bin, binary], check=True)
    finally:
        subprocess.run(
            ["git", "worktree", "remove", "--force", wt_dir],
            cwd=src_dir, capture_output=True)

    return binary


def run_encode(binary, wav_path, mp3_path, quality, core, extra_args=None):
    """Run a single encode, return elapsed seconds parsed from encoder output."""
    cmd = ["taskset", "-c", str(core),
           binary, wav_path, mp3_path,
           "-b", "256", "-m", "stereo", "-q", quality]
    if extra_args:
        cmd.extend(extra_args)

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)

    # Parse "Speed: 23.7x realtime (60.00 sec audio in 2.53 sec)"
    # Note: we don't check returncode here because PGO builds may crash
    # during cleanup after encoding completes successfully.
    m = re.search(r"Speed:\s+([0-9.]+)x\s+realtime\s+\(([0-9.]+)\s+sec audio in\s+([0-9.]+)\s+sec\)",
                  result.stderr)
    if not m:
        raise RuntimeError(f"Could not parse speed from: {result.stderr[-200:]}")

    speed_x = float(m.group(1))
    audio_sec = float(m.group(2))
    elapsed_sec = float(m.group(3))
    return speed_x, audio_sec, elapsed_sec


def measure_quality(binary, wav_path, mp3_path, quality, core):
    """Encode at a given quality tier, decode, measure full quality metrics.

    Returns dict with: snr, seg_snr, rolloff, centroid, hf_pct, decode_errors
    """
    # Encode
    subprocess.run(
        ["taskset", "-c", str(core),
         binary, wav_path, mp3_path, "-b", "256", "-m", "stereo", "-q", quality],
        capture_output=True, text=True, timeout=300)

    # Validate with ffmpeg
    result = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", mp3_path, "-f", "null", "-"],
        capture_output=True, text=True, timeout=60)
    errors = [l for l in result.stderr.split('\n')
              if 'error' in l.lower() and 'no error' not in l.lower()]

    # Decode to WAV
    dec_path = mp3_path.replace(".mp3", "_dec.wav")
    subprocess.run(
        ["ffmpeg", "-y", "-v", "error", "-i", mp3_path,
         "-ar", str(SR), "-ac", "1", "-acodec", "pcm_s16le", dec_path],
        capture_output=True, text=True, check=True, timeout=60)

    ref = load_wav_mono(wav_path)
    dec = load_wav_mono(dec_path)

    # Align
    delay = find_delay(ref, dec)
    t = dec[delay:] if delay >= 0 else dec
    n = min(len(ref), len(t))
    trim = int(0.5 * SR)
    a = ref[trim:n - trim]
    b = t[trim:n - trim]
    err = a - b

    # Global SNR
    snr = 10 * np.log10(np.sum(a ** 2) / (np.sum(err ** 2) + 1e-12))

    # Segmental SNR (20ms windows, skip silence — tracks perceived quality)
    win = int(0.02 * SR)
    seg_snrs = []
    for i in range(0, len(a) - win, win):
        s = np.sum(a[i:i + win] ** 2)
        e = np.sum(err[i:i + win] ** 2)
        if s < 1e-7:
            continue
        seg_snrs.append(np.clip(10 * np.log10(s / (e + 1e-12)), -10, 50))
    seg_snr = float(np.mean(seg_snrs)) if seg_snrs else float('nan')

    # Spectral shape of decoded signal (rolloff, centroid, HF energy)
    try:
        import scipy.signal as sig
        f, _, X = sig.stft(b, SR, nperseg=2048)
        psd = (np.abs(X) ** 2).mean(axis=1) + 1e-15
        centroid = float(np.sum(f * psd) / np.sum(psd))
        hf_pct = float(100 * psd[f >= 10000].sum() / psd.sum())
        rolloff = float(f[np.searchsorted(np.cumsum(psd) / psd.sum(), 0.95)])
    except ImportError:
        centroid, hf_pct, rolloff = float('nan'), float('nan'), float('nan')

    os.unlink(dec_path)
    return {
        "snr": snr,
        "seg_snr": seg_snr,
        "rolloff": rolloff,
        "centroid": centroid,
        "hf_pct": hf_pct,
        "decode_errors": len(errors),
    }


def load_wav_mono(path):
    """Load WAV as float64 mono (average channels)."""
    with wave.open(path, "r") as w:
        nch = w.getnchannels()
        data = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)
        if nch == 2:
            data = data.reshape(-1, 2).mean(axis=1)
    return data


def find_delay(ref, test, search=4000):
    """Find best alignment delay."""
    win = min(200000, len(ref) // 2, len(test) // 2)
    off = min(len(ref), len(test)) // 4
    best, bd = -1e9, 0
    for d in range(0, min(search, len(test) - off - win)):
        rseg = ref[off:off + win]
        tseg = test[off + d:off + d + win]
        if len(tseg) < win:
            continue
        c = float(np.dot(rseg, tseg))
        if c > best:
            best, bd = c, d
    return bd


def statistics(values):
    """Return dict of basic statistics."""
    a = np.array(values)
    return {
        "mean": float(np.mean(a)),
        "std": float(np.std(a, ddof=1)) if len(a) > 1 else 0.0,
        "min": float(np.min(a)),
        "max": float(np.max(a)),
        "median": float(np.median(a)),
        "n": len(a),
    }


def mann_whitney_p(a, b):
    """Simple Mann-Whitney U test, two-sided. Returns U and approximate p-value."""
    from scipy.stats import mannwhitneyu
    try:
        u, p = mannwhitneyu(a, b, alternative="two-sided")
        return u, p
    except ImportError:
        # Fallback: no scipy, skip significance test
        return None, None
    except ValueError:
        return None, None


def print_comparison(label, stats_a, stats_b, vals_a, vals_b):
    """Print formatted comparison for one quality tier."""
    delta_pct = ((stats_b["mean"] - stats_a["mean"]) / stats_a["mean"] * 100
                 if stats_a["mean"] != 0 else 0)
    sign = "+" if delta_pct > 0 else ""

    print(f"\n  {label}:")
    print(f"    {'':12s} {'Mean':>10s} {'Std':>8s} {'Min':>8s} {'Max':>8s} {'Median':>8s}")
    print(f"    {'A':12s} {stats_a['mean']:10.3f} {stats_a['std']:8.3f} "
          f"{stats_a['min']:8.3f} {stats_a['max']:8.3f} {stats_a['median']:8.3f}")
    print(f"    {'B':12s} {stats_b['mean']:10.3f} {stats_b['std']:8.3f} "
          f"{stats_b['min']:8.3f} {stats_b['max']:8.3f} {stats_b['median']:8.3f}")
    print(f"    {'Delta':12s} {sign}{delta_pct:.2f}%")

    u, p = mann_whitney_p(vals_a, vals_b)
    if p is not None:
        sig = "***" if p < 0.001 else "**" if p < 0.01 else "*" if p < 0.05 else "n.s."
        print(f"    {'Significance':12s} p={p:.4f} ({sig})")
    else:
        print(f"    {'Significance':12s} (scipy not available)")


def main():
    parser = argparse.ArgumentParser(
        description="A/B benchmark harness for glint encoder",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)

    # Binary or ref specification
    parser.add_argument("--a", dest="bin_a", help="Path to encoder A binary")
    parser.add_argument("--b", dest="bin_b", help="Path to encoder B binary")
    parser.add_argument("--ref-a", help="Git ref for A (builds automatically)")
    parser.add_argument("--ref-b", help="Git ref for B (builds automatically)")
    parser.add_argument("--label-a", default="A", help="Label for encoder A")
    parser.add_argument("--label-b", default="B", help="Label for encoder B")

    # Benchmark params
    parser.add_argument("-n", "--runs", type=int, default=7,
                        help="Number of iterations per config (default: 7)")
    parser.add_argument("--warmup", type=int, default=2,
                        help="Warmup runs to discard (default: 2)")
    parser.add_argument("--core", default="3",
                        help="CPU core(s) to pin to via taskset (default: 3, e.g. '2,3')")
    parser.add_argument("--seconds", type=int, default=30,
                        help="Audio duration in seconds (default: 30)")
    parser.add_argument("--tiers", nargs="+", default=["speed", "normal", "best"],
                        choices=["speed", "normal", "best"],
                        help="Quality tiers to test (default: all)")
    parser.add_argument("--quality", action="store_true",
                        help="Also compare output quality (SNR)")
    parser.add_argument("--interleave", action="store_true", default=True,
                        help="Interleave A/B runs (default: true)")
    parser.add_argument("--no-interleave", dest="interleave", action="store_false",
                        help="Run all A first, then all B")
    parser.add_argument("--json", metavar="FILE",
                        help="Write raw results to JSON file")

    args = parser.parse_args()

    # Resolve binaries
    if not (args.bin_a or args.ref_a) or not (args.bin_b or args.ref_b):
        parser.error("Need both A and B: use --a/--b for binaries or --ref-a/--ref-b for git refs")

    tmpdir_obj = tempfile.TemporaryDirectory(prefix="glint_ab_")
    tmpdir = tmpdir_obj.name

    if args.ref_a:
        print(f"Building A from ref '{args.ref_a}'...", flush=True)
        args.bin_a = build_from_ref(args.ref_a, os.path.join(tmpdir, "build_a"))
        if args.label_a == "A":
            args.label_a = args.ref_a
    if args.ref_b:
        print(f"Building B from ref '{args.ref_b}'...", flush=True)
        args.bin_b = build_from_ref(args.ref_b, os.path.join(tmpdir, "build_b"))
        if args.label_b == "B":
            args.label_b = args.ref_b

    for label, path in [("A", args.bin_a), ("B", args.bin_b)]:
        if not os.path.isfile(path):
            print(f"ERROR: encoder {label} not found: {path}", file=sys.stderr)
            return 1

    total_runs = args.warmup + args.runs

    # Generate test WAV
    wav_path = os.path.join(tmpdir, "ab_test.wav")
    print(f"Generating {args.seconds}s stereo test WAV...", flush=True)
    generate_test_wav(wav_path, args.seconds)

    print(f"\n{'='*60}")
    print(f"A/B Benchmark: glint encoder")
    print(f"{'='*60}")
    print(f"  A: {args.label_a} ({args.bin_a})")
    print(f"  B: {args.label_b} ({args.bin_b})")
    print(f"  CPU core: {args.core} (via taskset)")
    print(f"  Runs: {args.runs} (+{args.warmup} warmup)")
    print(f"  Audio: {args.seconds}s stereo @ {SR} Hz")
    print(f"  Tiers: {', '.join(args.tiers)}")
    print(f"  Interleaved: {args.interleave}")
    print(f"{'='*60}", flush=True)

    # Collect results: {tier: {"a": [speeds], "b": [speeds]}}
    results = {tier: {"a_speed": [], "b_speed": [],
                      "a_elapsed": [], "b_elapsed": []} for tier in args.tiers}

    for tier in args.tiers:
        print(f"\n--- Tier: {tier} ---", flush=True)

        mp3_a = os.path.join(tmpdir, f"{tier}_a.mp3")
        mp3_b = os.path.join(tmpdir, f"{tier}_b.mp3")

        for i in range(total_runs):
            is_warmup = i < args.warmup
            label = f"  warmup {i+1}/{args.warmup}" if is_warmup else f"  run {i-args.warmup+1}/{args.runs}"

            if args.interleave:
                # A then B each iteration
                sa, _, ea = run_encode(args.bin_a, wav_path, mp3_a, tier, args.core)
                sb, _, eb = run_encode(args.bin_b, wav_path, mp3_b, tier, args.core)
            else:
                sa, _, ea = run_encode(args.bin_a, wav_path, mp3_a, tier, args.core)
                sb, sb_audio, eb = 0, 0, 0  # filled in second pass

            if not is_warmup:
                results[tier]["a_speed"].append(sa)
                results[tier]["a_elapsed"].append(ea)
                if args.interleave:
                    results[tier]["b_speed"].append(sb)
                    results[tier]["b_elapsed"].append(eb)

            status = "warmup" if is_warmup else "measured"
            print(f"{label}: A={sa:.1f}x ({ea:.3f}s) B={sb:.1f}x ({eb:.3f}s)  [{status}]",
                  flush=True)

        # Non-interleaved: run B separately
        if not args.interleave:
            print(f"  --- B runs ---", flush=True)
            for i in range(total_runs):
                is_warmup = i < args.warmup
                sb, _, eb = run_encode(args.bin_b, wav_path, mp3_b, tier, args.core)
                if not is_warmup:
                    results[tier]["b_speed"].append(sb)
                    results[tier]["b_elapsed"].append(eb)
                label = f"  warmup {i+1}" if is_warmup else f"  run {i-args.warmup+1}/{args.runs}"
                status = "warmup" if is_warmup else "measured"
                print(f"{label}: B={sb:.1f}x ({eb:.3f}s)  [{status}]", flush=True)

    # === Results ===
    print(f"\n{'='*60}")
    print(f"RESULTS (speed, x realtime — higher is better)")
    print(f"{'='*60}")
    print(f"  A = {args.label_a}")
    print(f"  B = {args.label_b}")

    for tier in args.tiers:
        r = results[tier]
        sa = statistics(r["a_speed"])
        sb = statistics(r["b_speed"])
        print_comparison(f"{tier} (speed x)", sa, sb, r["a_speed"], r["b_speed"])

    print(f"\n{'='*60}")
    print(f"RESULTS (elapsed seconds — lower is better)")
    print(f"{'='*60}")

    for tier in args.tiers:
        r = results[tier]
        sa = statistics(r["a_elapsed"])
        sb = statistics(r["b_elapsed"])
        print_comparison(f"{tier} (elapsed s)", sa, sb, r["a_elapsed"], r["b_elapsed"])

    # Quality comparison — all tiers, full metrics
    quality_results = {}
    if args.quality:
        print(f"\n{'='*60}")
        print(f"QUALITY COMPARISON (per tier, higher is better except errors)")
        print(f"{'='*60}")

        any_regression = False

        for tier in args.tiers:
            mp3_qa = os.path.join(tmpdir, f"quality_{tier}_a.mp3")
            mp3_qb = os.path.join(tmpdir, f"quality_{tier}_b.mp3")

            qa = measure_quality(args.bin_a, wav_path, mp3_qa, tier, args.core)
            qb = measure_quality(args.bin_b, wav_path, mp3_qb, tier, args.core)
            quality_results[tier] = {"a": qa, "b": qb}

            print(f"\n  --- {tier} ---")
            print(f"    {'':14s} {'SNR':>8s} {'segSNR':>8s} {'rolloff':>9s} "
                  f"{'centroid':>10s} {'HF%':>6s} {'errors':>7s}")
            print(f"    {'A':14s} {qa['snr']:8.2f} {qa['seg_snr']:8.2f} "
                  f"{qa['rolloff']:9.0f} {qa['centroid']:10.0f} "
                  f"{qa['hf_pct']:6.2f} {qa['decode_errors']:7d}")
            print(f"    {'B':14s} {qb['snr']:8.2f} {qb['seg_snr']:8.2f} "
                  f"{qb['rolloff']:9.0f} {qb['centroid']:10.0f} "
                  f"{qb['hf_pct']:6.2f} {qb['decode_errors']:7d}")

            # Regression flags
            flags = []
            d_snr = qb["snr"] - qa["snr"]
            d_seg = qb["seg_snr"] - qa["seg_snr"]
            d_roll = qb["rolloff"] - qa["rolloff"]
            d_cent = qb["centroid"] - qa["centroid"]

            if d_seg < -0.5:
                flags.append(f"segSNR {d_seg:+.2f} dB")
            if qa["rolloff"] > 0 and d_roll / qa["rolloff"] < -0.10:
                flags.append(f"rolloff {d_roll:+.0f} Hz ({d_roll/qa['rolloff']*100:+.0f}%)")
            if qa["centroid"] > 0 and d_cent / qa["centroid"] < -0.05:
                flags.append(f"centroid {d_cent:+.0f} Hz ({d_cent/qa['centroid']*100:+.0f}%)")
            if qb["decode_errors"] > qa["decode_errors"]:
                flags.append(f"+{qb['decode_errors'] - qa['decode_errors']} decode errors")

            if flags:
                any_regression = True
                print(f"    {'⚠ REGRESSION':14s} {', '.join(flags)}")
            else:
                delta_str = f"SNR {d_snr:+.2f}, segSNR {d_seg:+.2f}, rolloff {d_roll:+.0f}"
                print(f"    {'✓ OK':14s} {delta_str}")

        if any_regression:
            print(f"\n  ⚠ QUALITY REGRESSIONS DETECTED — review before merging")
        else:
            print(f"\n  ✓ All tiers quality-equivalent")

    # JSON output
    if args.json:
        out = {
            "config": {
                "a": {"label": args.label_a, "binary": args.bin_a},
                "b": {"label": args.label_b, "binary": args.bin_b},
                "core": args.core,
                "runs": args.runs,
                "warmup": args.warmup,
                "seconds": args.seconds,
                "interleaved": args.interleave,
            },
            "results": {}
        }
        for tier in args.tiers:
            r = results[tier]
            out["results"][tier] = {
                "a_speed": r["a_speed"],
                "b_speed": r["b_speed"],
                "a_elapsed": r["a_elapsed"],
                "b_elapsed": r["b_elapsed"],
                "a_speed_stats": statistics(r["a_speed"]),
                "b_speed_stats": statistics(r["b_speed"]),
            }
        if args.quality and quality_results:
            out["quality"] = {}
            for tier, qr in quality_results.items():
                out["quality"][tier] = {"a": qr["a"], "b": qr["b"]}
        with open(args.json, "w") as f:
            json.dump(out, f, indent=2)
        print(f"\nRaw results written to {args.json}")

    print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

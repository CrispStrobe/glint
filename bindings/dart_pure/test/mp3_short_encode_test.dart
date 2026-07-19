// Short-block (window-switching) ENCODE round-trip. The opt-in `shortBlocks`
// path emits start/short/stop granules on transients; this pins the two bugs
// that made it reconstruct at ~3 dB (both now fixed in mp3_short.dart):
//   1. the WS big-values table selection couldn't represent large coefficients
//      (ESC candidate list stopped at table 24, linbits 4) → truncated codes,
//   2. the WS quantizer had no anti-clip min-gain bound → the peak clipped to
//      8191.
// With both fixed, a forced valid long→start→short→stop→long sequence and a
// real transient both reconstruct at high SNR through our own pure-Dart decoder.

import 'dart:math' as math;
import 'dart:typed_data';

import 'package:glint_audio_pure/glint_audio_pure.dart';
import 'package:glint_audio_pure/src/mp3_short.dart';
import 'package:test/test.dart';

/// Best-lag, best-gain SNR (dB) of [dec] vs [ref] (codec delay ⇒ align).
double _snr(Float64List ref, Float64List dec) {
  final n = math.min(ref.length, dec.length);
  var best = -1e18, bestLag = 0;
  for (var lag = 0; lag < 1400; lag++) {
    if (n - lag < 20000) break;
    var c = 0.0;
    for (var i = 0; i < 20000; i++) {
      c += ref[i] * dec[i + lag];
    }
    if (c > best) {
      best = c;
      bestLag = lag;
    }
  }
  final m = n - bestLag;
  var dot = 0.0, dd = 0.0;
  for (var i = 0; i < m; i++) {
    dot += ref[i] * dec[i + bestLag];
    dd += dec[i + bestLag] * dec[i + bestLag];
  }
  final g = dd > 0 ? dot / dd : 1.0;
  var sig = 0.0, err = 0.0;
  for (var i = 0; i < m; i++) {
    final e = ref[i] - g * dec[i + bestLag];
    sig += ref[i] * ref[i];
    err += e * e;
  }
  return 10 * math.log(sig / (err + 1e-30)) / math.ln10;
}

/// De-interleave channel [c] of a decoded [p].
Float64List _chan(Mp3Pcm p, int c) {
  final nc = p.channels;
  final out = Float64List(p.samples.length ~/ nc);
  for (var i = 0; i < out.length; i++) {
    out[i] = p.samples[i * nc + c];
  }
  return out;
}

/// A stereo transient pair — L clicks at 300 Hz, R at 500 Hz (distinct).
(Float64List, Float64List) _stereoTransient(int n, int sr) {
  final l = Float64List(n), r = Float64List(n);
  for (var i = 0; i < n; i++) {
    final t = i / sr;
    final ph = t % 0.25;
    l[i] = 0.7 * math.exp(-40 * ph) * math.sin(2 * math.pi * 300 * t);
    r[i] = 0.6 * math.exp(-40 * ph) * math.sin(2 * math.pi * 500 * t);
  }
  return (l, r);
}

void main() {
  const sr = 44100;

  tearDown(() => Mp3BlockScheduler.debugForceSeq = null);

  test('forced long→start→short→stop→long sequence reconstructs (>40 dB)', () {
    const n = sr * 2;
    final tone = Float64List(n);
    for (var i = 0; i < n; i++) {
      tone[i] = 0.4 * math.sin(2 * math.pi * 440 * i / sr);
    }
    Mp3BlockScheduler.debugForceSeq = [0, 0, 1, 2, 3, 0];
    final mp3 = mp3EncodeMono(tone, shortBlocks: true);
    final snr = _snr(tone, mp3Decode(mp3).samples);
    expect(snr, greaterThan(40.0), reason: 'short-block emission SNR=$snr dB');
  });

  test('short blocks on a real transient beat long-only reconstruction', () {
    const n = sr * 2;
    final tr = Float64List(n);
    for (var i = 0; i < n; i++) {
      final t = i / sr;
      final ph = t % 0.25; // a click every 250 ms
      tr[i] = 0.7 * math.exp(-40 * ph) * math.sin(2 * math.pi * 300 * t);
    }
    final withShort =
        _snr(tr, mp3Decode(mp3EncodeMono(tr, shortBlocks: true)).samples);
    final longOnly = _snr(tr, mp3Decode(mp3EncodeMono(tr)).samples);
    expect(withShort, greaterThan(40.0), reason: 'short=$withShort dB');
    expect(
      withShort,
      greaterThanOrEqualTo(longOnly - 0.5),
      reason: 'short=$withShort should not lose to long=$longOnly',
    );
  });

  test('shortBlocks default OFF is byte-identical on steady tone', () {
    const n = sr;
    final x = Float64List(n);
    for (var i = 0; i < n; i++) {
      x[i] = 0.3 * math.sin(2 * math.pi * 440 * i / sr);
    }
    final a = mp3EncodeMono(x);
    final b = mp3EncodeMono(x, shortBlocks: true); // no attack
    expect(b, equals(a));
  });

  test('stereo short blocks reconstruct BOTH channels and beat long-only', () {
    const n = sr * 2;
    final (l, r) = _stereoTransient(n, sr);
    final short = mp3Decode(mp3EncodeStereo(l, r, shortBlocks: true));
    final long = mp3Decode(mp3EncodeStereo(l, r));
    final sl = _snr(l, _chan(short, 0)), sr_ = _snr(r, _chan(short, 1));
    final ll = _snr(l, _chan(long, 0)), lr = _snr(r, _chan(long, 1));
    expect(sl, greaterThan(40.0), reason: 'L short=$sl dB');
    expect(sr_, greaterThan(40.0), reason: 'R short=$sr_ dB');
    expect(sl, greaterThanOrEqualTo(ll - 0.5), reason: 'L $sl vs long $ll');
    expect(sr_, greaterThanOrEqualTo(lr - 0.5), reason: 'R $sr_ vs long $lr');
  });

  test('joint (M/S) short blocks reconstruct both channels', () {
    const n = sr * 2;
    final (l, r) = _stereoTransient(n, sr);
    final dec = mp3Decode(mp3EncodeJointStereo(l, r, shortBlocks: true));
    expect(_snr(l, _chan(dec, 0)), greaterThan(40.0));
    expect(_snr(r, _chan(dec, 1)), greaterThan(40.0));
  });

  test('stereo/joint: shortBlocks on a steady tone stays all-long = plain', () {
    // No transient ⇒ the scheduler picks all-long, so the generalized useShort
    // path must produce bytes identical to the untouched non-short path.
    const n = sr;
    final l = Float64List(n), r = Float64List(n);
    for (var i = 0; i < n; i++) {
      l[i] = 0.3 * math.sin(2 * math.pi * 440 * i / sr);
      r[i] = 0.3 * math.sin(2 * math.pi * 330 * i / sr);
    }
    expect(
      mp3EncodeStereo(l, r, shortBlocks: true),
      equals(mp3EncodeStereo(l, r)),
    );
    expect(
      mp3EncodeJointStereo(l, r, shortBlocks: true),
      equals(mp3EncodeJointStereo(l, r)),
    );
  });
}

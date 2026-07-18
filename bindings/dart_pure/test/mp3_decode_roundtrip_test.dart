// End-to-end MP3 decode regression: encode with the pure-Dart encoder, decode
// with ffmpeg (an independent, standards-correct decoder), assert the audio
// survives. This is the test that would have caught the frequency-inversion
// bug — a forward transform that self-reconstructs perfectly (35 dB in the MDCT
// domain) but whose odd subbands come out spectrally flipped through a standard
// decoder (sweep SNR was 1.8 dB before the fix, 78 dB after).
//
// ffmpeg-gated: skipped when ffmpeg isn't on PATH (e.g. minimal CI), so it never
// blocks the pure-Dart suite. The bench/ab_vs_glint.py harness is the fuller A/B.

import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:glint_audio_pure/src/mp3_encoder.dart';
import 'package:test/test.dart';

bool _hasFfmpeg() {
  try {
    return Process.runSync('ffmpeg', ['-version']).exitCode == 0;
  } catch (_) {
    return false;
  }
}

/// Best-lag, best-gain SNR of [dec] against [ref] (MP3 has an encoder/decoder
/// delay, so search a small lag window both directions).
double _snr(Float64List ref, Float64List dec) {
  final n = math.min(ref.length, dec.length);
  var best = -1e9;
  var bestLag = 0;
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

void main() {
  const sr = 44100;
  final ffmpeg = _hasFfmpeg();

  for (final tc in [
    // A frequency sweep crosses every subband incl. odd ones — the direct
    // regression for frequency inversion (was 1.8 dB when broken).
    (name: 'sweep-200-3000Hz', min: 30.0),
    // A tone parked in odd subband 1 (~700 Hz) — odd-band-specific.
    (name: 'tone-700Hz', min: 20.0),
  ]) {
    test(
      'ffmpeg decodes ${tc.name} cleanly (SNR ≥ ${tc.min} dB)',
      () {
        const n = sr * 2;
        final x = Float64List(n);
        for (var i = 0; i < n; i++) {
          final t = i / sr;
          x[i] = tc.name.startsWith('sweep')
              ? 0.5 * math.sin(2 * math.pi * (200 + 3000 * t / 2) * t)
              : 0.6 * math.sin(2 * math.pi * 700 * t);
        }
        final mp3 = mp3EncodeMono(x);
        expect(mp3.length, greaterThan(0));
        expect(mp3[0], 0xFF); // sync

        final dir = Directory.systemTemp.createTempSync('mp3rt');
        try {
          final mp3f = File('${dir.path}/a.mp3')..writeAsBytesSync(mp3);
          final pcmf = '${dir.path}/a.pcm';
          final r = Process.runSync('ffmpeg', [
            '-v', 'error', '-i', mp3f.path, //
            '-f', 's16le', '-ac', '1', '-ar', '$sr', pcmf,
          ]);
          expect(r.exitCode, 0, reason: 'ffmpeg decode failed: ${r.stderr}');
          final raw = File(pcmf).readAsBytesSync();
          final bd = ByteData.sublistView(raw);
          final dec = Float64List(raw.length ~/ 2);
          for (var i = 0; i < dec.length; i++) {
            dec[i] = bd.getInt16(i * 2, Endian.little) / 32768.0;
          }
          final snr = _snr(x, dec);
          expect(
            snr,
            greaterThan(tc.min),
            reason: '${tc.name} decoded at only ${snr.toStringAsFixed(1)} dB '
                '(frequency inversion regression?)',
          );
        } finally {
          dir.deleteSync(recursive: true);
        }
      },
      skip: ffmpeg ? false : 'ffmpeg not on PATH',
    );
  }

  test(
    'ffmpeg decodes stereo to two distinct channels',
    () {
      const n = sr * 2;
      final left = Float64List(n);
      final right = Float64List(n);
      for (var i = 0; i < n; i++) {
        final t = i / sr;
        left[i] = 0.5 * math.sin(2 * math.pi * 440 * t); // L: 440 Hz
        right[i] = 0.5 * math.sin(2 * math.pi * 880 * t); // R: 880 Hz
      }
      final mp3 = mp3EncodeStereo(left, right, bitrate: 192);
      final dir = Directory.systemTemp.createTempSync('mp3st');
      try {
        File('${dir.path}/s.mp3').writeAsBytesSync(mp3);
        final r = Process.runSync('ffmpeg', [
          '-v', 'error', '-i', '${dir.path}/s.mp3', //
          '-f', 's16le', '-ac', '2', '-ar', '$sr', '${dir.path}/s.pcm',
        ]);
        expect(r.exitCode, 0, reason: 'ffmpeg: ${r.stderr}');
        final raw = File('${dir.path}/s.pcm').readAsBytesSync();
        final bd = ByteData.sublistView(raw);
        // De-interleave and reconstruct each channel's dominant frequency by
        // correlating against the expected tone — L must favour 440, R 880.
        double corr(int ch, double f) {
          var re = 0.0, im = 0.0;
          final frames = raw.length ~/ 4;
          for (var i = 0; i < frames; i++) {
            final s = bd.getInt16((i * 2 + ch) * 2, Endian.little) / 32768.0;
            final t = i / sr;
            re += s * math.cos(2 * math.pi * f * t);
            im += s * math.sin(2 * math.pi * f * t);
          }
          return math.sqrt(re * re + im * im);
        }

        expect(corr(0, 440) > corr(0, 880), isTrue, reason: 'L favours 440 Hz');
        expect(corr(1, 880) > corr(1, 440), isTrue, reason: 'R favours 880 Hz');
      } finally {
        dir.deleteSync(recursive: true);
      }
    },
    skip: ffmpeg ? false : 'ffmpeg not on PATH',
  );
}

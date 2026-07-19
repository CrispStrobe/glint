// mp3Decode must be robust to MALFORMED input — it decodes arbitrary user files
// now (audio import) and is the published glint_audio_pure public API. The
// contract these tests lock: for ANY bytes, mp3Decode returns an Mp3Pcm (the
// decodable prefix, possibly empty) or throws a plain Exception — NEVER a
// RangeError / other Error, and never hangs. Valid streams still decode intact.

import 'dart:math' as math;
import 'dart:typed_data';

import 'package:glint_audio_pure/glint_audio_pure.dart';
import 'package:test/test.dart';

Float64List _tone(int n) => Float64List.fromList([
      for (var i = 0; i < n; i++) 0.4 * math.sin(2 * math.pi * 220 * i / 44100),
    ]);

/// Deterministic pseudo-random bytes for [seed], sprinkled with frame syncs so
/// many offsets pass the header check and reach the frame body.
Uint8List _adversarial(int seed, int n) {
  final b = Uint8List(n);
  var x = seed * 2654435761 + 1;
  for (var i = 0; i < n; i++) {
    x = (x * 1103515245 + 12345) & 0x7fffffff;
    b[i] = x & 0xff;
  }
  final stride = 40 + (seed % 200);
  for (var i = 0; i + 1 < n; i += stride) {
    b[i] = 0xFF;
    b[i + 1] = 0xFB;
  }
  return b;
}

void main() {
  test('a valid MP3 still decodes intact (no regression)', () {
    final mp3 = mp3EncodeMono(_tone(44100));
    final pcm = mp3Decode(mp3);
    expect(pcm.channels, 1);
    expect(pcm.sampleRate, 44100);
    expect(pcm.samples.length, greaterThan(40000));
  });

  test('the byte pattern that used to throw RangeError now decodes cleanly',
      () {
    // Was: RangeError from _readScalefactors on a valid-header/garbage-body frame.
    final r = Uint8List(40000);
    var x = 12345;
    for (var i = 0; i < r.length; i++) {
      x = (x * 1103515245 + 12345) & 0x7fffffff;
      r[i] = x & 0xff;
    }
    for (var i = 0; i < r.length; i += 137) {
      r[i] = 0xFF;
      if (i + 1 < r.length) r[i + 1] = 0xFB;
    }
    late Mp3Pcm pcm;
    expect(() => pcm = mp3Decode(r), returnsNormally);
    expect(pcm.samples.length, greaterThanOrEqualTo(0));
  });

  test('fuzz: no adversarial input throws an Error (only Exceptions ok)', () {
    for (var seed = 0; seed < 80; seed++) {
      final bytes = _adversarial(seed, 500 + (seed * 97) % 8000);
      try {
        final pcm = mp3Decode(bytes);
        expect(pcm.samples.length, greaterThanOrEqualTo(0));
      } on Exception {
        // a clean Exception (e.g. FormatException) is an acceptable outcome
      } catch (e) {
        fail('seed $seed threw a non-Exception: ${e.runtimeType}: $e');
      }
    }
  });

  test('a truncated valid MP3 decodes a prefix without throwing', () {
    final full = mp3EncodeMono(_tone(44100 * 2));
    // Cut at several points, including mid-frame.
    for (final frac in [0.1, 0.37, 0.5, 0.83, 0.99]) {
      final cut = Uint8List.sublistView(full, 0, (full.length * frac).floor());
      late Mp3Pcm pcm;
      expect(() => pcm = mp3Decode(cut), returnsNormally, reason: 'frac $frac');
      expect(pcm.samples.length, greaterThanOrEqualTo(0));
    }
  });

  test('empty and sub-header inputs return an empty result, not a throw', () {
    for (final b in [
      Uint8List(0),
      Uint8List.fromList([0xFF, 0xFB]),
    ]) {
      final pcm = mp3Decode(b);
      expect(pcm.samples, isEmpty);
    }
  });
}

// Pure-Dart MP3 round-trip: encode with mp3_encoder, decode with mp3_decoder,
// assert the audio survives — no external decoder needed, so CI covers the full
// codec. Complements the ffmpeg-gated tests (which prove our streams are
// STANDARD; these prove our decoder is CORRECT).

import 'dart:math' as math;
import 'dart:typed_data';

import 'package:glint_audio_pure/src/mp3_decoder.dart';
import 'package:glint_audio_pure/src/mp3_encoder.dart';
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

Float64List _tone(int n, double f, int sr, [double amp = 0.4]) {
  final x = Float64List(n);
  for (var i = 0; i < n; i++) {
    x[i] = amp * math.sin(2 * math.pi * f * i / sr);
  }
  return x;
}

Float64List _chan(Float64List interleaved, int ch, int nch) {
  final o = Float64List(interleaved.length ~/ nch);
  for (var i = 0; i < o.length; i++) {
    o[i] = interleaved[i * nch + ch];
  }
  return o;
}

void main() {
  const sr = 44100;

  test('mono encode → decode round-trips (SNR ≥ 40 dB)', () {
    final x = _tone(sr * 2, 440, sr);
    final pcm = mp3Decode(mp3EncodeMono(x));
    expect(pcm.channels, 1);
    expect(pcm.sampleRate, sr);
    expect(_snr(x, pcm.samples), greaterThan(40));
  });

  test('stereo encode → decode reconstructs both channels', () {
    final l = _tone(sr * 2, 440, sr);
    final r = _tone(sr * 2, 660, sr);
    final pcm = mp3Decode(mp3EncodeStereo(l, r, bitrate: 192));
    expect(pcm.channels, 2);
    expect(_snr(l, _chan(pcm.samples, 0, 2)), greaterThan(40));
    expect(_snr(r, _chan(pcm.samples, 1, 2)), greaterThan(40));
  });

  test('joint (M/S) stereo decodes back to L/R', () {
    final l = _tone(sr * 2, 440, sr);
    final r = _tone(sr * 2, 660, sr);
    final pcm = mp3Decode(mp3EncodeJointStereo(l, r, bitrate: 192));
    expect(pcm.channels, 2);
    expect(_snr(l, _chan(pcm.samples, 0, 2)), greaterThan(40));
    expect(_snr(r, _chan(pcm.samples, 1, 2)), greaterThan(40));
  });

  test('VBR (with Xing header) decodes', () {
    final x = _tone(sr * 2, 440, sr);
    final pcm = mp3Decode(mp3EncodeMonoVbr(x, quality: 2));
    expect(pcm.channels, 1);
    expect(_snr(x, pcm.samples), greaterThan(30));
  });

  test('bit reservoir reassembles across frames (broadband)', () {
    // Broadband content makes hard granules borrow from the reservoir, so a
    // correct main_data_begin walk is required to reconstruct it.
    final x = Float64List(sr * 2);
    var s = 0x2545F491;
    for (var i = 0; i < x.length; i++) {
      s = (s * 1664525 + 1013904223) & 0xFFFFFFFF;
      x[i] = 0.3 * ((s >> 9) / 4194304.0 - 1.0);
    }
    final pcm = mp3Decode(mp3EncodeMono(x, bitrate: 192));
    // White noise at 192k is lossy, but a correct decode still correlates.
    expect(_snr(x, pcm.samples), greaterThan(6));
  });
}

// Live test for the PLAN buckets A+B helpers: glintResample and
// glintDecodeAudio (whole-file decode). Run: dart example/buckets_ab.dart
import 'dart:math' as math;
import 'dart:typed_data';
import 'package:glint/glint.dart';

void main() {
  // 1. Resample: length scales by the ratio; passthrough is a copy.
  const n = 4410;
  final tone = Float32List(n);
  for (var i = 0; i < n; i++) {
    tone[i] = math.sin(2 * math.pi * 200 * i / 44100);
  }
  final up = glintResample(tone, 1, 44100, 48000);
  final expect = (n * 48000 / 44100).round();
  if ((up.length - expect).abs() > 2) {
    throw StateError('resample length ${up.length} != ~$expect');
  }
  final same = glintResample(tone, 1, 44100, 44100);
  if (same.length != n) throw StateError('passthrough length ${same.length}');

  // 2. Resample preserves amplitude (0.5 sine, downsample 2:1).
  const m = 8820;
  final half = Float32List(m);
  for (var i = 0; i < m; i++) {
    half[i] = 0.5 * math.sin(2 * math.pi * 300 * i / 44100);
  }
  final down = glintResample(half, 1, 44100, 22050);
  var peak = 0.0;
  for (var i = 100; i < down.length - 100; i++) {
    if (down[i].abs() > peak) peak = down[i].abs();
  }
  if (peak < 0.45 || peak > 0.55) throw StateError('amplitude peak $peak');

  // 3. Whole-file decode of glint's own MP3 and AAC output.
  for (final codec in ['mp3', 'aac']) {
    final bytes = BytesBuilder();
    var phase = 0;
    if (codec == 'mp3') {
      final enc = GlintEncoder(sampleRate: 44100, channels: 2, bitrate: 128);
      final spf = enc.samplesPerFrame;
      for (var f = 0; f < 50; f++) {
        final pcm = Int16List(spf * 2);
        for (var i = 0; i < spf; i++) {
          final s = (0.4 * math.sin(2 * math.pi * 440 * phase / 44100) * 20000)
              .toInt();
          phase++;
          pcm[i * 2] = s;
          pcm[i * 2 + 1] = s;
        }
        bytes.add(enc.encode(pcm));
      }
      bytes.add(enc.flush());
      enc.dispose();
    } else {
      final enc = GlintAacEncoder(sampleRate: 44100, channels: 2, bitrate: 128);
      final spf = enc.samplesPerFrame;
      for (var f = 0; f < 50; f++) {
        final pcm = Int16List(spf * 2);
        for (var i = 0; i < spf; i++) {
          final s = (0.4 * math.sin(2 * math.pi * 440 * phase / 44100) * 20000)
              .toInt();
          phase++;
          pcm[i * 2] = s;
          pcm[i * 2 + 1] = s;
        }
        bytes.add(enc.encode(pcm));
      }
      bytes.add(enc.flush());
      enc.dispose();
    }
    final dec = glintDecodeAudio(bytes.toBytes());
    if (dec.channels != 2) throw StateError('$codec channels ${dec.channels}');
    if (dec.pcm.length < 40000 * 2) {
      throw StateError('$codec too few samples ${dec.pcm.length}');
    }
    print('dart glintDecodeAudio $codec: ${dec.pcm.length ~/ dec.channels} '
        'frames/ch @ ${dec.sampleRate} Hz');
  }

  // 4. Opus: 48 kHz float sine -> glintEncodeOpus -> glintDecodeAudio.
  const on = 48000;
  final opcm = Float32List(on * 2);
  for (var i = 0; i < on; i++) {
    final v = 0.4 * math.sin(2 * math.pi * 440 * i / 48000);
    opcm[i * 2] = v;
    opcm[i * 2 + 1] = v;
  }
  final opus = glintEncodeOpus(opcm, 2, bitrate: 96000);
  if (opus.length < 1000 ||
      !(opus[0] == 0x4F && opus[1] == 0x67 && opus[2] == 0x67)) {
    throw StateError('opus output not an Ogg stream');
  }
  final od = glintDecodeAudio(opus);
  if (od.sampleRate != 48000 || od.channels != 2) {
    throw StateError('opus decode ${od.sampleRate}/${od.channels}');
  }
  if (od.pcm.length < 40000 * 2) {
    throw StateError('opus too few samples ${od.pcm.length}');
  }
  print('dart glintEncodeOpus: ${opus.length} B -> '
      '${od.pcm.length ~/ od.channels} frames/ch @ ${od.sampleRate} Hz');

  // 5. WAV I/O at multiple bit depths (round-trip amplitude survives).
  const wn = 4096;
  final wsrc = Float32List(wn);
  for (var i = 0; i < wn; i++) {
    wsrc[i] = 0.5 * math.sin(2 * math.pi * 300 * i / 44100);
  }
  for (final spec in [
    [8, 0, 0.02],
    [16, 0, 2e-4],
    [24, 0, 1e-5],
    [32, 0, 1e-6],
    [32, 1, 1e-6],
  ]) {
    final bits = spec[0] as int;
    final flt = (spec[1] as int) == 1;
    final tol = spec[2] as double;
    final wav = glintWriteWav(wsrc, 1, 44100, bits: bits, floatFmt: flt);
    final rd = glintReadWav(wav);
    if (rd.sampleRate != 44100 || rd.channels != 1) {
      throw StateError('wav $bits meta ${rd.sampleRate}/${rd.channels}');
    }
    var err = 0.0;
    for (var i = 0; i < wn; i++) {
      final e = (wsrc[i] - rd.pcm[i]).abs();
      if (e > err) err = e;
    }
    if (err > tol) throw StateError('wav ${bits}bit err $err > $tol');
  }
  print('dart glintReadWav/glintWriteWav: 8/16/24/32-int + float OK');

  print('OK');
}

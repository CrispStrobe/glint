// Encode a short tone to a valid .mp3 with the pure-Dart encoder.
//
//   dart run example/glint_audio_pure_example.dart out.mp3
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:glint_audio_pure/glint_audio_pure.dart';

void main(List<String> args) {
  const sampleRate = 44100;
  const seconds = 2;

  // A 440 Hz + 660 Hz mono tone.
  final pcm = Float64List(sampleRate * seconds);
  for (var i = 0; i < pcm.length; i++) {
    final t = i / sampleRate;
    pcm[i] = 0.4 * math.sin(2 * math.pi * 440 * t) +
        0.2 * math.sin(2 * math.pi * 660 * t);
  }

  final mp3 = mp3EncodeMono(pcm, sampleRate: sampleRate, bitrate: 128);

  final out = args.isNotEmpty ? args[0] : 'out.mp3';
  File(out).writeAsBytesSync(mp3);
  stdout.writeln('wrote $out (${mp3.length} bytes, '
      '${seconds}s @ 128 kbps mono) — plays in any MP3 decoder.');
}

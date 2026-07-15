import 'dart:math' as math;
import 'dart:typed_data';
import 'package:glint_audio/glint_audio.dart';

void main() {
  // MP3 encode -> decode
  final enc = GlintEncoder(sampleRate: 44100, channels: 2, bitrate: 128);
  final spf = enc.samplesPerFrame;
  final mp3 = BytesBuilder();
  var phase = 0;
  for (var f = 0; f < 40; f++) {
    final pcm = Int16List(spf * 2);
    for (var i = 0; i < spf; i++) {
      final s = 0.4 * math.sin(2 * math.pi * 440 * phase / 44100);
      phase++;
      pcm[i * 2] = (s * 20000).toInt();
      pcm[i * 2 + 1] = (s * 15000).toInt();
    }
    mp3.add(enc.encode(pcm));
  }
  mp3.add(enc.flush());
  enc.dispose();
  final dec = GlintMp3Decoder();
  final out = dec.decode(mp3.toBytes());
  print(
      'dart GlintMp3Decoder: ${mp3.length} B -> ${out.length ~/ 2} samples/ch');
  if (out.length < 40000 * 2) throw StateError('too few samples');
  dec.dispose();
  print('OK');
}

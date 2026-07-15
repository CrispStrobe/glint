import 'dart:math' as math;
import 'dart:typed_data';
import 'package:glint_audio/glint_audio.dart';

void main() {
  final enc = GlintOpusEncoder(channels: 2, bitrate: 96000);
  final dec = GlintOpusDecoder(channels: 2, sampleRate: 48000);
  var matches = 0;
  for (var f = 0; f < 5; f++) {
    final pcm = Float32List(960 * 2);
    for (var i = 0; i < 960; i++) {
      final t = (f * 960 + i) / 48000.0;
      pcm[i * 2] = 0.4 * math.sin(2 * math.pi * 440 * t);
      pcm[i * 2 + 1] = 0.3 * math.sin(2 * math.pi * 660 * t);
    }
    final pkt = enc.encode(pcm);
    final out = dec.decode(pkt);
    assert(out.length == 960 * 2);
    if (enc.finalRange() == dec.finalRange()) matches++;
  }
  final lost = dec.decodeLost(960);
  assert(lost.length == 960 * 2);
  print('dart opus binding: $matches/5 range matches, PLC ok');
  enc.dispose();
  dec.dispose();
}

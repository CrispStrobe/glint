import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:glint_audio/glint_audio.dart';

void main() {
  const sampleRate = 44100;
  const duration = 1; // seconds
  const frequency = 440.0;
  const numSamples = sampleRate * duration;

  // Generate a 440 Hz sine wave
  final pcm = Int16List(numSamples);
  for (var i = 0; i < numSamples; i++) {
    pcm[i] = (sin(2.0 * pi * frequency * i / sampleRate) * 30000).toInt();
  }

  // Encode to MP3
  final encoder =
      GlintEncoder(sampleRate: sampleRate, channels: 1, bitrate: 128);
  final mp3 = BytesBuilder();

  final frameSize = encoder.samplesPerFrame;
  for (var offset = 0; offset < numSamples; offset += frameSize) {
    final end = (offset + frameSize).clamp(0, numSamples);
    final chunk = Int16List.sublistView(pcm, offset, end);
    mp3.add(encoder.encode(chunk));
  }
  mp3.add(encoder.flush());
  encoder.dispose();

  // Write to file
  final output = File('output.mp3');
  output.writeAsBytesSync(mp3.toBytes());
  print('Wrote ${mp3.length} bytes to ${output.path}');
}

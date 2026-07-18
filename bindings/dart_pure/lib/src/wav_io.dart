// lib/core/audio/wav_io.dart
//
// A minimal WAV *reader* — the counterpart to synth.dart's `wavBytes` writer.
// Enough to load real recordings (mono/stereo PCM16) for the CLI and tests to
// stream through the detectors. Not a general codec: it handles uncompressed
// PCM16 only (what the mic path and `sox`/`ffmpeg -f s16le` produce).

import 'dart:typed_data';

class WavData {
  const WavData({
    required this.samples,
    required this.sampleRate,
    required this.channels,
  });

  /// Interleaved PCM16 samples (all channels).
  final Int16List samples;
  final int sampleRate;
  final int channels;
}

/// Parse a PCM16 WAV. Throws [FormatException] on anything it can't read.
WavData readWavPcm16(Uint8List bytes) {
  final data = ByteData.sublistView(bytes);
  String tag(int o) => String.fromCharCodes(bytes.sublist(o, o + 4));

  if (bytes.length < 44 || tag(0) != 'RIFF' || tag(8) != 'WAVE') {
    throw const FormatException('Not a RIFF/WAVE file');
  }

  int sampleRate = 0;
  int channels = 0;
  int bitsPerSample = 0;
  int audioFormat = 0;
  int dataOffset = -1;
  int dataLength = 0;

  // Walk the chunks after the 12-byte RIFF/WAVE header.
  var p = 12;
  while (p + 8 <= bytes.length) {
    final id = tag(p);
    final size = data.getUint32(p + 4, Endian.little);
    final body = p + 8;
    // The loop only guarantees the 8-byte chunk header fits (body <= length);
    // the fmt chunk reads 16 more bytes (up to body + 16). A `fmt ` id within
    // 16 bytes of EOF (a truncated / crafted WAV) would otherwise read past the
    // buffer and throw a RangeError — but this reader's contract is to throw
    // FormatException on anything it can't read. Skip an under-length fmt chunk;
    // audioFormat then stays 0 and the PCM16 check below rejects it cleanly.
    if (id == 'fmt ' && body + 16 <= bytes.length) {
      audioFormat = data.getUint16(body, Endian.little);
      channels = data.getUint16(body + 2, Endian.little);
      sampleRate = data.getUint32(body + 4, Endian.little);
      bitsPerSample = data.getUint16(body + 14, Endian.little);
    } else if (id == 'data') {
      dataOffset = body;
      dataLength = size;
    }
    p = body + size + (size & 1); // chunks are word-aligned
  }

  if (audioFormat != 1 || bitsPerSample != 16) {
    throw FormatException(
      'Only uncompressed PCM16 is supported '
      '(format=$audioFormat, bits=$bitsPerSample)',
    );
  }
  if (dataOffset < 0) throw const FormatException('No data chunk');

  final available = bytes.length - dataOffset;
  final n = (dataLength.clamp(0, available)) ~/ 2;
  final out = Int16List(n);
  for (var i = 0; i < n; i++) {
    out[i] = data.getInt16(dataOffset + i * 2, Endian.little);
  }
  return WavData(samples: out, sampleRate: sampleRate, channels: channels);
}

/// Downmix interleaved samples to a mono float list in [-1, 1].
Float64List wavToMonoFloat(WavData wav) {
  final ch = wav.channels < 1 ? 1 : wav.channels;
  final frames = wav.samples.length ~/ ch;
  final out = Float64List(frames);
  for (var f = 0; f < frames; f++) {
    var sum = 0.0;
    for (var c = 0; c < ch; c++) {
      sum += wav.samples[f * ch + c];
    }
    out[f] = (sum / ch) / 32768.0;
  }
  return out;
}

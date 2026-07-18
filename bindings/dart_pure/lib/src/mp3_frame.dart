// lib/core/audio/mp3/mp3_frame.dart
//
// MP3 (MPEG-1 Layer III) frame header + framing math — slice 1 of the pure-Dart
// MP3 encoder port. Tables + header layout ported from glint (MIT, clean-room)
// and ISO 11172-3. Later slices add subband/MDCT/quantize/Huffman to fill the
// frame's main data; this file defines the container they pack into.

import 'dart:typed_data';

import 'mp3_bitstream.dart';

/// MPEG-1 Layer III bitrates (kbps), header index = position + 1 (1..14).
const List<int> kMp3Bitrates = [
  32,
  40,
  48,
  56,
  64,
  80,
  96,
  112,
  128,
  160,
  192,
  224,
  256,
  320,
];

/// MPEG-1 sample rates → 2-bit header index (0=44100, 1=48000, 2=32000).
const List<int> kMp3SampleRates = [44100, 48000, 32000];

/// 1152 PCM samples per channel per MPEG-1 Layer III frame (2 granules × 576).
const int kMp3SamplesPerFrame = 1152;

/// Channel mode field (2 bits).
enum Mp3ChannelMode { stereo, jointStereo, dualChannel, mono }

/// Header index (1..14) for [bitrateKbps], or 0 if not a valid MPEG-1 rate.
int mp3BitrateIndex(int bitrateKbps) {
  final i = kMp3Bitrates.indexOf(bitrateKbps);
  return i < 0 ? 0 : i + 1;
}

/// 2-bit header index for [sampleRate], or -1 if unsupported.
int mp3SampleRateIndex(int sampleRate) => kMp3SampleRates.indexOf(sampleRate);

/// Bytes in one MPEG-1 Layer III frame at [bitrateKbps]/[sampleRate]
/// (+1 when [padding]). `144 * bitrate_bps / sample_rate` (+pad).
int mp3FrameSize(int bitrateKbps, int sampleRate, {bool padding = false}) =>
    (144 * (bitrateKbps * 1000) ~/ sampleRate) + (padding ? 1 : 0);

/// The 32-bit MP3 frame header (4 bytes) for MPEG-1 Layer III, no CRC.
/// Throws [ArgumentError] on an invalid bitrate/sample-rate.
Uint8List mp3FrameHeader({
  required int bitrateKbps,
  required int sampleRate,
  bool padding = false,
  Mp3ChannelMode channelMode = Mp3ChannelMode.stereo,
  int modeExtension = 0,
}) {
  final brIndex = mp3BitrateIndex(bitrateKbps);
  final srIndex = mp3SampleRateIndex(sampleRate);
  if (brIndex == 0) {
    throw ArgumentError('unsupported MP3 bitrate: $bitrateKbps kbps');
  }
  if (srIndex < 0) {
    throw ArgumentError('unsupported MP3 sample rate: $sampleRate Hz');
  }
  final w = Mp3BitWriter()
    ..writeBits(0x7FF, 11) // sync
    ..writeBits(0x3, 2) // MPEG-1
    ..writeBits(0x1, 2) // Layer III
    ..writeBits(1, 1) // protection: 1 = no CRC
    ..writeBits(brIndex, 4)
    ..writeBits(srIndex, 2)
    ..writeBits(padding ? 1 : 0, 1)
    ..writeBits(0, 1) // private
    ..writeBits(channelMode.index, 2)
    ..writeBits(modeExtension, 2)
    ..writeBits(0, 1) // copyright
    ..writeBits(1, 1) // original
    ..writeBits(0, 2); // emphasis: none
  return w.takeBytes();
}

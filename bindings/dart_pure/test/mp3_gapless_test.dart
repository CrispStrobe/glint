// MP3 gapless trimming: an MP3 carrying a LAME tag must decode sample-aligned
// with the PCM that was encoded, instead of ~23 ms late.
//
// Why this exists: every MPEG-1 Layer III decoder adds a fixed 529-sample
// latency, and LAME prepends its own encoder delay on top. `mp3Decode` skipped
// the Xing/Info frame as metadata but never read its gapless fields, so an MP3
// sample loaded through the SFZ path started 1106 frames (23.0 ms @48k) late and
// ran 1152 frames long. Uniform, so it is inaudible on its own — but it flams
// against WAV/SF2 voices and smears a percussive attack. Measured on a real
// bell sample: WAV onset 35 / MP3 onset 1141 before the fix; 35 / 36 after.

import 'dart:math' as math;
import 'dart:typed_data';

import 'package:glint_audio_pure/src/mp3_decoder.dart';
import 'package:glint_audio_pure/src/mp3_encoder.dart';
import 'package:test/test.dart';

/// A valid MPEG-1 Layer III 128 kbps 44.1 kHz stereo frame carrying an
/// Info/Xing tag whose LAME extension declares [delay] and [padding].
Uint8List _xingFrame({required int delay, required int padding}) {
  const frameBytes = 417; // 144 * 128000 / 44100, no padding bit
  final f = Uint8List(frameBytes);
  f[0] = 0xFF;
  f[1] = 0xFB; // MPEG-1, Layer III, no CRC
  f[2] = 0x90; // bitrate idx 9 (128k), sr idx 0 (44.1k), no pad
  f[3] = 0x00; // stereo
  const tagOff = 4 + 32; // stereo side info is 32 bytes
  f.setRange(tagOff, tagOff + 4, 'Info'.codeUnits);
  // flags = 0 -> no frames/bytes/TOC/quality fields follow
  const p = tagOff + 4 + 4;
  f.setRange(p, p + 9, 'LAME3.100'.codeUnits);
  // The delays live 21 bytes into the LAME extension: 12 bits each.
  const lame = p + 21;
  f[lame] = (delay >> 4) & 0xFF;
  f[lame + 1] = ((delay & 0x0F) << 4) | ((padding >> 8) & 0x0F);
  f[lame + 2] = padding & 0xFF;
  return f;
}

void main() {
  // Encode a real signal so the stream under test is genuine MP3 audio.
  final pcm = Float64List(44100);
  for (var i = 0; i < pcm.length; i++) {
    pcm[i] = 0.4 * math.sin(2 * math.pi * 440 * i / 44100);
  }
  final bare = mp3EncodeMono(pcm);

  test('a tagless MP3 is returned untrimmed (old behaviour preserved)', () {
    final n = mp3Decode(bare).samples.length;
    expect(n, greaterThan(0));
    // Decoding is deterministic and nothing is dropped without a tag.
    expect(mp3Decode(bare).samples.length, n);
  });

  test('a LAME tag trims encoder delay + 529 from the head', () {
    const delay = 576, padding = 0;
    final tagged = Uint8List.fromList([
      ..._xingFrame(delay: delay, padding: padding),
      ...bare,
    ]);
    final plain = mp3Decode(bare);
    final trimmed = mp3Decode(tagged);
    expect(trimmed.channels, plain.channels);
    expect(
      plain.samples.length - trimmed.samples.length,
      (delay + 529) * plain.channels,
      reason:
          'head trim must be exactly encoder delay + the 529-sample decoder '
          'latency',
    );
  });

  test('end padding is trimmed too (padding beyond the 529 latency)', () {
    const delay = 576, padding = 1000;
    final tagged = Uint8List.fromList([
      ..._xingFrame(delay: delay, padding: padding),
      ...bare,
    ]);
    final plain = mp3Decode(bare);
    final trimmed = mp3Decode(tagged);
    expect(
      plain.samples.length - trimmed.samples.length,
      ((delay + 529) + (padding - 529)) * plain.channels,
    );
  });

  test('an all-zero LAME tag is treated as absent, not as "no delay"', () {
    final tagged = Uint8List.fromList([
      ..._xingFrame(delay: 0, padding: 0),
      ...bare,
    ]);
    expect(mp3Decode(tagged).samples.length, mp3Decode(bare).samples.length);
  });

  test('a hostile tag cannot produce a negative-length result', () {
    // Delay far larger than the stream: must yield empty, never throw.
    final tagged = Uint8List.fromList([
      ..._xingFrame(delay: 4095, padding: 4095),
      ...bare,
    ]);
    var n = -1;
    expect(() => n = mp3Decode(tagged).samples.length, returnsNormally);
    expect(n, greaterThanOrEqualTo(0));
  });
}

// Top-level MP3 encoder — the frame stream must be valid: every frame syncs,
// header fields are right, and the framing math (bitrate/samplerate/padding)
// lines up. Decodability is additionally checked out-of-band with ffmpeg
// (see bench/README.md); this test validates the bitstream structure in CI.

import 'dart:math' as math;
import 'dart:typed_data';

import 'package:glint_audio_pure/src/mp3_encoder.dart';
import 'package:glint_audio_pure/src/mp3_frame.dart';
import 'package:test/test.dart';

Float64List _sine(int samples, double freq, int sr) => Float64List.fromList(
      List.generate(
        samples,
        (i) => 0.6 * math.sin(2 * math.pi * freq * i / sr),
      ),
    );

void main() {
  test('produces a well-framed MPEG-1 Layer III stream', () {
    const sr = 44100, br = 128;
    final pcm = _sine(sr, 440, sr); // 1 second
    final mp3 = mp3EncodeMono(pcm);

    expect(mp3.length, greaterThan(0));
    // Walk every frame by its header-derived size; each must start on a sync.
    var off = 0;
    var frames = 0;
    while (off + 4 <= mp3.length) {
      // Sync: 11 bits all 1 → byte0 == FF, top 3 bits of byte1 set.
      expect(mp3[off], 0xFF, reason: 'frame $frames sync at $off');
      expect(mp3[off + 1] & 0xE0, 0xE0, reason: 'frame $frames sync2');
      // MPEG-1 (11), Layer III (01) → byte1 low bits 1_1011_x → (b1 & 0x1E)==0x1A.
      expect(mp3[off + 1] & 0x1E, 0x1A);
      final brIdx = (mp3[off + 2] >> 4) & 0xF;
      final srIdx = (mp3[off + 2] >> 2) & 0x3;
      final pad = (mp3[off + 2] >> 1) & 0x1;
      expect(brIdx, mp3BitrateIndex(br));
      expect(srIdx, mp3SampleRateIndex(sr));
      final size = mp3FrameSize(br, sr, padding: pad == 1);
      off += size;
      frames++;
    }
    // 1 s @ 44100 / 1152 per frame ≈ 38 frames.
    expect(frames, inInclusiveRange(36, 40));
    expect(off, mp3.length, reason: 'frames tile the stream exactly');
  });

  test('length tracks the CBR bitrate', () {
    const sr = 44100;
    final pcm = _sine(2 * sr, 220, sr); // 2 seconds
    final mp3 = mp3EncodeMono(pcm);
    // ~128 kbit/s * 2 s / 8 = ~32000 bytes (± one frame).
    expect(mp3.length, closeTo(32000, 1000));
  });

  test('silence encodes to a valid (tiny-per-frame) stream', () {
    final mp3 = mp3EncodeMono(Float64List(44100));
    expect(mp3[0], 0xFF);
    expect(mp3.length, greaterThan(0));
  });

  test('rejects bad params', () {
    expect(
      () => mp3EncodeMono(Float64List(1152), sampleRate: 44101),
      throwsArgumentError,
    );
    expect(
      () => mp3EncodeMono(Float64List(1152), bitrate: 130),
      throwsArgumentError,
    );
  });

  test('stereo: well-framed stream with the stereo channel-mode flag', () {
    const sr = 44100, br = 192;
    final left = _sine(sr, 440, sr);
    final right = _sine(sr, 554, sr);
    final mp3 = mp3EncodeStereo(left, right, bitrate: br);
    expect(mp3.length, greaterThan(0));

    var off = 0, frames = 0;
    while (off + 4 <= mp3.length) {
      expect(mp3[off], 0xFF, reason: 'frame $frames sync');
      expect(mp3[off + 1] & 0xE0, 0xE0);
      // Channel mode is bits 7..6 of byte 3; stereo = 00.
      expect(
        (mp3[off + 3] >> 6) & 0x3,
        0,
        reason: 'frame $frames channel mode',
      );
      off += mp3FrameSize(br, sr, padding: (mp3[off + 2] >> 1) & 0x1 == 1);
      frames++;
    }
    expect(off, mp3.length, reason: 'frames tile the stream exactly');
    expect(frames, inInclusiveRange(36, 40));
  });

  test('VBR: variable-size frames that all sync + tile, quality→size', () {
    const sr = 44100;
    final busy = _sine(2 * sr, 440, sr); // 2 s of content
    final best = mp3EncodeMonoVbr(busy, quality: 0);
    final small = mp3EncodeMonoVbr(busy, quality: 9);

    // Better quality => more bytes.
    expect(best.length, greaterThan(small.length));

    // Walk VBR frames by each header's OWN bitrate; every frame must sync/tile.
    var off = 0, frames = 0;
    while (off + 4 <= best.length) {
      expect(best[off], 0xFF, reason: 'frame $frames sync');
      expect(best[off + 1] & 0xE0, 0xE0);
      final brIdx = (best[off + 2] >> 4) & 0xF;
      final srIdx = (best[off + 2] >> 2) & 0x3;
      expect(srIdx, mp3SampleRateIndex(sr));
      final kbps = kMp3Bitrates[brIdx - 1]; // brIdx 1..14 → index 0..13
      final pad = (best[off + 2] >> 1) & 0x1;
      off += mp3FrameSize(kbps, sr, padding: pad == 1);
      frames++;
    }
    expect(off, best.length, reason: 'VBR frames tile exactly');
  });
}

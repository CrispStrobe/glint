// MP3 granule Huffman encoding — slice 5b. Verifies region partitioning +
// the emit (self-consistent bit count, determinism, table chooser).

import 'package:glint_audio_pure/src/mp3_bitstream.dart';
import 'package:glint_audio_pure/src/mp3_granule.dart';
import 'package:test/test.dart';

void main() {
  test('choose_huff_table thresholds (glint)', () {
    expect(mp3ChooseTable(0), 0);
    expect(mp3ChooseTable(1), 1);
    expect(mp3ChooseTable(2), 3);
    expect(mp3ChooseTable(15), 13);
    expect(mp3ChooseTable(16), greaterThanOrEqualTo(16)); // ESC
    expect(mp3ChooseTable(2000), greaterThanOrEqualTo(16));
  });

  test('all-zero granule -> no regions, no bits', () {
    final ix = List.filled(576, 0);
    final r = mp3ComputeRegions(ix, 0);
    expect(r.bigValues, 0);
    expect(r.count1, 0);
    final w = Mp3BitWriter();
    mp3EncodeGranule(w, ix, r, 0);
    expect(w.bitCount, 0);
  });

  test('regions partition exactly the coded lines', () {
    final ix = List.filled(576, 0);
    for (var i = 0; i < 40; i++) {
      ix[i] = (i % 9) - 4; // mix of big + small values
    }
    ix[40] = 1;
    ix[41] = -1;
    ix[42] = 0;
    ix[43] = 1; // a count1 quad
    final r = mp3ComputeRegions(ix, 0);
    // rzero = 44 (last nonzero at 43). big_values pairs + count1 quads cover it.
    expect(r.bigValues * 2 + r.count1 * 4, 44);
    expect(r.count1, greaterThanOrEqualTo(1)); // the trailing {1,-1,0,1} quad
    expect(r.region0Count, inInclusiveRange(0, 15));
    expect(r.region1Count, inInclusiveRange(0, 7));
  });

  test('a pure count1 granule (all |v|<=1) has no big_values', () {
    final ix = List.filled(576, 0);
    for (var i = 0; i < 16; i++) {
      ix[i] = (i.isEven) ? 1 : -1;
    }
    final r = mp3ComputeRegions(ix, 0);
    expect(r.bigValues, 0);
    expect(r.count1, 4); // 16 lines / 4
  });

  test('emit is deterministic and non-empty for a real granule', () {
    final ix = List.filled(576, 0);
    for (var i = 0; i < 60; i++) {
      ix[i] = ((i * 37) % 23) - 11;
    }
    final r = mp3ComputeRegions(ix, 0);
    Mp3BitWriter enc() {
      final w = Mp3BitWriter();
      mp3EncodeGranule(w, ix, r, 0);
      return w;
    }

    final a = enc().takeBytes();
    final b = enc().takeBytes();
    expect(a, b); // deterministic
    expect(a.length, greaterThan(0));
  });
}

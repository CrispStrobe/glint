// MP3 Huffman pair coder — slice 5a. Verified against glint's ISO tables:
// known code lengths, encode==pairBits consistency, ESC linbits, table ranges.

import 'package:glint_audio_pure/src/mp3_bitstream.dart';
import 'package:glint_audio_pure/src/mp3_huffman.dart';
import 'package:test/test.dart';

void main() {
  test('table 1 pair bit counts (ISO ht1_len {1,3,2,3} + sign bits)', () {
    expect(mp3PairBits(1, 0, 0), 1); // len[0]=1, no signs
    expect(mp3PairBits(1, 0, 1), 3 + 1); // len[1]=3 + y sign
    expect(mp3PairBits(1, -1, 0), 2 + 1); // len[2]=2 + x sign
    expect(mp3PairBits(1, 1, 1), 3 + 1 + 1); // len[3]=3 + both signs
  });

  test('encodePair emits exactly pairBits bits', () {
    final w = Mp3BitWriter();
    const pairs = [
      [1, 0, 0],
      [1, 1, 1],
      [2, 2, 1],
      [13, 5, 7],
      [16, 20, 3], // ESC table, linbits
      [24, 300, 2], // ESC table, large value
    ];
    var expected = 0;
    for (final p in pairs) {
      mp3EncodePair(w, p[0], p[1], p[2]);
      expected += mp3PairBits(p[0], p[1], p[2]);
    }
    expect(w.bitCount, expected);
  });

  test('ESC linbits add bits for large values', () {
    expect(mp3PairBits(16, 20, 0), greaterThan(mp3PairBits(16, 3, 0)));
    // table 24 has more linbits than 16, so a huge value costs more there.
    expect(mp3PairBits(24, 1000, 0), greaterThan(mp3PairBits(16, 1000, 0)));
  });

  test('table 0 emits nothing', () {
    final w = Mp3BitWriter();
    mp3EncodePair(w, 0, 5, 5);
    expect(w.bitCount, 0);
    expect(mp3PairBits(0, 5, 5), 0);
  });

  test('ESC table ranges map to the right linbits', () {
    expect(getHuffTable(16).linbits, 1);
    expect(getHuffTable(23).linbits, 13); // ISO linbits are non-linear
    expect(getHuffTable(24).linbits, 4);
    expect(getHuffTable(31).linbits, 13);
    expect(getHuffTable(16).xlen, 16);
  });

  test('a single small pair round-trips through the writer bytes', () {
    final w = Mp3BitWriter();
    mp3EncodePair(w, 1, 0, 0); // code 0x0001, len 1 -> a single 1 bit
    expect(w.takeBytes(), [0x80]); // 1 then 7 pad zeros
  });
}

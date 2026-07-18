// lib/core/audio/mp3/mp3_huffman.dart
//
// MP3 Huffman pair coder — slice 5a of the pure-Dart MP3 encoder port. Ported
// from glint's `get_huff_table` + `encode_pair` (MIT, clean-room). Encodes a
// value pair (x, y) with one of the ISO 11172-3 code tables: the Huffman code
// for (|x|,|y|) capped at 15, then linbits for the ESC tables (16-31), then a
// sign bit per non-zero value. Pure Dart => identical native + web.

import 'mp3_bitstream.dart';
import 'mp3_huffman_tables.dart';

/// One MPEG-1 Layer III Huffman table: code lengths + codes (indexed
/// `x*xlen + y`), the ESC [linbits], and the pair dimension [xlen].
class HuffTable {
  const HuffTable(this.id, this.linbits, this.xlen, this.len, this.code);
  final int id;
  final int linbits;
  final int xlen;
  final List<int> len;
  final List<int> code; // empty for table 0

  static const _empty = <int>[];
}

/// ISO 11172-3 ESC linbits (non-linear) for tables 16-23 and 24-31.
const List<int> _kLinbits16 = [1, 2, 3, 4, 6, 8, 10, 13];
const List<int> _kLinbits24 = [4, 5, 6, 7, 8, 9, 11, 13];

/// The 33 tables (0..33; 4 and 14 don't exist → table 0). Tables 16-23 share
/// kHT16* with [_kLinbits16]; 24-31 share kHT24* with [_kLinbits24].
HuffTable getHuffTable(int id) {
  switch (id) {
    case 1:
      return const HuffTable(1, 0, 2, kHT1Len, kHT1Code);
    case 2:
      return const HuffTable(2, 0, 3, kHT2Len, kHT2Code);
    case 3:
      return const HuffTable(3, 0, 3, kHT3Len, kHT3Code);
    case 5:
      return const HuffTable(5, 0, 4, kHT5Len, kHT5Code);
    case 6:
      return const HuffTable(6, 0, 4, kHT6Len, kHT6Code);
    case 7:
      return const HuffTable(7, 0, 6, kHT7Len, kHT7Code);
    case 8:
      return const HuffTable(8, 0, 6, kHT8Len, kHT8Code);
    case 9:
      return const HuffTable(9, 0, 6, kHT9Len, kHT9Code);
    case 10:
      return const HuffTable(10, 0, 8, kHT10Len, kHT10Code);
    case 11:
      return const HuffTable(11, 0, 8, kHT11Len, kHT11Code);
    case 12:
      return const HuffTable(12, 0, 8, kHT12Len, kHT12Code);
    case 13:
      return const HuffTable(13, 0, 16, kHT13Len, kHT13Code);
    case 15:
      return const HuffTable(15, 0, 16, kHT15Len, kHT15Code);
    case >= 16 && <= 23:
      return HuffTable(id, _kLinbits16[id - 16], 16, kHT16Len, kHT16Code);
    case >= 24 && <= 31:
      return HuffTable(id, _kLinbits24[id - 24], 16, kHT24Len, kHT24Code);
    default:
      return const HuffTable(0, 0, 1, kHT0Len, HuffTable._empty);
  }
}

/// Encode the pair (x, y) with [tableId] into [bs] (glint's `encode_pair`).
void mp3EncodePair(Mp3BitWriter bs, int tableId, int x, int y) {
  if (tableId == 0) return;
  final ht = getHuffTable(tableId);
  var ax = x.abs();
  var ay = y.abs();
  final linbits = ht.linbits;
  var extX = 0, extY = 0, extXBits = 0, extYBits = 0;
  if (linbits > 0 && ax >= 15) {
    extX = ax - 15;
    extXBits = linbits;
    ax = 15;
  }
  if (linbits > 0 && ay >= 15) {
    extY = ay - 15;
    extYBits = linbits;
    ay = 15;
  }
  final idx = ax * ht.xlen + ay;
  bs.writeBits(ht.code[idx], ht.len[idx]);
  if (extXBits > 0) bs.writeBits(extX, extXBits);
  if (x != 0) bs.writeBits(x < 0 ? 1 : 0, 1);
  if (extYBits > 0) bs.writeBits(extY, extYBits);
  if (y != 0) bs.writeBits(y < 0 ? 1 : 0, 1);
}

/// The number of bits [mp3EncodePair] would emit (for the rate loop) — code
/// length + linbits + one sign bit per non-zero value.
int mp3PairBits(int tableId, int x, int y) {
  if (tableId == 0) return 0;
  final ht = getHuffTable(tableId);
  var ax = x.abs();
  var ay = y.abs();
  var bits = 0;
  if (ht.linbits > 0 && ax >= 15) {
    bits += ht.linbits;
    ax = 15;
  }
  if (ht.linbits > 0 && ay >= 15) {
    bits += ht.linbits;
    ay = 15;
  }
  bits += ht.len[ax * ht.xlen + ay];
  if (x != 0) bits++;
  if (y != 0) bits++;
  return bits;
}

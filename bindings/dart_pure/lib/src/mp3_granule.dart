// lib/core/audio/mp3/mp3_granule.dart
//
// MP3 granule Huffman encoding — slice 5b of the pure-Dart MP3 encoder port.
// Ported from glint's huffman_encode + encode_count1 + choose_huff_table
// (MIT, clean-room), plus a straightforward (valid, not-yet-rate-optimal)
// region split. Emits the 3 big_values regions + the count1 quads for one
// granule of 576 quantized lines. Pure Dart => identical native + web.

import 'mp3_bitstream.dart';
import 'mp3_huffman.dart';
import 'mp3_huffman_tables.dart';

/// MPEG-1 long-block scalefactor-band boundaries (ISO 11172-3 Table B.8),
/// indexed by sample-rate index (0=44100, 1=48000, 2=32000). 23 boundaries.
const List<List<int>> kMp3SfbLong = [
  [
    0,
    4,
    8,
    12,
    16,
    20,
    24,
    30,
    36,
    44,
    52,
    62,
    74,
    90,
    110,
    134,
    162,
    196,
    238,
    288,
    342,
    418,
    576,
  ],
  [
    0,
    4,
    8,
    12,
    16,
    20,
    24,
    30,
    36,
    42,
    50,
    60,
    72,
    88,
    106,
    128,
    156,
    190,
    230,
    276,
    330,
    384,
    576,
  ],
  [
    0,
    4,
    8,
    12,
    16,
    20,
    24,
    30,
    36,
    44,
    54,
    66,
    82,
    102,
    126,
    156,
    194,
    240,
    296,
    364,
    448,
    550,
    576,
  ],
];

/// The per-granule Huffman partitioning glint calls `HuffRegions`.
class Mp3HuffRegions {
  Mp3HuffRegions({
    required this.bigValues,
    required this.region0Count,
    required this.region1Count,
    required this.tableSelect,
    required this.count1,
    required this.count1Table,
  });

  final int bigValues; // number of big_value PAIRS
  final int region0Count; // sfb-count for region 0 boundary
  final int region1Count; // sfb-count for region 1 boundary
  final List<int> tableSelect; // Huffman table id per region (length 3)
  final int count1; // number of count1 QUADS
  final int count1Table; // 0 = table 32 (A), 1 = table 33 (B)
}

/// glint's `choose_huff_table` — pick a Huffman table for a region's [maxVal].
int mp3ChooseTable(int maxVal) {
  if (maxVal == 0) return 0;
  if (maxVal <= 1) return 1;
  if (maxVal <= 2) return 3;
  if (maxVal <= 3) return 5;
  if (maxVal <= 5) return 7;
  if (maxVal <= 7) return 10;
  if (maxVal <= 15) return 13;
  var bitsNeeded = 0;
  var tmp = maxVal - 15;
  while (tmp > 0) {
    bitsNeeded++;
    tmp >>= 1;
  }
  const linbits16 = [1, 2, 3, 4, 6, 8, 10, 13];
  for (var t = 16; t < 32; t++) {
    if (linbits16[(t - 16) & 7] >= bitsNeeded && t < 24) return t;
    if (t >= 24 && const [4, 5, 6, 7, 8, 9, 11, 13][t - 24] >= bitsNeeded) {
      return t;
    }
  }
  return 24;
}

/// Encode one count1 quad (table 32/33) — glint's `encode_count1`.
void mp3EncodeCount1(Mp3BitWriter bs, int tableId, int v, int w, int x, int y) {
  final idx = ((v != 0) ? 8 : 0) |
      ((w != 0) ? 4 : 0) |
      ((x != 0) ? 2 : 0) |
      ((y != 0) ? 1 : 0);
  if (tableId == 33) {
    bs.writeBits(kHT33Code[idx], 4);
  } else {
    bs.writeBits(kHT32Code[idx], kHT32Len[idx]);
  }
  if (v != 0) bs.writeBits(v < 0 ? 1 : 0, 1);
  if (w != 0) bs.writeBits(w < 0 ? 1 : 0, 1);
  if (x != 0) bs.writeBits(x < 0 ? 1 : 0, 1);
  if (y != 0) bs.writeBits(y < 0 ? 1 : 0, 1);
}

/// Emit one granule's Huffman data (3 big_values regions + count1) — glint's
/// `huffman_encode`. [ix] is 576 quantized lines.
void mp3EncodeGranule(
  Mp3BitWriter bs,
  List<int> ix,
  Mp3HuffRegions r,
  int srIndex,
) {
  final sfb = kMp3SfbLong[srIndex];
  final bigEnd = r.bigValues * 2;
  if (bigEnd > 0) {
    var region0End = sfb[r.region0Count + 1];
    if (region0End > bigEnd) region0End = bigEnd;
    var region1End = sfb[r.region0Count + 1 + r.region1Count + 1];
    if (region1End > bigEnd) region1End = bigEnd;

    for (var i = 0; i < region0End; i += 2) {
      final y = i + 1 < region0End ? ix[i + 1] : 0;
      mp3EncodePair(bs, r.tableSelect[0], ix[i], y);
    }
    for (var i = region0End; i < region1End; i += 2) {
      final y = i + 1 < region1End ? ix[i + 1] : 0;
      mp3EncodePair(bs, r.tableSelect[1], ix[i], y);
    }
    for (var i = region1End; i < bigEnd; i += 2) {
      final y = i + 1 < bigEnd ? ix[i + 1] : 0;
      mp3EncodePair(bs, r.tableSelect[2], ix[i], y);
    }
  }
  final count1End = bigEnd + r.count1 * 4;
  final ct = r.count1Table == 1 ? 33 : 32;
  for (var i = bigEnd; i + 3 < count1End && i + 3 < 576; i += 4) {
    mp3EncodeCount1(bs, ct, ix[i], ix[i + 1], ix[i + 2], ix[i + 3]);
  }
}

/// Count the Huffman bits [ix]+[r] emit for one granule (main-data part3),
/// without materializing the bytes — the rate loop's budget probe.
int mp3GranuleBits(List<int> ix, Mp3HuffRegions r, int srIndex) {
  final w = Mp3BitWriter();
  mp3EncodeGranule(w, ix, r, srIndex);
  return w.bitCount;
}

/// A valid (not yet rate-optimal) region split for [ix] (576 lines) at
/// [srIndex]: count1 = trailing quads of 0/±1, big_values = the rest, split
/// into thirds by scalefactor band with per-region table selection.
Mp3HuffRegions mp3ComputeRegions(List<int> ix, int srIndex) {
  final sfb = kMp3SfbLong[srIndex];
  var rzero = 576;
  while (rzero > 0 && ix[rzero - 1] == 0) {
    rzero--;
  }
  // count1: from rzero back, whole quads whose values are all in {-1,0,1}.
  var count1Start = rzero;
  while (count1Start >= 4) {
    var ok = true;
    for (var j = count1Start - 4; j < count1Start; j++) {
      if (ix[j] < -1 || ix[j] > 1) {
        ok = false;
        break;
      }
    }
    if (!ok) break;
    count1Start -= 4;
  }
  final bigEnd = count1Start; // even (multiple of 4)
  final bigValues = bigEnd ~/ 2;
  final count1 = (rzero - count1Start) ~/ 4;

  int maxIn(int a, int b) {
    var m = 0;
    for (var i = a; i < b && i < 576; i++) {
      final v = ix[i].abs();
      if (v > m) m = v;
    }
    return m;
  }

  // Split big_values into thirds by scalefactor band index.
  var r0 = 7, r1 = 13;
  if (bigEnd > 0) {
    // region0 ~ up to the sfb whose boundary is nearest bigEnd/3.
    r0 = _sfbCountFor(sfb, bigEnd ~/ 3);
    r1 = _sfbCountFor(sfb, (bigEnd * 2) ~/ 3) - r0 - 1;
    if (r0 < 0) r0 = 0;
    if (r0 > 15) r0 = 15;
    if (r1 < 0) r1 = 0;
    if (r1 > 7) r1 = 7;
    if (r0 + r1 + 2 > 22) r1 = 22 - r0 - 2;
  }
  final region0End = bigEnd == 0 ? 0 : sfb[(r0 + 1).clamp(0, 22)];
  final region1End = bigEnd == 0 ? 0 : sfb[(r0 + 1 + r1 + 1).clamp(0, 22)];
  final r0End = region0End.clamp(0, bigEnd);
  final r1End = region1End.clamp(0, bigEnd);
  final tables = [
    mp3ChooseTable(maxIn(0, r0End)),
    mp3ChooseTable(maxIn(r0End, r1End)),
    mp3ChooseTable(maxIn(r1End, bigEnd)),
  ];
  return Mp3HuffRegions(
    bigValues: bigValues,
    region0Count: r0,
    region1Count: r1,
    tableSelect: tables,
    count1: count1,
    count1Table: 0,
  );
}

/// The sfb-band index whose boundary first reaches [pos].
int _sfbCountFor(List<int> sfb, int pos) {
  for (var i = 0; i < sfb.length; i++) {
    if (sfb[i] >= pos) return i;
  }
  return sfb.length - 1;
}

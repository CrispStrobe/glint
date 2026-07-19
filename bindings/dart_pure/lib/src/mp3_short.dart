// lib/core/audio/mp3/mp3_short.dart
//
// Short/transient (window-switching) block ENCODING — opt-in, kept separate so
// the published long-block path is untouched. Covers the transient scheduler
// (glint's schedule_block_types), the window-switching Huffman layout (region0
// hardwired to line 36, two tables, no reorder for start/stop; short blocks add
// the [sfb][window][line] reorder), and a simple gain-search quantizer (zero
// short scalefactors, subblock_gain 0 — valid + decodable). The decoder already
// reads all of this. Pure Dart => native + web.

import 'dart:math' as math;
import 'dart:typed_data';

import 'mp3_bitstream.dart';
import 'mp3_granule.dart';
import 'mp3_huffman.dart';
import 'mp3_huffman_tables.dart';
import 'mp3_quantize.dart';
import 'mp3_shape.dart';

/// MPEG-1 short-block sfb boundaries (per window, 0..192), by sr index — for the
/// short-block reorder.
const List<List<int>> kMp3SfbShort = [
  [0, 4, 8, 12, 16, 22, 30, 40, 52, 66, 84, 106, 136, 192], // 44100
  [0, 4, 8, 12, 16, 22, 28, 38, 50, 64, 80, 100, 126, 192], // 48000
  [0, 4, 8, 12, 16, 22, 30, 42, 58, 78, 104, 138, 180, 192], // 32000
];

/// Per-frame transient scheduler — decides a block type (0 long, 1 start,
/// 2 short, 3 stop) per granule, keeping the long→start→short→stop→long chain
/// valid across granules and frames. Ported from glint's schedule_block_types
/// (no encoder lookahead here, so slightly less eager on cross-frame attacks).
class Mp3BlockScheduler {
  /// Test hook: force every granule to this block type (0/1/2/3). Null = normal.
  static int? debugForceType;

  /// Test hook: if non-null, each schedule() appends its `[t0,t1]` here.
  static List<List<int>>? debugLog;

  /// Test hook: if non-null, block types are taken from this flat granule
  /// sequence (by a global granule counter) instead of the energy scheduler.
  static List<int>? debugForceSeq;
  static int _seqPos = 0;

  static const double _attackRatio = 6.0; // ~7.8 dB energy jump
  static const int _shortExtend = 2; // keep short this many granules after

  double _prevEnergy = 0.0;
  bool _energyValid = false;
  int _shortRun = 0;
  int _carry = 0; // window chain carried into the next frame (0/2/3)

  /// [grEnergy] are the two granules' subband energies. Returns `[t0, t1]`.
  List<int> schedule(List<double> grEnergy) {
    if (debugForceSeq != null) {
      final s = debugForceSeq!;
      final r = [s[_seqPos % s.length], s[(_seqPos + 1) % s.length]];
      _seqPos += 2;
      debugLog?.add(r);
      return r;
    }
    if (debugForceType != null) {
      final r = [debugForceType!, debugForceType!];
      debugLog?.add(r);
      return r;
    }
    final want = [false, false];
    for (var g = 0; g < 2; g++) {
      if (_energyValid &&
          _prevEnergy > 0.0 &&
          grEnergy[g] > _attackRatio * _prevEnergy) {
        want[g] = true;
      }
      _prevEnergy = grEnergy[g];
      _energyValid = true;
    }
    for (var g = 0; g < 2; g++) {
      if (want[g]) {
        _shortRun = _shortExtend;
      } else if (_shortRun > 0) {
        want[g] = true;
        _shortRun--;
      }
    }
    _prevEnergy = grEnergy[1];

    final types = [0, 0];
    if (_carry == 2) {
      types[0] = 2;
    } else if (_carry == 3) {
      types[0] = want[0] ? 2 : 3;
    }
    for (var g = 0; g < 2; g++) {
      if (types[g] != 0 || !want[g]) continue;
      final prev = g > 0 ? types[g - 1] : -1; // -1 = previous frame (long)
      if (prev == 1 || prev == 2) {
        types[g] = 2;
      } else if (prev == 0) {
        types[g - 1] = 1;
        types[g] = 2;
      } else {
        types[g] = 1;
      }
    }
    // Enforce the forward chain inside the frame.
    if (types[0] == 1 && types[1] != 2) {
      types[1] = 2;
    } else if (types[0] == 2 && types[1] == 0) {
      types[1] = 3;
    }

    final last = types[1];
    _carry = last == 1 ? 2 : (last == 2 ? 3 : 0);
    debugLog?.add(types);
    return types;
  }
}

/// Reorder a short-block MDCT output `[sb][win][k]` (flat sb*18+win*6+k) into
/// the `[sfb][window][line]` frequency order (glint's reorder_short_blocks).
Float64List mp3ReorderShort(Float64List mdctShort, int srIndex) {
  final sfb = kMp3SfbShort[srIndex];
  final flat = Float64List(576);
  var outIdx = 0;
  for (var s = 0; s < 13; s++) {
    final width = sfb[s + 1] - sfb[s];
    for (var win = 0; win < 3; win++) {
      for (var j = 0; j < width; j++) {
        final freq = sfb[s] + j;
        final sb = freq ~/ 6;
        final k = freq % 6;
        if (sb < 32) flat[outIdx] = mdctShort[sb * 18 + win * 6 + k];
        outIdx++;
      }
    }
  }
  return flat;
}

/// Candidate Huffman tables for a region's [maxVal] (ISO `table_candidates`) —
/// crucially picks the ESC table whose linbits actually cover the value (a
/// hardcoded list that stops at table 24 (linbits 4, max ~30) silently
/// truncates larger values and under-counts bits).
List<int> _tableCandidates(int maxVal) {
  if (maxVal <= 1) return const [1, 2, 3];
  if (maxVal <= 2) return const [2, 3];
  if (maxVal <= 3) return const [5, 6];
  if (maxVal <= 5) return const [7, 8, 9];
  if (maxVal <= 7) return const [10, 11, 12];
  if (maxVal <= 15) return const [13, 15];
  var bitsNeeded = 0;
  var tmp = maxVal - 15;
  while (tmp > 0) {
    bitsNeeded++;
    tmp >>= 1;
  }
  const lin16 = [1, 2, 3, 4, 6, 8, 10, 13];
  const lin24 = [4, 5, 6, 7, 8, 9, 11, 13];
  final out = <int>[];
  for (var t = 16; t < 24; t++) {
    if (lin16[t - 16] >= bitsNeeded) {
      out.add(t);
      break;
    }
  }
  for (var t = 24; t < 32; t++) {
    if (lin24[t - 24] >= bitsNeeded) {
      out.add(t);
      break;
    }
  }
  return out.isEmpty ? const [31] : out;
}

/// Cheapest big-values table for [start,end) by actual bit cost.
int _bestTable(List<int> ix, int start, int end) {
  if (start >= end) return 0;
  var maxVal = 0;
  for (var i = start; i < end; i++) {
    final v = ix[i].abs();
    if (v > maxVal) maxVal = v;
  }
  if (maxVal == 0) return 0;
  final cands = _tableCandidates(maxVal);
  var best = cands[0], bestBits = 1 << 30;
  for (final t in cands) {
    var bits = 0;
    for (var i = start; i < end; i += 2) {
      bits += mp3PairBits(t, ix[i], i + 1 < end ? ix[i + 1] : 0);
    }
    if (bits < bestBits) {
      bestBits = bits;
      best = t;
    }
  }
  return best;
}

/// Window-switching Huffman regions: region0 = [0, min(36, bigEnd)), region1 =
/// [that, bigEnd), two tables; count1 as usual. `region0Count` carries the LINE
/// boundary (36) so the decoder's implied layout matches.
Mp3HuffRegions mp3ComputeRegionsWs(List<int> ix, int srIndex) {
  var rzero = 576;
  while (rzero > 0 && ix[rzero - 1] == 0) {
    rzero--;
  }
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
  if (count1Start.isOdd) count1Start++;
  final bigEnd = count1Start;
  final bigValues = bigEnd ~/ 2;
  final count1 = (rzero - count1Start) ~/ 4;

  final r0End = bigEnd < 36 ? bigEnd : 36;
  final t0 = _bestTable(ix, 0, r0End);
  final t1 = _bestTable(ix, r0End, bigEnd);
  var bits = 0;
  for (var i = 0; i < r0End; i += 2) {
    bits += mp3PairBits(t0, ix[i], i + 1 < r0End ? ix[i + 1] : 0);
  }
  for (var i = r0End; i < bigEnd; i += 2) {
    bits += mp3PairBits(t1, ix[i], i + 1 < bigEnd ? ix[i + 1] : 0);
  }
  var count1Table = 0;
  if (count1 > 0) {
    var bitsA = 0, bitsB = 0;
    for (var i = bigEnd; i + 3 < rzero; i += 4) {
      final mask = ((ix[i] != 0 ? 1 : 0) << 3) |
          ((ix[i + 1] != 0 ? 1 : 0) << 2) |
          ((ix[i + 2] != 0 ? 1 : 0) << 1) |
          (ix[i + 3] != 0 ? 1 : 0);
      final signs = (mask & 1) +
          ((mask >> 1) & 1) +
          ((mask >> 2) & 1) +
          ((mask >> 3) & 1);
      bitsA += kHT32Len[mask] + signs;
      bitsB += 4 + signs;
    }
    if (bitsB < bitsA) count1Table = 1;
    bits += bitsB < bitsA ? bitsB : bitsA;
  }

  return Mp3HuffRegions(
    bigValues: bigValues,
    region0Count: 36, // LINE boundary (window-switching layout)
    region1Count: 576,
    tableSelect: [t0, t1, 0],
    count1: count1,
    count1Table: count1Table,
    bits: bits,
  );
}

/// Emit a window-switching granule's Huffman data: region0 = [0, min(36,
/// bigEnd)) with table[0], region1 = [36, bigEnd) with table[1], then count1.
void mp3EncodeGranuleWs(Mp3BitWriter bs, List<int> ix, Mp3HuffRegions r) {
  final bigEnd = r.bigValues * 2;
  final r0End = bigEnd < 36 ? bigEnd : 36;
  for (var i = 0; i < r0End; i += 2) {
    mp3EncodePair(bs, r.tableSelect[0], ix[i], i + 1 < r0End ? ix[i + 1] : 0);
  }
  for (var i = r0End; i < bigEnd; i += 2) {
    mp3EncodePair(bs, r.tableSelect[1], ix[i], i + 1 < bigEnd ? ix[i + 1] : 0);
  }
  final count1End = bigEnd + r.count1 * 4;
  final ct = r.count1Table == 1 ? 33 : 32;
  for (var i = bigEnd; i + 3 < count1End && i + 3 < 576; i += 4) {
    mp3EncodeCount1(bs, ct, ix[i], ix[i + 1], ix[i + 2], ix[i + 3]);
  }
}

/// Quantize a window-switching granule (long-length spectrum [mdct], already
/// reordered for short blocks): binary-search global_gain to fit [availBits]
/// with zero scalefactors + subblock_gain, using the WS Huffman layout. Returns
/// an [Mp3GranuleInfo] with [Mp3GranuleInfo.blockType] set to [blockType].
Mp3GranuleInfo mp3QuantizeGranuleWs(
  Float64List mdct,
  int availBits,
  int srIndex,
  int blockType,
) {
  // Minimum global_gain that keeps the peak below the 8191 clip: quantizing
  // finer clamps large coefficients to 8191 (silently corrupting them). glint's
  // gain-bound: g > 210 − (16/3)·log2(8190/peak34).
  var peak34 = 0.0;
  for (var i = 0; i < 576; i++) {
    final p = mp3Pow34(mdct[i].abs());
    if (p > peak34) peak34 = p;
  }
  var minGain = 0;
  if (peak34 > 0.0) {
    minGain =
        (210.0 - (16.0 / 3.0) * (math.log(8190.0 / peak34) / math.ln2)).ceil();
    if (minGain < 0) minGain = 0;
    if (minGain > 255) minGain = 255;
  }
  var lo = minGain, hi = 255, bestGain = 255;
  Int16List? bestIx;
  Mp3HuffRegions? bestReg;
  while (lo <= hi) {
    final mid = (lo + hi) >> 1;
    final ix = mp3QuantizeUniform(mdct, mid);
    final reg = mp3ComputeRegionsWs(ix, srIndex);
    if (reg.bits <= availBits) {
      bestGain = mid;
      bestIx = ix;
      bestReg = reg;
      hi = mid - 1; // finer
    } else {
      lo = mid + 1; // coarser
    }
  }
  var gain = bestGain;
  var ix = bestIx ?? mp3QuantizeUniform(mdct, 255);
  var reg = bestReg ?? mp3ComputeRegionsWs(ix, srIndex);
  final limit = availBits < 4095 ? availBits : 4095;
  while (reg.bits > limit && gain < 255) {
    gain++;
    ix = mp3QuantizeUniform(mdct, gain);
    reg = mp3ComputeRegionsWs(ix, srIndex);
  }
  return Mp3GranuleInfo(
    ix: ix,
    globalGain: gain,
    rcGain: gain,
    scalefac: List<int>.filled(21, 0),
    scalefacCompress: 0,
    scalefacScale: 0,
    preflag: 0,
    part2Length: 0,
    part23Length: reg.bits,
    regions: reg,
    blockType: blockType,
  );
}

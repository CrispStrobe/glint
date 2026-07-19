// lib/core/audio/mp3/mp3_shape.dart
//
// The MP3 rate/distortion quantization loop — ported from glint's quantize.cpp
// (`quantize_granule` → factor-grid MSE search → `gain_search_with_scalefacs`
// → `nmr_outer_loop`). This is what replaces the first-cut zero-scalefactor
// path: it shapes quantization noise UNDER the psychoacoustic masking threshold
// (mp3_psycho.dart) by amplifying the scalefactors of the worst noise-to-mask
// bands and re-coarsening global_gain to pay for them, so noise moves from
// audible bands into masked ones. Pure Dart => identical native + web.
//
// NB: glint's exact Huffman region optimizer isn't ported (we use
// mp3_granule's region split), so bit counts — and thus the last global_gain
// step — can differ by a little; the psychoacoustic decisions (which sfbs get
// amplified, scalefac_scale/preflag) match, which is what closes the SNR/NMR
// gap. See bench/README.md for the A/B.

import 'dart:math' as math;
import 'dart:typed_data';

import 'mp3_granule.dart';
import 'mp3_psycho.dart';
import 'mp3_quantize.dart';

/// One granule's quantization result (glint's `GranuleInfo`, long-block subset).
class Mp3GranuleInfo {
  Mp3GranuleInfo({
    required this.ix,
    required this.globalGain,
    required this.rcGain,
    required this.scalefac,
    required this.scalefacCompress,
    required this.scalefacScale,
    required this.preflag,
    required this.part2Length,
    required this.part23Length,
    required this.regions,
  });

  final Int16List ix;
  int globalGain;
  int rcGain;
  final List<int> scalefac; // 21 sfb gains
  int scalefacCompress;
  int scalefacScale;
  int preflag;
  int part2Length; // scalefactor bits
  int part23Length; // scalefactor + Huffman bits
  Mp3HuffRegions regions;

  Mp3GranuleInfo copy() => Mp3GranuleInfo(
        ix: Int16List.fromList(ix),
        globalGain: globalGain,
        rcGain: rcGain,
        scalefac: List<int>.of(scalefac),
        scalefacCompress: scalefacCompress,
        scalefacScale: scalefacScale,
        preflag: preflag,
        part2Length: part2Length,
        part23Length: part23Length,
        regions: regions,
      );
}

const List<int> _kSfMax = [
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, //
  7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
];
// glint's 4-bit scalefac_compress table (slen1, slen2) pairs.
const List<List<int>> _kSlenTable = [
  [0, 0], [0, 1], [0, 2], [0, 3], [3, 0], [1, 1], [1, 2], [1, 3], //
  [2, 1], [2, 2], [2, 3], [3, 1], [3, 2], [3, 3], [4, 2], [4, 3],
];

double _shapeTarget() => 0.125;
double _noiseGuard() => 1.25;

/// Public entry: quantize one long-block granule of 576 MDCT coefficients to
/// [availableBits], returning the shaped [Mp3GranuleInfo]. [quality] 1=normal,
/// 2=best (more factor candidates + more shaping iterations); 0 = no shaping.
Mp3GranuleInfo mp3QuantizeGranule(
  Float64List mdct,
  int availableBits,
  int srIndex, {
  int quality = 2,
  bool tonalMasks = false,
  int gainFloor = 0,
  bool vbrShaping = false,
  bool allowPsy = true,
}) {
  final sfb = kMp3SfbLong[srIndex < 3 ? srIndex : 0];
  // allowPsy=false (the M/S side channel) skips noise shaping entirely: side
  // noise leaks into the decoded L/R where the side signal doesn't mask it, so
  // shaping against the side's own masks destroys joint-mode quality.
  if (quality <= 0 || !allowPsy) {
    return _quantizeBase(mdct, availableBits, srIndex, sfb, gainFloor);
  }

  const kNormal = [1.0, 1.04, 1.09, 1.15, 1.22, 1.30];
  const kBest = [
    1.0,
    1.02,
    1.05,
    1.08,
    1.11,
    1.15,
    1.19,
    1.24,
    1.30,
    1.38,
    1.48,
    1.60,
  ];
  final factors = quality >= 2 ? kBest : kNormal;

  final srcBand = mp3ComputeSrcBand(mdct, srIndex);

  var best = _quantizeBase(
    _scale(mdct, factors[0]),
    availableBits,
    srIndex,
    sfb,
    gainFloor,
  );
  var bestMse = _granuleMse(best, mdct, srIndex, sfb);
  var bestFactor = factors[0];
  for (var fi = 1; fi < factors.length; fi++) {
    final r = _quantizeBase(
      _scale(mdct, factors[fi]),
      availableBits,
      srIndex,
      sfb,
      gainFloor,
    );
    final mse = _granuleMse(r, mdct, srIndex, sfb);
    if (mse < bestMse) {
      bestMse = mse;
      best = r;
      bestFactor = factors[fi];
    }
  }

  if (quality >= 2) {
    // Fine refinement around the winning factor (steps ±1, ±2 of 0.015).
    for (var step = -2; step <= 2; step++) {
      if (step == 0) continue;
      final f = bestFactor + step * 0.015;
      if (f < 0.98) continue;
      final r = _quantizeBase(
        _scale(mdct, f),
        availableBits,
        srIndex,
        sfb,
        gainFloor,
      );
      final mse = _granuleMse(r, mdct, srIndex, sfb);
      if (mse < bestMse) {
        bestMse = mse;
        best = r;
        bestFactor = f;
      }
    }
  }

  // NMR-driven noise shaping on the winner (MPEG-1 long blocks only).
  final maxIters = quality >= 2 ? 20 : 8;
  best = _nmrOuterLoop(
    best,
    bestFactor,
    mdct,
    availableBits,
    srIndex,
    maxIters,
    srcBand,
    sfb,
    tonalMasks,
    gainFloor,
    vbrShaping,
  );
  return best;
}

Float64List _scale(Float64List mdct, double f) {
  final out = Float64List(576);
  for (var i = 0; i < 576; i++) {
    out[i] = mdct[i] * f;
  }
  return out;
}

/// Base quantizer: zero scalefactors + a global_gain search (glint
/// `quantize_base`, kEnergySeed=false). Sets rc_gain to the found gain.
Mp3GranuleInfo _quantizeBase(
  Float64List mdct,
  int availableBits,
  int srIndex,
  List<int> sfb,
  int gainFloor,
) {
  final gi = Mp3GranuleInfo(
    ix: Int16List(576),
    globalGain: 210,
    rcGain: 210,
    scalefac: List<int>.filled(21, 0),
    scalefacCompress: 0,
    scalefacScale: 0,
    preflag: 0,
    part2Length: 0,
    part23Length: 0,
    regions: mp3ComputeRegions(Int16List(576), srIndex),
  );
  _gainSearchWithScalefacs(gi, mdct, availableBits, srIndex, sfb, gainFloor);
  gi.rcGain = gi.globalGain;
  return gi;
}

/// Binary-search global_gain to the bit budget with [gi]'s scalefactors fixed
/// (glint `gain_search_with_scalefacs`). Sets ix/global_gain/regions/part2_3.
void _gainSearchWithScalefacs(
  Mp3GranuleInfo gi,
  Float64List mdct,
  int availableBits,
  int srIndex,
  List<int> sfb,
  int gainFloor,
) {
  var targetBits = availableBits - gi.part2Length;
  if (targetBits < 0) targetBits = 0;

  // peak34 including the per-band scalefactor boost — bounds the gain search.
  final sfb21 = sfb[21];
  var peak34 = 0.0;
  var band = 0;
  for (var i = 0; i < 576; i++) {
    while (band < 21 && i >= sfb[band + 1]) {
      band++;
    }
    var sf = 0;
    if (band < 21 && i < sfb21) {
      sf = gi.scalefac[band];
      if (gi.preflag != 0) sf += kMp3Preemphasis[band];
    }
    final sfs = (sf > 0 && sf < 16) ? kMp3SfTable[gi.scalefacScale][sf] : 1.0;
    final p = mp3Pow34(mdct[i].abs()) * sfs;
    if (p > peak34) peak34 = p;
  }

  var minGain = 0, maxGain = 255;
  if (peak34 > 0.0) {
    final gMin = 210.0 - (16.0 / 3.0) * _log2(8190.0 / peak34);
    minGain = gMin.ceil();
    if (minGain < 0) minGain = 0;
    final gMax = 210.0 - (16.0 / 3.0) * _log2(0.6 / peak34);
    final est = gMax.toInt() + 2;
    if (est < maxGain && est > minGain) maxGain = est;
  }
  // Rate-control anchor: never quantize finer than the floor, so easy granules
  // bank bits into the reservoir instead of gold-plating SNR (glint's gain_floor).
  if (gainFloor > minGain) minGain = gainFloor;
  if (maxGain < minGain) maxGain = minGain;

  var lo = minGain, hi = maxGain, bestGain = maxGain;
  var bestBits = -1;
  Int16List? bestIx;
  Mp3HuffRegions? bestRegions;
  for (var iter = 0; iter < 8 && lo <= hi; iter++) {
    final gain = (lo + hi) ~/ 2;
    final ix = mp3QuantizeScaled(
      mdct,
      gain,
      gi.scalefac,
      gi.scalefacScale,
      gi.preflag,
      sfb,
    );
    final regs = mp3ComputeRegions(ix, srIndex);
    final bits = regs.bits;
    if (bits <= targetBits) {
      hi = gain - 1;
      bestGain = gain;
      bestBits = bits;
      bestIx = ix;
      bestRegions = regs;
    } else {
      lo = gain + 1;
    }
  }
  gi.globalGain = bestGain;
  int huffBits;
  if (bestBits >= 0) {
    _copyInto(gi.ix, bestIx!);
    gi.regions = bestRegions!;
    huffBits = bestBits;
  } else {
    final ix = mp3QuantizeScaled(
      mdct,
      bestGain,
      gi.scalefac,
      gi.scalefacScale,
      gi.preflag,
      sfb,
    );
    _copyInto(gi.ix, ix);
    gi.regions = mp3ComputeRegions(ix, srIndex);
    huffBits = gi.regions.bits;
  }
  gi.part23Length = gi.part2Length + huffBits;

  // Budget guarantee: coarsen until part2_3 fits the budget + the 12-bit field.
  var limit = availableBits;
  if (limit > 4095) limit = 4095;
  while (gi.part23Length > limit && gi.globalGain < 255) {
    gi.globalGain++;
    final ix = mp3QuantizeScaled(
      mdct,
      gi.globalGain,
      gi.scalefac,
      gi.scalefacScale,
      gi.preflag,
      sfb,
    );
    _copyInto(gi.ix, ix);
    gi.regions = mp3ComputeRegions(gi.ix, srIndex);
    huffBits = gi.regions.bits;
    gi.part23Length = gi.part2Length + huffBits;
  }
}

/// NMR outer loop (glint `nmr_outer_loop`): amplify the worst noise-to-mask
/// bands, re-search global_gain, keep the best iterate by Σ noise/mask.
Mp3GranuleInfo _nmrOuterLoop(
  Mp3GranuleInfo start,
  double factor,
  Float64List mdct,
  int availableBits,
  int srIndex,
  int maxIters,
  Float64List srcBand,
  List<int> sfb,
  bool tonalMasks,
  int gainFloor,
  bool vbrShaping,
) {
  final maskBand = tonalMasks
      ? mp3ComputeBandMasks(
          srcBand,
          srIndex,
          alpha: mp3ComputeBandTonality(mdct, srIndex),
        )
      : mp3ComputeBandMasks(srcBand, srIndex);

  var curNoise = _computeBandNoise(start, mdct, srIndex, sfb);
  var jBest = _nmrObjective(curNoise, maskBand);
  final target = _shapeTarget();

  var worst0 = 0.0;
  for (var b = 0; b < 21; b++) {
    if (maskBand[b] <= 0.0) continue;
    final r = curNoise[b] / maskBand[b];
    if (r > worst0) worst0 = r;
  }
  if (worst0 <= target) return start;

  var total0 = 0.0;
  for (var b = 0; b < 21; b++) {
    total0 += curNoise[b];
  }

  final scaled = _scale(mdct, factor);
  // CBR: spend the unshaped size + half the leftover slot. VBR: bound at
  // unshaped×1.25 so a big ceiling doesn't let shaping balloon the frame.
  var shapeBits = vbrShaping
      ? start.part23Length + start.part23Length ~/ 4
      : start.part23Length + (availableBits - start.part23Length) ~/ 2;
  if (shapeBits > availableBits) shapeBits = availableBits;
  final kNoiseGuard = _noiseGuard();

  var best = start;
  var cur = start;
  var stall = 0;

  for (var iter = 0; iter < maxIters; iter++) {
    var worst = 0.0;
    for (var b = 0; b < 21; b++) {
      if (maskBand[b] <= 0.0) continue;
      final r = curNoise[b] / maskBand[b];
      if (r > worst) worst = r;
    }
    if (worst <= target) break;
    final thresh = math.max(target, worst * 0.25);

    final cand = cur.copy();
    var amplified = false;
    var cappedNeed = false;
    for (var b = 0; b < 21; b++) {
      if (maskBand[b] <= 0.0) continue;
      final r = curNoise[b] / maskBand[b];
      if (r >= thresh) {
        if (cand.scalefac[b] < _kSfMax[b]) {
          cand.scalefac[b]++;
          amplified = true;
        } else if (r > 4.0) {
          cappedNeed = true;
        }
      }
    }
    if (!amplified && cappedNeed && cand.scalefacScale == 0) {
      cand.scalefacScale = 1;
      for (var b = 0; b < 21; b++) {
        cand.scalefac[b] = (cand.scalefac[b] + 1) ~/ 2;
      }
      amplified = true;
    }
    if (!amplified) break;
    if (srIndex < 3) _tryFoldPreflag(cand);
    if (!_encodeScalefacFields(cand, srIndex)) break;
    _gainSearchWithScalefacs(cand, scaled, shapeBits, srIndex, sfb, gainFloor);

    final candNoise = _computeBandNoise(cand, mdct, srIndex, sfb);
    final jCand = _nmrObjective(candNoise, maskBand);
    var candTotal = 0.0;
    for (var b = 0; b < 21; b++) {
      candTotal += candNoise[b];
    }
    if (jCand < jBest && candTotal <= total0 * kNoiseGuard) {
      jBest = jCand;
      best = cand;
      stall = 0;
    } else if (++stall >= 3) {
      break;
    }
    cur = cand;
    curNoise = candNoise;
  }
  return best;
}

double _nmrObjective(Float64List noiseBand, Float64List maskBand) {
  var j = 0.0;
  for (var b = 0; b < 21; b++) {
    if (maskBand[b] > 0.0) j += noiseBand[b] / maskBand[b];
  }
  return j;
}

/// Per-band decoder-reconstruction noise (glint `compute_band_noise`): the
/// sfb21 region reconstructs with sf_d=1 and its noise folds into band 20.
Float64List _computeBandNoise(
  Mp3GranuleInfo gi,
  Float64List mdct,
  int srIndex,
  List<int> sfb,
) {
  final decoderGain = math.pow(2.0, 0.25 * (gi.globalGain - 210)).toDouble();
  final noise = Float64List(21);
  for (var b = 0; b < 22; b++) {
    final start = sfb[b];
    final end = (b < 21) ? sfb[b + 1] : 576;
    if (start >= end) continue;
    var sfD = 1.0;
    if (b < 21) {
      var sf = gi.scalefac[b];
      if (gi.preflag != 0) sf += kMp3Preemphasis[b];
      sfD = math.pow(2.0, -0.5 * sf * (1 + gi.scalefacScale)).toDouble();
    }
    var acc = 0.0;
    final g = decoderGain * sfD;
    for (var i = start; i < end; i++) {
      final m = gi.ix[i];
      var xrHat = 0.0;
      if (m != 0) {
        final a = m.abs();
        final a43 = a * math.pow(a, 1.0 / 3.0).toDouble();
        xrHat = (mdct[i] < 0 ? -1.0 : 1.0) * a43 * g;
      }
      final err = mdct[i] - xrHat;
      acc += err * err;
    }
    noise[(b < 21) ? b : 20] += acc;
  }
  return noise;
}

/// Decoder-reconstruction MSE for factor selection (glint `granule_mse`).
double _granuleMse(
  Mp3GranuleInfo gi,
  Float64List mdct,
  int srIndex,
  List<int> sfb,
) {
  final decoderGain = math.pow(2.0, 0.25 * (gi.globalGain - 210)).toDouble();
  var noise = 0.0;
  for (var b = 0; b < 22; b++) {
    final start = sfb[b];
    final end = (b < 21) ? sfb[b + 1] : 576;
    if (start >= end) continue;
    var sfD = 1.0;
    if (b < 21) {
      var sf = gi.scalefac[b];
      if (gi.preflag != 0) sf += kMp3Preemphasis[b];
      sfD = math.pow(2.0, -0.5 * sf * (1 + gi.scalefacScale)).toDouble();
    }
    final g = decoderGain * sfD;
    for (var i = start; i < end; i++) {
      final m = gi.ix[i];
      var xrHat = 0.0;
      if (m != 0) {
        final a = m.abs();
        final a43 = a * math.pow(a, 1.0 / 3.0).toDouble();
        xrHat = (mdct[i] < 0 ? -1.0 : 1.0) * a43 * g;
      }
      final err = mdct[i] - xrHat;
      noise += err * err;
    }
  }
  return noise;
}

/// Fold the pretab into preflag when every HF band's sf covers it (glint
/// `try_fold_preflag`): lossless, shrinks slen2. MPEG-1 long only.
void _tryFoldPreflag(Mp3GranuleInfo gi) {
  if (gi.preflag != 0) return;
  var any = false;
  for (var b = 11; b < 21; b++) {
    if (gi.scalefac[b] < kMp3Preemphasis[b]) return;
    if (kMp3Preemphasis[b] > 0 && gi.scalefac[b] > 0) any = true;
  }
  if (!any) return;
  gi.preflag = 1;
  for (var b = 11; b < 21; b++) {
    gi.scalefac[b] -= kMp3Preemphasis[b];
  }
}

/// Compute scalefac_compress + part2_length from scalefac[21] (glint
/// `encode_scalefac_fields`, MPEG-1 long). Returns false if not representable.
bool _encodeScalefacFields(Mp3GranuleInfo gi, int srIndex) {
  var maxSf1 = 0, maxSf2 = 0;
  for (var b = 0; b < 11; b++) {
    if (gi.scalefac[b] > maxSf1) maxSf1 = gi.scalefac[b];
  }
  for (var b = 11; b < 21; b++) {
    if (gi.scalefac[b] > maxSf2) maxSf2 = gi.scalefac[b];
  }
  if (maxSf1 > 15 || maxSf2 > 7) return false;
  var slen1 = 0;
  while ((1 << slen1) <= maxSf1) {
    slen1++;
  }
  var slen2 = 0;
  while ((1 << slen2) <= maxSf2) {
    slen2++;
  }
  var bestSfc = -1, bestCost = 1 << 30;
  for (var i = 0; i < 16; i++) {
    if (_kSlenTable[i][0] >= slen1 && _kSlenTable[i][1] >= slen2) {
      final cost = _kSlenTable[i][0] * 11 + _kSlenTable[i][1] * 10;
      if (cost < bestCost) {
        bestCost = cost;
        bestSfc = i;
      }
    }
  }
  if (bestSfc < 0) return false;
  gi.scalefacCompress = bestSfc;
  gi.part2Length = bestCost;
  return true;
}

void _copyInto(Int16List dst, Int16List src) {
  for (var i = 0; i < 576; i++) {
    dst[i] = src[i];
  }
}

double _log2(double x) => math.log(x) / math.ln2;

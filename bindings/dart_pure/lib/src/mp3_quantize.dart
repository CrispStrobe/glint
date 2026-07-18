// lib/core/audio/mp3/mp3_quantize.dart
//
// MP3 nonlinear (power-law) quantizer — slice 4 of the pure-Dart MP3 encoder
// port. Ported from glint's `quantize_and_count` core + `gain_table`/`pow34`
// (MIT, clean-room). The uniform quantizer (no per-band scalefactors yet — those
// arrive with the rate-distortion loop that needs Huffman bit counts). Pure
// dart:math+typed_data => identical native + web.

import 'dart:math' as math;
import 'dart:typed_data';

/// The classic MP3 quantizer rounding bias.
const double _kQuantBias = 0.4054;

/// Largest quantized magnitude (13-bit big_values ceiling).
const int _kMaxQuant = 8191;

/// Encoder step table: `gain_table[g] = 2^(-3·(g-210)/16)`.
final Float64List kMp3GainTable = _buildGainTable();

Float64List _buildGainTable() {
  final t = Float64List(256);
  for (var g = 0; g < 256; g++) {
    t[g] = math.pow(2.0, -3.0 * (g - 210) / 16.0).toDouble();
  }
  return t;
}

/// x^0.75 for the quantizer (glint's `fast_pow34`, here exact).
double mp3Pow34(double x) => math.pow(x, 0.75).toDouble();

/// ISO 11172-3 pretab (glint `tables::preemphasis[22]`): added to the HF-band
/// scalefactors when `preflag` is set (band 20 is 2, not 3).
const List<int> kMp3Preemphasis = [
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, //
  1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0,
];

/// Encoder-side scalefactor multiplier (glint `sf_table[scalefac_scale][sf]`),
/// the positive exponent that cancels the decoder's `2^(-0.5(1+sfs)·sf)`:
/// `[0][sf]=2^(0.75·0.5·sf)`, `[1][sf]=2^(0.75·sf)`.
final List<Float64List> kMp3SfTable = _buildSfTable();

List<Float64List> _buildSfTable() {
  final t = [Float64List(16), Float64List(16)];
  for (var sf = 0; sf < 16; sf++) {
    t[0][sf] = math.pow(2.0, 0.75 * sf * 0.5).toDouble();
    t[1][sf] = math.pow(2.0, 0.75 * sf * 1.0).toDouble();
  }
  return t;
}

/// Quantize with per-scalefactor-band gain (glint's `quantize_and_count` core,
/// long blocks). [scalefac] is the 21 sfb gains, [scalefacScale] 0/1 picks the
/// step, [preflag] adds the pretab; [sfbLong] is the 23-entry sfb boundary row.
/// Bins at/above `sfb[21]` (the untransmitted "sfb21" region) get no
/// scalefactor — mirroring the decoder exactly.
Int16List mp3QuantizeScaled(
  Float64List mdct,
  int globalGain,
  List<int> scalefac,
  int scalefacScale,
  int preflag,
  List<int> sfbLong,
) {
  final base = kMp3GainTable[globalGain];
  final sfb21 = sfbLong[21];
  final ix = Int16List(576);
  var band = 0;
  for (var i = 0; i < 576; i++) {
    while (band < 21 && i >= sfbLong[band + 1]) {
      band++;
    }
    var sf = 0;
    if (band < 21 && i < sfb21) {
      sf = scalefac[band];
      if (preflag != 0) sf += kMp3Preemphasis[band];
    }
    final sfs = (sf > 0 && sf < 16) ? kMp3SfTable[scalefacScale][sf] : 1.0;
    final v = mdct[i];
    final q = mp3Pow34(v.abs()) * base * sfs + _kQuantBias;
    final qi = q >= _kMaxQuant.toDouble() ? _kMaxQuant : q.toInt();
    ix[i] = v < 0.0 ? -qi : qi;
  }
  return ix;
}

/// Uniformly quantize the 576 MDCT lines at [globalGain] (0..255):
/// `ix = trunc(|xr|^0.75 · gain_table[gg] + 0.4054)`, clamped to ±8191, signed.
Int16List mp3QuantizeUniform(Float64List mdct, int globalGain) {
  final base = kMp3GainTable[globalGain];
  final ix = Int16List(576);
  for (var i = 0; i < 576; i++) {
    final v = mdct[i];
    final q = mp3Pow34(v.abs()) * base + _kQuantBias;
    final qi = q >= _kMaxQuant.toDouble() ? _kMaxQuant : q.toInt();
    ix[i] = v < 0.0 ? -qi : qi;
  }
  return ix;
}

/// Decoder reconstruction of the quantized lines (for round-trip verification):
/// `xr = sign(ix)·|ix|^(4/3)·gain_table[gg]^(-4/3)`.
Float64List mp3Dequantize(Int16List ix, int globalGain) {
  final invGain = math.pow(kMp3GainTable[globalGain], -4.0 / 3.0).toDouble();
  final out = Float64List(576);
  for (var i = 0; i < 576; i++) {
    final m = ix[i];
    if (m == 0) continue;
    final s = m < 0 ? -1.0 : 1.0;
    final a = (m * s).toDouble();
    out[i] = s * math.pow(a, 4.0 / 3.0).toDouble() * invGain;
  }
  return out;
}

/// Index (exclusive) after the last non-zero quantized line — the "rzero"
/// boundary glint scans for (trailing zeros are not coded).
int mp3Rzero(Int16List ix) {
  var r = ix.length;
  while (r > 0 && ix[r - 1] == 0) {
    r--;
  }
  return r;
}

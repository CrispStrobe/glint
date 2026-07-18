// lib/core/audio/mp3/mp3_psycho.dart
//
// The MP3 encoder's psychoacoustic masking model — ported from glint's REAL
// model, which lives inside quantize.cpp (`compute_src_band` / `get_mask_model`
// / `compute_band_masks` / `compute_band_tonality`), NOT the dead PsychoModel
// in psycho.cpp. It works directly on the 576 MDCT coefficients: per-sfb source
// energy → Schroeder spreading between Bark-band centres + an ATH floor → the
// allowed distortion per scalefactor band (`mask_band`, glint's xmin analog).
// This mirrors the NMR metric in glint/tests/measure_audio.py, so the shaping
// loop optimizes exactly what we measure. Pure Dart => identical native + web.

import 'dart:math' as math;
import 'dart:typed_data';

import 'mp3_granule.dart' show kMp3SfbLong;

/// Absolute threshold of hearing (dB SPL) at the 25 Bark-band centres
/// (glint `tables::ath_cb[25]`).
const List<double> _kAthCb = [
  38.0, 31.0, 22.0, 18.5, 15.5, //
  13.0, 11.0, 9.5, 8.5, 7.5, //
  6.5, 5.5, 4.5, 3.5, 3.5, //
  4.0, 5.0, 6.5, 9.0, 13.0, //
  19.0, 28.0, 42.0, 62.0, 80.0,
];

const List<int> _kRates = [44100, 48000, 32000, 22050, 24000, 16000];

/// The −14 dB flat masker offset (glint `kOffset = 10^(-14/10)`).
final double _kOffset = math.pow(10.0, -14.0 / 10.0).toDouble();

/// Per-sample-rate masking model: energy spreading gains between band centres
/// and the ATH floor in the MDCT-energy domain (glint `BandMaskModel`).
class Mp3MaskModel {
  Mp3MaskModel._(this.spread, this.ath);

  /// `spread[b][j]` — Schroeder spreading gain from masker band j to band b.
  final List<Float64List> spread;

  /// `ath[b]` — absolute-threshold floor per band (MDCT-energy domain).
  final Float64List ath;

  static final Map<int, Mp3MaskModel> _cache = {};

  /// Build (or fetch cached) the model for [srIndex] (0=44100, 1=48000, …).
  factory Mp3MaskModel.forSampleRate(int srIndex) {
    final sr = (srIndex < 0 || srIndex > 5) ? 0 : srIndex;
    return _cache.putIfAbsent(sr, () => _build(sr));
  }

  static Mp3MaskModel _build(int srIndex) {
    final srate = _kRates[srIndex].toDouble();
    final sfb = kMp3SfbLong[srIndex < 3 ? srIndex : 0];
    final z = Float64List(21);
    final ath = Float64List(21);
    for (var b = 0; b < 21; b++) {
      final fc = 0.5 * (sfb[b] + sfb[b + 1]) * (srate / 2.0) / 576.0;
      z[b] = 13.0 * math.atan(0.00076 * fc) +
          3.5 * math.atan((fc / 7500.0) * (fc / 7500.0));
      final athDb = _kAthCb[b < 24 ? b : 24];
      ath[b] =
          math.pow(10.0, (athDb - 96.0) / 10.0).toDouble() / (288.0 * 288.0);
    }
    final spread = List<Float64List>.generate(21, (_) => Float64List(21));
    for (var b = 0; b < 21; b++) {
      for (var j = 0; j < 21; j++) {
        final dz = z[b] - z[j];
        final sDb = 15.81 +
            7.5 * (dz + 0.474) -
            17.5 * math.sqrt(1.0 + (dz + 0.474) * (dz + 0.474));
        spread[b][j] = math.pow(10.0, sDb / 10.0).toDouble();
      }
    }
    return Mp3MaskModel._(spread, ath);
  }
}

/// Per-sfb source energy `src_band[21]` (glint `compute_src_band`): band 20
/// runs to 576 (it absorbs the sfb21 region for the envelope model).
Float64List mp3ComputeSrcBand(Float64List mdct, int srIndex) {
  final sfb = kMp3SfbLong[srIndex < 3 ? srIndex : 0];
  final out = Float64List(21);
  for (var b = 0; b < 21; b++) {
    final start = sfb[b];
    final end = (b < 20) ? sfb[b + 1] : 576;
    var acc = 0.0;
    for (var i = start; i < end; i++) {
      acc += mdct[i] * mdct[i];
    }
    out[b] = acc;
  }
  return out;
}

/// Per-band spectral-flatness tonality `alpha[21]` in [0,1] (glint
/// `compute_band_tonality`): 1 = tonal, 0 = noisy.
Float64List mp3ComputeBandTonality(Float64List mdct, int srIndex) {
  final sfb = kMp3SfbLong[srIndex < 3 ? srIndex : 0];
  final alpha = Float64List(21);
  for (var b = 0; b < 21; b++) {
    final start = sfb[b], end = sfb[b + 1];
    final n = end - start;
    var am = 0.0, lg = 0.0;
    for (var i = start; i < end; i++) {
      final e = mdct[i] * mdct[i] + 1e-30;
      am += e;
      lg += math.log(e);
    }
    am /= n;
    final gm = math.exp(lg / n);
    final sfmDb = 10.0 * (math.log(gm / am + 1e-30) / math.ln10);
    final a = sfmDb / -20.0;
    alpha[b] = a > 1.0 ? 1.0 : a;
  }
  return alpha;
}

/// Allowed distortion per scalefactor band `mask_band[21]` (glint
/// `compute_band_masks`): spread source energy × masker offset, floored at ATH.
/// If [alpha] is given, use per-band tonality offsets (−6 dB noisy … −18 dB
/// tonal) instead of the flat −14 dB.
Float64List mp3ComputeBandMasks(
  Float64List srcBand,
  int srIndex, {
  Float64List? alpha,
}) {
  final m = Mp3MaskModel.forSampleRate(srIndex);
  final mask = Float64List(21);
  if (alpha != null) {
    final off = Float64List(21);
    for (var j = 0; j < 21; j++) {
      off[j] = math.pow(10.0, -(6.0 + 12.0 * alpha[j]) / 10.0).toDouble();
    }
    for (var b = 0; b < 21; b++) {
      var acc = 0.0;
      for (var j = 0; j < 21; j++) {
        acc += srcBand[j] * m.spread[b][j] * off[j];
      }
      mask[b] = math.max(acc, m.ath[b]);
    }
  } else {
    for (var b = 0; b < 21; b++) {
      var acc = 0.0;
      for (var j = 0; j < 21; j++) {
        acc += srcBand[j] * m.spread[b][j];
      }
      mask[b] = math.max(acc * _kOffset, m.ath[b]);
    }
  }
  return mask;
}

// lib/core/audio/mp3/mp3_mdct.dart
//
// MP3 MDCT + windowing (long blocks) — slice 3 of the pure-Dart MP3 encoder
// port. Ported from glint's scalar `MDCT::process` + `alias_reduce_d` (MIT,
// clean-room). Per subband: overlap the 18 previous + 18 current subband
// samples into x[36], apply the fused window·cosine·(1/288) table, output 18
// MDCT lines; then the classic butterfly alias reduction across subband
// boundaries. Short (transient) blocks are a later slice — long blocks alone
// are valid MP3. Pure dart:math+typed_data => identical native + web.

import 'dart:math' as math;
import 'dart:typed_data';

class Mp3Mdct {
  /// Per-subband overlap state (32×18, flat sb*18+n).
  final Float64List _prev = Float64List(32 * 18);

  /// Fused window·cosine·(1/288) table (36×18, flat n*18+k) for the normal
  /// (block_type 0) window sin(π/36·(n+½)).
  static final Float64List _winCos = _buildWinCos();

  static Float64List _buildWinCos() {
    final t = Float64List(36 * 18);
    for (var n = 0; n < 36; n++) {
      final win = math.sin(math.pi / 36.0 * (n + 0.5));
      for (var k = 0; k < 18; k++) {
        final c =
            math.cos(math.pi / 72.0 * (2.0 * n + 19.0) * (2.0 * k + 1.0)) /
                288.0;
        t[n * 18 + k] = win * c;
      }
    }
    return t;
  }

  /// The alias-reduction butterfly coefficients (cs, ca), each length 8.
  static final (Float64List, Float64List) _alias = _buildAlias();

  static (Float64List, Float64List) _buildAlias() {
    const c = [-0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037];
    final cs = Float64List(8);
    final ca = Float64List(8);
    for (var i = 0; i < 8; i++) {
      final d = math.sqrt(1.0 + c[i] * c[i]);
      cs[i] = 1.0 / d;
      ca[i] = c[i] / d;
    }
    return (cs, ca);
  }

  /// Fused window·cos tables for the START (1) and STOP (3) transition blocks —
  /// the long window's left/right halves replaced by the hybrid shape that
  /// keeps time-domain alias cancellation valid across a long↔short switch.
  static final Float64List _winCosStart = _buildTransitionWinCos(_startWin);
  static final Float64List _winCosStop = _buildTransitionWinCos(_stopWin);

  static double _startWin(int i) {
    if (i < 18) return math.sin(math.pi / 36.0 * (i + 0.5));
    if (i < 24) return 1.0;
    if (i < 30) return math.sin(math.pi / 12.0 * (i - 18 + 0.5));
    return 0.0;
  }

  static double _stopWin(int i) {
    if (i < 6) return 0.0;
    if (i < 12) return math.sin(math.pi / 12.0 * (i - 6 + 0.5));
    if (i < 18) return 1.0;
    return math.sin(math.pi / 36.0 * (i + 0.5));
  }

  static Float64List _buildTransitionWinCos(double Function(int) win) {
    final t = Float64List(36 * 18);
    for (var n = 0; n < 36; n++) {
      final w = win(n);
      for (var k = 0; k < 18; k++) {
        final c =
            math.cos(math.pi / 72.0 * (2.0 * n + 19.0) * (2.0 * k + 1.0)) /
                288.0;
        t[n * 18 + k] = w * c;
      }
    }
    return t;
  }

  /// Short-block 12-point window (sin(π/12·(n+½))) and cosine matrix.
  static final Float64List _shortWin = _buildShortWin();
  static final Float64List _shortCos = _buildShortCos();

  static Float64List _buildShortWin() {
    final t = Float64List(12);
    for (var n = 0; n < 12; n++) {
      t[n] = math.sin(math.pi / 12.0 * (n + 0.5));
    }
    return t;
  }

  static Float64List _buildShortCos() {
    final t = Float64List(12 * 6);
    for (var n = 0; n < 12; n++) {
      for (var k = 0; k < 6; k++) {
        t[n * 6 + k] = math.cos(math.pi / 24.0 * (2 * n + 7) * (2 * k + 1));
      }
    }
    return t;
  }

  void reset() => _prev.fillRange(0, 32 * 18, 0);

  /// MDCT one granule: [subband] is 32×18 (flat sb*18+n); writes [mdct] 32×18.
  /// [blockType] 0=long, 1=start, 3=stop (all 36-point, hybrid windows). Use
  /// [processShort] for block_type 2.
  void process(Float64List subband, Float64List mdct, {int blockType = 0}) {
    final wc = blockType == 1
        ? _winCosStart
        : (blockType == 3 ? _winCosStop : _winCos);
    final x = Float64List(36);
    for (var sb = 0; sb < 32; sb++) {
      for (var n = 0; n < 18; n++) {
        x[n] = _prev[sb * 18 + n];
        x[n + 18] = subband[sb * 18 + n];
      }
      for (var k = 0; k < 18; k++) {
        var sum = 0.0;
        for (var n = 0; n < 36; n++) {
          sum += x[n] * wc[n * 18 + k];
        }
        mdct[sb * 18 + k] = sum;
      }
      for (var n = 0; n < 18; n++) {
        _prev[sb * 18 + n] = subband[sb * 18 + n];
      }
    }
  }

  /// Short-block MDCT: three 12-point transforms per subband over the 36-sample
  /// overlap buffer, offsets 6/12/18. Writes [out] as 32×3×6 (flat
  /// sb*18 + win*6 + k). Shares [_prev] with [process] so the overlap chain
  /// stays consistent across a window switch.
  void processShort(Float64List subband, Float64List out) {
    final x = Float64List(36);
    final z = Float64List(12);
    for (var sb = 0; sb < 32; sb++) {
      for (var n = 0; n < 18; n++) {
        x[n] = _prev[sb * 18 + n];
        x[n + 18] = subband[sb * 18 + n];
      }
      for (var win = 0; win < 3; win++) {
        final offset = 6 + win * 6;
        for (var n = 0; n < 12; n++) {
          z[n] = x[offset + n] * _shortWin[n];
        }
        for (var k = 0; k < 6; k++) {
          var sum = 0.0;
          for (var n = 0; n < 12; n++) {
            sum += z[n] * _shortCos[n * 6 + k];
          }
          out[sb * 18 + win * 6 + k] = sum / 96.0;
        }
      }
      for (var n = 0; n < 18; n++) {
        _prev[sb * 18 + n] = subband[sb * 18 + n];
      }
    }
  }

  /// The classic MP3 alias reduction across the 31 subband boundaries. Each
  /// butterfly is an orthonormal rotation (energy-preserving).
  void aliasReduce(Float64List mdct) {
    final cs = _alias.$1;
    final ca = _alias.$2;
    for (var sb = 0; sb < 31; sb++) {
      for (var i = 0; i < 8; i++) {
        final a = mdct[sb * 18 + (17 - i)];
        final b = mdct[(sb + 1) * 18 + i];
        mdct[sb * 18 + (17 - i)] = a * cs[i] + b * ca[i];
        mdct[(sb + 1) * 18 + i] = b * cs[i] - a * ca[i];
      }
    }
  }
}

// MP3 DSP front-end — golden regression, VALIDATED against glint.
//
// The expected values below are glint's own output (its C++ reference dumped by
// bench/glint_ref.cpp) for a fixed deterministic LCG input after 5 warmup
// granules. The pure-Dart port reproduces them to ~1e-15 (double-precision floor
// — glint uses -ffast-math/FMA, so it is machine-equivalent, not bit-identical).
// This pins that equivalence in CI without needing glint present.

import 'dart:typed_data';

import 'package:glint_audio_pure/src/mp3_mdct.dart';
import 'package:glint_audio_pure/src/mp3_subband.dart';
import 'package:test/test.dart';

class _Lcg {
  int _s = 0x12345;
  final Float32List _f = Float32List(1);
  double next() {
    _s = (_s * 1664525 + 1013904223) & 0xFFFFFFFF;
    _f[0] = (((_s >> 8) & 0xFFFF) / 65536.0) * 2.0 - 1.0; // round to float32
    return _f[0];
  }
}

void main() {
  test('subband + MDCT match glint to double precision', () {
    final sb = Mp3SubbandAnalysis();
    final mdct = Mp3Mdct();
    final subband = Float64List(32 * 18);
    final out = Float64List(32 * 18);
    final lcg = _Lcg();
    final slot = Float64List(32);
    final o = Float64List(32);

    void granule() {
      for (var ts = 0; ts < 18; ts++) {
        for (var i = 0; i < 32; i++) {
          slot[i] = lcg.next();
        }
        sb.processSlot(slot, o);
        for (var b = 0; b < 32; b++) {
          subband[b * 18 + ts] = o[b];
        }
      }
      mdct.process(subband, out);
      mdct.aliasReduce(out);
    }

    for (var g = 0; g < 5; g++) {
      granule();
    }
    granule(); // granule 5 — the one glint dumped

    // glint's exact dumped values (bench/glint_ref.cpp).
    expect(subband[0], closeTo(-2.0240591652842883, 1e-12));
    expect(subband[1], closeTo(5.1052375210973402, 1e-12));
    expect(subband[2], closeTo(0.061073758229525876, 1e-12));
    expect(out[0], closeTo(-0.027945745177679733, 1e-12));
    expect(out[1], closeTo(0.0097317020525610649, 1e-12));
    expect(out[2], closeTo(0.012313771993814619, 1e-12));
  });
}

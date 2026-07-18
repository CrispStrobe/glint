// lib/core/audio/mp3/mp3_subband.dart
//
// MP3 polyphase subband analysis filter — slice 2 of the pure-Dart MP3 encoder
// port. Ported from glint's scalar `SubbandAnalysis::process_slot` (MIT,
// clean-room). Takes 32 PCM samples per "slot" and produces 32 subband samples
// via the ISO 11172-3 polyphase filterbank: window a 512-sample FIFO, fold into
// 64 partial sums, then the cosine matrix M[i][k] = cos((2i+1)(k-16)π/64).
// Pure Dart (dart:math + dart:typed_data) => runs identically on native + web.

import 'dart:math' as math;
import 'dart:typed_data';

import 'mp3_window.dart';

class Mp3SubbandAnalysis {
  final Float64List _windowBuf = Float64List(512);
  int _windowOffset = 0;

  /// The analysis cosine matrix, M[i*64 + k] = cos((2i+1)(k-16)π/64), computed
  /// once (glint computes the same table at init).
  static final Float64List _matrix = _buildMatrix();

  static Float64List _buildMatrix() {
    final m = Float64List(32 * 64);
    for (var i = 0; i < 32; i++) {
      for (var k = 0; k < 64; k++) {
        m[i * 64 + k] = math.cos((2.0 * i + 1.0) * (k - 16.0) * math.pi / 64.0);
      }
    }
    return m;
  }

  void reset() {
    _windowBuf.fillRange(0, 512, 0);
    _windowOffset = 0;
  }

  /// Filter one slot of 32 [samples] (normalized −1..1) into 32 [out] subband
  /// values. [samples] and [out] must each hold at least 32 entries.
  void processSlot(Float64List samples, Float64List out) {
    _windowOffset = (_windowOffset - 32) & 0x1FF;
    for (var i = 0; i < 32; i++) {
      _windowBuf[(_windowOffset + 31 - i) & 0x1FF] = samples[i];
    }

    final z = Float64List(64);
    for (var j = 0; j < 64; j++) {
      var sum = 0.0;
      for (var p = 0; p < 8; p++) {
        sum += _windowBuf[(_windowOffset + j + 64 * p) & 0x1FF] *
            kMp3AnalysisWindow[j + 64 * p];
      }
      z[j] = sum;
    }

    for (var i = 0; i < 32; i++) {
      var sum = 0.0;
      final base = i * 64;
      for (var k = 0; k < 64; k++) {
        sum += z[k] * _matrix[base + k];
      }
      out[i] = sum;
    }
  }
}

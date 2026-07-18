// lib/core/audio/mp3/mp3_bitstream.dart
//
// Pure-Dart MSB-first bit writer for the MP3 encoder port (slice 1). A faithful
// port of glint's `BitstreamWriter` (MIT, clean-room) — so it stays byte-exact
// with the reference for the later DSP slices. Pure Dart => identical on native
// AND web (the whole point: compressed export must work on all platforms).

import 'dart:typed_data';

/// Accumulates bits MSB-first (big-endian) into a byte buffer.
class Mp3BitWriter {
  final BytesBuilder _out = BytesBuilder(copy: false);
  int _cache = 0; // up to 32 bits pending
  int _bitsInCache = 0;

  /// Write the low [numBits] of [value], MSB-first. Splits writes > 25 bits to
  /// stay within a 32-bit accumulator (matching glint).
  void writeBits(int value, int numBits) {
    if (numBits <= 0) return;
    if (numBits > 25) {
      final top = numBits - 25;
      writeBits(value >> top, 25);
      writeBits(value & ((1 << top) - 1), top);
      return;
    }
    _cache =
        ((_cache << numBits) | (value & ((1 << numBits) - 1))) & 0xFFFFFFFF;
    _bitsInCache += numBits;
    while (_bitsInCache >= 8) {
      _bitsInCache -= 8;
      _out.addByte((_cache >> _bitsInCache) & 0xFF);
    }
  }

  /// Flush a partial byte (right zero-padded).
  void flush() {
    if (_bitsInCache > 0) {
      _out.addByte((_cache << (8 - _bitsInCache)) & 0xFF);
      _bitsInCache = 0;
      _cache = 0;
    }
  }

  /// Pad to the next byte boundary.
  void byteAlign() => flush();

  /// Total bits written so far.
  int get bitCount => _out.length * 8 + _bitsInCache;

  /// Bytes written, rounding up a partial byte.
  int get byteCount => _out.length + (_bitsInCache > 0 ? 1 : 0);

  /// The written bytes (flushes any partial byte first).
  Uint8List takeBytes() {
    flush();
    return _out.toBytes();
  }
}

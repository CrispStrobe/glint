// lib/core/audio/mp3/mp3_decoder.dart
//
// Pure-Dart MP3 (MPEG-1 Layer III) DECODER — the inverse of mp3_encoder.dart,
// ported from glint's mp3_decoder.cpp. Scope matches our encoder: MPEG-1, long
// blocks, mono / stereo / joint(M/S). Pipeline per granule: parse side info →
// reservoir reassembly (main_data_begin) → scalefactors → Huffman → requantize
// → M/S → antialias → IMDCT (36-pt) + window + overlap + frequency inversion →
// synthesis polyphase filterbank → PCM. Pure Dart => native + web.

import 'dart:math' as math;
import 'dart:typed_data';

import 'mp3_frame.dart';
import 'mp3_granule.dart' show kMp3SfbLong;
import 'mp3_huffman.dart';
import 'mp3_huffman_tables.dart';
import 'mp3_quantize.dart'
    show kMp3Preemphasis;
import 'mp3_window.dart';

/// Decoded PCM audio from an MP3: mono/interleaved [samples] in −1..1.
class Mp3Pcm {
  Mp3Pcm(this.samples, this.channels, this.sampleRate);

  /// Interleaved float samples (−1..1), `channels` per frame.
  final Float64List samples;
  final int channels;
  final int sampleRate;
}

/// Decode a full MPEG-1 Layer III [mp3] stream (mono/stereo/joint, long blocks).
Mp3Pcm mp3Decode(Uint8List mp3) => _Mp3Decoder().decode(mp3);

const List<int> _kSlen1 = [0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4];
const List<int> _kSlen2 = [0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3];
const List<int> _kScfBands = [0, 6, 11, 16, 21];
// Alias-reduction butterfly coefficients (same as the encoder's).
const List<double> _kCi = [
  -0.6,
  -0.535,
  -0.33,
  -0.185,
  -0.095,
  -0.041,
  -0.0142,
  -0.0037,
];

class _BitReader {
  _BitReader(this.b) : limit = -1;
  final Uint8List b;
  final int limit; // byte length available (-1 = all)
  int pos = 0; // bit cursor

  int get(int n) {
    var v = 0;
    for (var i = 0; i < n; i++) {
      final byteIdx = pos >> 3;
      final by = byteIdx < b.length ? b[byteIdx] : 0;
      v = (v << 1) | ((by >> (7 - (pos & 7))) & 1);
      pos++;
    }
    return v;
  }

  int get1() => get(1);
}

/// Reverse Huffman decode maps: `(len<<24)|code -> table index`, built once.
final Map<int, Map<int, int>> _decodeMaps = {};
Map<int, int> _decodeMap(List<int> lens, List<int> codes, int id) {
  return _decodeMaps.putIfAbsent(id, () {
    final m = <int, int>{};
    for (var i = 0; i < lens.length; i++) {
      if (lens[i] > 0) m[(lens[i] << 24) | codes[i]] = i;
    }
    return m;
  });
}

class _Mp3Decoder {
  final _store = <int>[]; // bit-reservoir history (main-data bytes)
  late List<Float64List> _overlap; // [ch][576] IMDCT overlap
  late List<Float64List> _synthV; // [ch][1024] synthesis FIFO
  late List<int> _synthOff; // [ch]
  late List<Int16List> _ix; // [ch][576] quantized lines
  late List<Float64List> _xr; // [ch][576] dequantized
  late List<List<int>> _scalefacL; // [ch][23]

  void _initChannels(int nch) {
    _overlap = [for (var c = 0; c < nch; c++) Float64List(576)];
    _synthV = [for (var c = 0; c < nch; c++) Float64List(1024)];
    _synthOff = List<int>.filled(nch, 0);
    _ix = [for (var c = 0; c < nch; c++) Int16List(576)];
    _xr = [for (var c = 0; c < nch; c++) Float64List(576)];
    _scalefacL = [for (var c = 0; c < nch; c++) List<int>.filled(23, 0)];
  }

  Mp3Pcm decode(Uint8List mp3) {
    final out = <double>[];
    var off = 0;
    var sampleRate = 44100;
    var channels = 1;
    var inited = false;
    while (off + 4 <= mp3.length) {
      if (mp3[off] != 0xFF || (mp3[off + 1] & 0xE0) != 0xE0) {
        off++; // resync
        continue;
      }
      final version = (mp3[off + 1] >> 3) & 3; // 3 = MPEG-1
      final layer = (mp3[off + 1] >> 1) & 3; // 1 = Layer III
      final brIdx = (mp3[off + 2] >> 4) & 0xF;
      final srIdx = (mp3[off + 2] >> 2) & 3;
      if (version != 3 ||
          layer != 1 ||
          brIdx == 0 ||
          brIdx == 15 ||
          srIdx == 3) {
        off++;
        continue;
      }
      final kbps = kMp3Bitrates[brIdx - 1];
      final sr = kMp3SampleRates[srIdx];
      final pad = (mp3[off + 2] >> 1) & 1;
      final mode = (mp3[off + 3] >> 6) & 3;
      final modeExt = (mp3[off + 3] >> 4) & 3;
      final crc = (mp3[off + 1] & 1) == 0;
      final nch = mode == 3 ? 1 : 2;
      final frameBytes = 144 * kbps * 1000 ~/ sr + pad;
      if (off + frameBytes > mp3.length) break;
      // A Xing/Info header frame (VBR tag right after header+side info) is
      // metadata, not audio — skip it as standard decoders do.
      final sideLen = nch == 1 ? 17 : 32;
      final tagOff = off + 4 + sideLen;
      if (tagOff + 4 <= mp3.length) {
        final tag = String.fromCharCodes(mp3, tagOff, tagOff + 4);
        if (tag == 'Xing' || tag == 'Info') {
          off += frameBytes;
          continue;
        }
      }
      if (!inited) {
        _initChannels(nch);
        sampleRate = sr;
        channels = nch;
        inited = true;
      }
      final pcm =
          _decodeFrame(mp3, off, frameBytes, nch, srIdx, mode, modeExt, crc);
      if (pcm != null) out.addAll(pcm);
      off += frameBytes;
    }
    return Mp3Pcm(Float64List.fromList(out), channels, sampleRate);
  }

  /// Returns interleaved PCM for the frame's 1152 samples, or null if the frame
  /// only stashed reservoir history (stream start).
  List<double>? _decodeFrame(
    Uint8List data,
    int off,
    int frameBytes,
    int nch,
    int srIdx,
    int mode,
    int modeExt,
    bool crc,
  ) {
    final headerLen = 4 + (crc ? 2 : 0);
    final sideLen = nch == 1 ? 17 : 32;
    final si = _BitReader(
      Uint8List.sublistView(
        data,
        off + headerLen,
        off + headerLen + sideLen,
      ),
    );
    final mainDataBegin = si.get(9);
    si.get(nch == 1 ? 5 : 3); // private
    final scfsi = [
      for (var ch = 0; ch < nch; ch++) [for (var b = 0; b < 4; b++) si.get1()],
    ];
    // Two granules x nch channels of side info (long-block subset).
    final gi = [
      for (var gr = 0; gr < 2; gr++)
        [for (var ch = 0; ch < nch; ch++) _readGranuleSide(si)],
    ];

    // Bit reservoir: this frame's main data starts main_data_begin bytes back.
    final mainStart = off + headerLen + sideLen;
    final mainLen = frameBytes - headerLen - sideLen;
    if (mainDataBegin > _store.length) {
      // Not enough history (stream start) — stash and emit nothing.
      _store.addAll(data.sublist(mainStart, mainStart + mainLen));
      if (_store.length > 511) _store.removeRange(0, _store.length - 511);
      return null;
    }
    final body = <int>[
      ..._store.sublist(_store.length - mainDataBegin),
      ...data.sublist(mainStart, mainStart + mainLen),
    ];
    // Update history for the next frame.
    _store.addAll(data.sublist(mainStart, mainStart + mainLen));
    if (_store.length > 511) _store.removeRange(0, _store.length - 511);

    final br = _BitReader(Uint8List.fromList(body));
    final sfb = kMp3SfbLong[srIdx];
    final pcm = List<double>.filled(1152 * nch, 0.0);

    for (var gr = 0; gr < 2; gr++) {
      for (var ch = 0; ch < nch; ch++) {
        final g = gi[gr][ch];
        final part2Start = br.pos;
        _readScalefactors(br, g, ch, gr, scfsi[ch]);
        _readHuffman(br, g, ch, sfb, part2Start);
      }
      for (var ch = 0; ch < nch; ch++) {
        _requantize(gi[gr][ch], ch, sfb);
      }
      if (nch == 2 && mode == 1 && (modeExt & 2) != 0) {
        _msStereo(); // joint M/S → L/R
      }
      for (var ch = 0; ch < nch; ch++) {
        _antialias(ch);
        _imdctGranule(ch);
        _synthGranule(ch, gr, pcm, nch);
      }
    }
    return pcm;
  }

  _Gran _readGranuleSide(_BitReader si) {
    final g = _Gran();
    g.part23Length = si.get(12);
    g.bigValues = si.get(9);
    g.globalGain = si.get(8);
    g.scalefacCompress = si.get(4);
    final windowSwitching = si.get1();
    if (windowSwitching == 1) {
      // Short/window-switching blocks are outside our long-only scope; read the
      // fields to stay aligned, but decode as a (silent) long block.
      si.get(2); // block_type
      si.get1(); // mixed
      si.get(5);
      si.get(5);
      for (var w = 0; w < 3; w++) {
        si.get(3);
      }
      g.tableSelect = [0, 0, 0];
      g.region0Count = 7;
      g.region1Count = 13;
    } else {
      g.tableSelect = [si.get(5), si.get(5), si.get(5)];
      g.region0Count = si.get(4);
      g.region1Count = si.get(3);
    }
    g.preflag = si.get1();
    g.scalefacScale = si.get1();
    g.count1Table = si.get1();
    return g;
  }

  void _readScalefactors(
    _BitReader br,
    _Gran g,
    int ch,
    int gr,
    List<int> scfsi,
  ) {
    final s1 = _kSlen1[g.scalefacCompress];
    final s2 = _kSlen2[g.scalefacCompress];
    final sf = _scalefacL[ch];
    for (var grp = 0; grp < 4; grp++) {
      final slen = grp < 2 ? s1 : s2;
      if (gr == 1 && scfsi[grp] == 1) continue; // reuse granule 0's
      for (var b = _kScfBands[grp]; b < _kScfBands[grp + 1]; b++) {
        sf[b] = slen > 0 ? br.get(slen) : 0;
      }
    }
    sf[21] = 0;
    sf[22] = 0;
  }

  void _readHuffman(
    _BitReader br,
    _Gran g,
    int ch,
    List<int> sfb,
    int part2Start,
  ) {
    final ix = _ix[ch];
    for (var i = 0; i < 576; i++) {
      ix[i] = 0;
    }
    final part23End = part2Start + g.part23Length;
    var r0 = g.region0Count + 1;
    var r1 = r0 + g.region1Count + 1;
    if (r0 > 22) r0 = 22;
    if (r1 > 22) r1 = 22;
    final regEnd = [sfb[r0], sfb[r1], 576];
    final nlines = g.bigValues * 2;
    var i = 0;
    for (var r = 0; r < 3 && i < nlines; r++) {
      final tsel = g.tableSelect[r];
      while (i < nlines && i < regEnd[r]) {
        if (tsel == 0) {
          ix[i] = 0;
          ix[i + 1] = 0;
        } else {
          final xy = _decodePair(br, tsel);
          ix[i] = xy.$1;
          ix[i + 1] = xy.$2;
        }
        i += 2;
      }
    }
    // count1 quads until the bit budget is spent.
    final lens = g.count1Table == 1 ? kHT33Len : kHT32Len;
    final codes = g.count1Table == 1 ? kHT33Code : kHT32Code;
    final id = g.count1Table == 1 ? 33 : 32;
    final map = _decodeMap(lens, codes, id);
    while (br.pos < part23End && i <= 572) {
      final idx = _decodeCode(br, map);
      if (idx < 0) break;
      final q = [(idx >> 3) & 1, (idx >> 2) & 1, (idx >> 1) & 1, idx & 1];
      for (var k = 0; k < 4; k++) {
        var v = q[k];
        if (v != 0) {
          if (br.pos >= part23End) {
            v = 0;
          } else if (br.get1() == 1) {
            v = -1;
          }
        }
        ix[i + k] = v;
      }
      i += 4;
    }
    if (br.pos > part23End) {
      for (var k = i - 4; k < i && k >= 0; k++) {
        ix[k] = 0;
      }
    }
    br.pos = part23End;
  }

  (int, int) _decodePair(_BitReader br, int tableId) {
    final ht = getHuffTable(tableId);
    final map = _decodeMap(ht.len, ht.code, tableId);
    final idx = _decodeCode(br, map);
    if (idx < 0) return (0, 0);
    var ax = idx ~/ ht.xlen;
    var ay = idx % ht.xlen;
    if (ax == 15 && ht.linbits > 0) ax += br.get(ht.linbits);
    var x = ax;
    if (ax != 0 && br.get1() == 1) x = -x;
    if (ay == 15 && ht.linbits > 0) ay += br.get(ht.linbits);
    var y = ay;
    if (ay != 0 && br.get1() == 1) y = -y;
    return (x, y);
  }

  int _decodeCode(_BitReader br, Map<int, int> map) {
    var code = 0, len = 0;
    while (len < 20) {
      code = (code << 1) | br.get1();
      len++;
      final idx = map[(len << 24) | code];
      if (idx != null) return idx;
    }
    return -1;
  }

  void _requantize(_Gran g, int ch, List<int> sfb) {
    final ix = _ix[ch];
    final xr = _xr[ch];
    final sf = _scalefacL[ch];
    final global = math.pow(2.0, 0.25 * (g.globalGain - 210)).toDouble();
    final sfMult = g.scalefacScale == 1 ? 1.0 : 0.5;
    var band = 0;
    for (var i = 0; i < 576; i++) {
      while (band < 21 && i >= sfb[band + 1]) {
        band++;
      }
      final s = band < 21 ? sf[band] : 0;
      final pre = (g.preflag == 1 && band < 21) ? kMp3Preemphasis[band] : 0;
      final m = global * math.pow(2.0, -sfMult * (s + pre)).toDouble();
      final a = ix[i];
      final v = math.pow(a.abs(), 4.0 / 3.0).toDouble();
      xr[i] = (a < 0 ? -v : v) * m;
    }
  }

  void _msStereo() {
    final l = _xr[0];
    final r = _xr[1];
    for (var i = 0; i < 576; i++) {
      final m = l[i];
      final s = r[i];
      l[i] = (m + s) * 0.7071067811865476;
      r[i] = (m - s) * 0.7071067811865476;
    }
  }

  void _antialias(int ch) {
    final xr = _xr[ch];
    for (var sb = 1; sb < 32; sb++) {
      for (var i = 0; i < 8; i++) {
        final cs = 1.0 / math.sqrt(1.0 + _kCi[i] * _kCi[i]);
        final ca = _kCi[i] * cs;
        final lo = xr[sb * 18 - 1 - i];
        final hi = xr[sb * 18 + i];
        xr[sb * 18 - 1 - i] = lo * cs - hi * ca;
        xr[sb * 18 + i] = hi * cs + lo * ca;
      }
    }
  }

  void _imdctGranule(int ch) {
    final xr = _xr[ch];
    final overlap = _overlap[ch];
    final xIn = Float64List(18);
    final out = Float64List(36);
    for (var sb = 0; sb < 32; sb++) {
      for (var k = 0; k < 18; k++) {
        xIn[k] = xr[sb * 18 + k];
      }
      _imdct36(xIn, out);
      for (var k = 0; k < 36; k++) {
        out[k] *= _winLong(k);
      }
      for (var k = 0; k < 18; k++) {
        final v = out[k] + overlap[sb * 18 + k];
        overlap[sb * 18 + k] = out[18 + k];
        xr[sb * 18 + k] = ((sb & 1) != 0 && (k & 1) != 0) ? -v : v;
      }
    }
  }

  void _synthGranule(int ch, int nGran, List<double> pcm, int nch) {
    final v = _synthV[ch];
    final s = Float64List(32);
    final xr = _xr[ch];
    for (var slot = 0; slot < 18; slot++) {
      for (var sb = 0; sb < 32; sb++) {
        s[sb] = xr[sb * 18 + slot];
      }
      _synthOff[ch] = (_synthOff[ch] - 64) & 1023;
      for (var i = 0; i < 64; i++) {
        var sum = 0.0;
        for (var k = 0; k < 32; k++) {
          sum += s[k] * math.cos((16 + i) * (2 * k + 1) * math.pi / 64.0);
        }
        v[(_synthOff[ch] + i) & 1023] = sum;
      }
      for (var j = 0; j < 32; j++) {
        var sum = 0.0;
        for (var i = 0; i < 16; i++) {
          final vidx =
              (_synthOff[ch] + 128 * (i ~/ 2) + ((i & 1) != 0 ? 96 : 0) + j) &
                  1023;
          sum += kMp3AnalysisWindow[j + 32 * i] * v[vidx];
        }
        final outIdx = (nGran * 18 + slot) * 32 + j;
        pcm[outIdx * nch + ch] = sum;
      }
    }
  }
}

double _winLong(int i) => math.sin(math.pi / 36.0 * (i + 0.5));

/// 36-point IMDCT: `x[i] = Σ_k X[k]·cos(π/72·(2i+1+18)(2k+1))`.
void _imdct36(Float64List x, Float64List out) {
  for (var i = 0; i < 36; i++) {
    var s = 0.0;
    for (var k = 0; k < 18; k++) {
      s += x[k] * math.cos(math.pi / 72.0 * (2 * i + 1 + 18) * (2 * k + 1));
    }
    out[i] = s;
  }
}

class _Gran {
  int part23Length = 0;
  int bigValues = 0;
  int globalGain = 0;
  int scalefacCompress = 0;
  List<int> tableSelect = const [0, 0, 0];
  int region0Count = 0;
  int region1Count = 0;
  int preflag = 0;
  int scalefacScale = 0;
  int count1Table = 0;
}

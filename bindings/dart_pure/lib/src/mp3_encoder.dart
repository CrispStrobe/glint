// lib/core/audio/mp3/mp3_encoder.dart
//
// Top-level pure-Dart MP3 encoder — slice 5c/6 of the port. Assembles a
// complete, decodable MPEG-1 Layer III (mono) CBR stream from PCM: per granule
// runs subband → MDCT → alias → a rate-control loop that picks global_gain so
// the Huffman data fits the frame budget → side-info + Huffman main data. This
// first cut uses zero scalefactors (global_gain only) and no bit reservoir
// (main_data_begin = 0) — valid MP3, simpler than glint's reservoir path.
// Pure Dart => runs identically on native AND web.

import 'dart:typed_data';

import 'mp3_bitstream.dart';
import 'mp3_frame.dart';
import 'mp3_granule.dart';
import 'mp3_mdct.dart';
import 'mp3_reservoir.dart';
import 'mp3_shape.dart';
import 'mp3_subband.dart';

/// Mono side-info size for MPEG-1 Layer III: 17 bytes.
const int _kMonoSideInfoBytes = 17;

/// Encode mono [pcm] (−1..1) to an MPEG-1 Layer III (mono, CBR) `.mp3`.
/// [bitrate] kbps must be a valid MPEG-1 rate; [sampleRate] one of 44100/48000/
/// 32000.
Uint8List mp3EncodeMono(
  Float64List pcm, {
  int sampleRate = 44100,
  int bitrate = 128,
}) {
  final srIndex = mp3SampleRateIndex(sampleRate);
  if (srIndex < 0) {
    throw ArgumentError('unsupported sample rate: $sampleRate');
  }
  if (mp3BitrateIndex(bitrate) == 0) {
    throw ArgumentError('unsupported bitrate: $bitrate');
  }

  final sb = Mp3SubbandAnalysis();
  final mdct = Mp3Mdct();
  final subband = Float64List(576);
  final mdctBuf = Float64List(576);
  final slot = Float64List(32);
  final so = Float64List(32);
  final out = BytesBuilder(copy: false);
  // Bit reservoir: main data spills across frame slots so a hard granule can
  // spend more than one slot (finer shaping) while easy granules bank the rest.
  // (A CBR gain-floor anchor was tried and measured WORSE — it coarsened easy
  // granules faster than shaping could compensate; hard granules borrowing the
  // naturally-banked surplus is the win. NMR −6.7 → −7.1 dB on speech.)
  final reservoir = Mp3ReservoirStream(511);

  final brBps = bitrate * 1000;
  final frameBase = 144 * brBps ~/ sampleRate;
  final rem = (144 * brBps) % sampleRate;
  var padAcc = 0;

  var pos = 0;
  while (pos < pcm.length) {
    // Padding bit averages the bitrate across frames (fractional frame size).
    padAcc += rem;
    final pad = padAcc >= sampleRate;
    if (pad) padAcc -= sampleRate;
    final frameSize = frameBase + (pad ? 1 : 0);
    final thisFrameBits = (frameSize - 4 - _kMonoSideInfoBytes) * 8;
    // Borrow up to one extra slot from the reservoir for this frame.
    final mdb = reservoir.mainDataBegin();
    final borrow = 8 * mdb < thisFrameBits ? 8 * mdb : thisFrameBits;
    final availPer = (thisFrameBits + borrow) ~/ 2;

    final gr = <Mp3GranuleInfo>[];
    for (var g = 0; g < 2; g++) {
      for (var ts = 0; ts < 18; ts++) {
        for (var i = 0; i < 32; i++) {
          final idx = pos + g * 576 + ts * 32 + i;
          slot[i] = idx < pcm.length ? pcm[idx] : 0.0;
        }
        sb.processSlot(slot, so);
        for (var b = 0; b < 32; b++) {
          // MPEG frequency inversion: negate odd subbands at odd time slots.
          // glint's encoder MDCT (process_strided) folds this in; our
          // Mp3Mdct.process (== glint's plain process()) does not, so we must
          // pre-invert here or the decoder's synthesis reconstructs the odd
          // subbands spectrally flipped (broadband audio scrambles; band-0
          // tones are unaffected — the symptom that localized this).
          final v = so[b];
          subband[b * 18 + ts] = ((b & 1) != 0 && (ts & 1) != 0) ? -v : v;
        }
      }
      mdct.process(subband, mdctBuf);
      mdct.aliasReduce(mdctBuf);
      // The rate/distortion + psychoacoustic shaping loop (mp3_shape.dart):
      // picks global_gain + per-sfb scalefactors so quantization noise sits
      // under the masking threshold, not flat across the spectrum.
      gr.add(mp3QuantizeGranule(mdctBuf, availPer, srIndex));
    }

    // Header + side info (mono, byte-aligned = 21 bytes) carrying main_data_begin.
    final si = Mp3BitWriter();
    _writeHeader(si, bitrate, sampleRate, pad);
    si.writeBits(mdb, 9); // main_data_begin (reservoir back-pointer)
    si.writeBits(0, 5); // private bits (mono)
    si.writeBits(0, 4); // scfsi (0 = no scalefactor sharing across granules)
    for (var g = 0; g < 2; g++) {
      _writeGranuleSideInfo(si, gr[g]);
    }
    si.byteAlign();
    final headerSi = si.takeBytes();

    // Main data (byte-aligned): per granule scalefactors then Huffman.
    final md = Mp3BitWriter();
    for (var g = 0; g < 2; g++) {
      _writeScalefactors(md, gr[g]);
      mp3EncodeGranule(md, gr[g].ix, gr[g].regions, srIndex);
    }
    md.byteAlign();
    reservoir.addFrame(
      headerSi,
      md.takeBytes(),
      frameSize - headerSi.length,
      out,
    );
    pos += 1152;
  }
  reservoir.flush(out);
  return out.toBytes();
}

/// glint's 4-bit scalefac_compress → (slen1, slen2) — bit widths of the two
/// scalefactor groups (bands 0–10 and 11–20) transmitted in the main data.
const List<List<int>> _kSlenTable = [
  [0, 0], [0, 1], [0, 2], [0, 3], [3, 0], [1, 1], [1, 2], [1, 3], //
  [2, 1], [2, 2], [2, 3], [3, 1], [3, 2], [3, 3], [4, 2], [4, 3],
];

/// Emit one granule's scalefactors (part2), MPEG-1 long block: slen1 bits for
/// bands 0–10, slen2 bits for bands 11–20 (widths from scalefac_compress).
void _writeScalefactors(Mp3BitWriter w, Mp3GranuleInfo gi) {
  final slen1 = _kSlenTable[gi.scalefacCompress][0];
  final slen2 = _kSlenTable[gi.scalefacCompress][1];
  if (slen1 > 0) {
    for (var b = 0; b < 11; b++) {
      w.writeBits(gi.scalefac[b], slen1);
    }
  }
  if (slen2 > 0) {
    for (var b = 11; b < 21; b++) {
      w.writeBits(gi.scalefac[b], slen2);
    }
  }
}

void _writeHeader(Mp3BitWriter w, int bitrate, int sampleRate, bool pad) {
  w
    ..writeBits(0x7FF, 11)
    ..writeBits(0x3, 2) // MPEG-1
    ..writeBits(0x1, 2) // Layer III
    ..writeBits(1, 1) // no CRC
    ..writeBits(mp3BitrateIndex(bitrate), 4)
    ..writeBits(mp3SampleRateIndex(sampleRate), 2)
    ..writeBits(pad ? 1 : 0, 1)
    ..writeBits(0, 1) // private
    ..writeBits(Mp3ChannelMode.mono.index, 2)
    ..writeBits(0, 2) // mode extension
    ..writeBits(0, 1) // copyright
    ..writeBits(1, 1) // original
    ..writeBits(0, 2); // emphasis
}

/// glint's `write_granule_side_info`, block_type 0 (long): now carries the
/// real scalefac_compress / preflag / scalefac_scale from the shaping loop.
void _writeGranuleSideInfo(Mp3BitWriter w, Mp3GranuleInfo gi) {
  final r = gi.regions;
  w
    ..writeBits(gi.part23Length, 12)
    ..writeBits(r.bigValues, 9)
    ..writeBits(gi.globalGain, 8)
    ..writeBits(gi.scalefacCompress, 4)
    ..writeBits(0, 1) // window_switching_flag = 0
    ..writeBits(r.tableSelect[0], 5)
    ..writeBits(r.tableSelect[1], 5)
    ..writeBits(r.tableSelect[2], 5)
    ..writeBits(r.region0Count, 4)
    ..writeBits(r.region1Count, 3)
    ..writeBits(gi.preflag, 1)
    ..writeBits(gi.scalefacScale, 1)
    ..writeBits(r.count1Table, 1);
}

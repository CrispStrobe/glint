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

/// Encode mono [pcm] (−1..1) to an MPEG-1 Layer III (mono, CBR) `.mp3`.
/// [bitrate] kbps must be a valid MPEG-1 rate; [sampleRate] one of 44100/48000/
/// 32000.
Uint8List mp3EncodeMono(
  Float64List pcm, {
  int sampleRate = 44100,
  int bitrate = 128,
}) =>
    _mp3Encode([pcm], sampleRate, bitrate, Mp3ChannelMode.mono);

/// Encode stereo [left]/[right] (−1..1) to an MPEG-1 Layer III (stereo, CBR)
/// `.mp3`. Channels may differ in length (the shorter is zero-padded).
Uint8List mp3EncodeStereo(
  Float64List left,
  Float64List right, {
  int sampleRate = 44100,
  int bitrate = 128,
}) =>
    _mp3Encode([left, right], sampleRate, bitrate, Mp3ChannelMode.stereo);

/// Channel-general MPEG-1 Layer III encoder (mono or L/R stereo), CBR, with the
/// bit reservoir. Each channel runs its own subband+MDCT; per frame the budget
/// (one slot + up to one reservoir slot) is split evenly across the
/// 2 granules x nch channels.
Uint8List _mp3Encode(
  List<Float64List> channels,
  int sampleRate,
  int bitrate,
  Mp3ChannelMode mode,
) {
  final srIndex = mp3SampleRateIndex(sampleRate);
  if (srIndex < 0) {
    throw ArgumentError('unsupported sample rate: $sampleRate');
  }
  if (mp3BitrateIndex(bitrate) == 0) {
    throw ArgumentError('unsupported bitrate: $bitrate');
  }
  final nch = channels.length;
  // MPEG-1 side info: 17 bytes mono, 32 bytes stereo.
  final sideInfoBytes = nch == 1 ? 17 : 32;
  var nSamples = 0;
  for (final c in channels) {
    if (c.length > nSamples) nSamples = c.length;
  }

  final sb = [for (var c = 0; c < nch; c++) Mp3SubbandAnalysis()];
  final mdct = [for (var c = 0; c < nch; c++) Mp3Mdct()];
  final subband = Float64List(576);
  final mdctBuf = Float64List(576);
  final slot = Float64List(32);
  final so = Float64List(32);
  final out = BytesBuilder(copy: false);
  // Bit reservoir: main data spills across frame slots so a hard granule can
  // spend more than one slot (finer shaping) while easy granules bank the rest.
  final reservoir = Mp3ReservoirStream(511);

  final brBps = bitrate * 1000;
  final frameBase = 144 * brBps ~/ sampleRate;
  final rem = (144 * brBps) % sampleRate;
  var padAcc = 0;

  var pos = 0;
  while (pos < nSamples) {
    // Padding bit averages the bitrate across frames (fractional frame size).
    padAcc += rem;
    final pad = padAcc >= sampleRate;
    if (pad) padAcc -= sampleRate;
    final frameSize = frameBase + (pad ? 1 : 0);
    final thisFrameBits = (frameSize - 4 - sideInfoBytes) * 8;
    final mdb = reservoir.mainDataBegin();
    final borrow = 8 * mdb < thisFrameBits ? 8 * mdb : thisFrameBits;
    final availPer = (thisFrameBits + borrow) ~/ (2 * nch);

    // Quantize 2 granules x nch channels.
    final gr = List.generate(2, (_) => <Mp3GranuleInfo>[]);
    for (var g = 0; g < 2; g++) {
      for (var ch = 0; ch < nch; ch++) {
        final pcm = channels[ch];
        for (var ts = 0; ts < 18; ts++) {
          for (var i = 0; i < 32; i++) {
            final idx = pos + g * 576 + ts * 32 + i;
            slot[i] = idx < pcm.length ? pcm[idx] : 0.0;
          }
          sb[ch].processSlot(slot, so);
          for (var b = 0; b < 32; b++) {
            // MPEG frequency inversion: negate odd subbands at odd time slots
            // (see the localization note in git history — without it the
            // decoder reconstructs odd subbands spectrally flipped).
            final v = so[b];
            subband[b * 18 + ts] = ((b & 1) != 0 && (ts & 1) != 0) ? -v : v;
          }
        }
        mdct[ch].process(subband, mdctBuf);
        mdct[ch].aliasReduce(mdctBuf);
        gr[g].add(mp3QuantizeGranule(mdctBuf, availPer, srIndex));
      }
    }

    // Header + side info carrying main_data_begin.
    final si = Mp3BitWriter();
    _writeHeader(si, bitrate, sampleRate, pad, mode);
    si.writeBits(mdb, 9); // main_data_begin (reservoir back-pointer)
    si.writeBits(0, nch == 1 ? 5 : 3); // private bits
    si.writeBits(0, nch * 4); // scfsi (no scalefactor sharing)
    for (var g = 0; g < 2; g++) {
      for (var ch = 0; ch < nch; ch++) {
        _writeGranuleSideInfo(si, gr[g][ch]);
      }
    }
    si.byteAlign();
    final headerSi = si.takeBytes();

    // Main data (byte-aligned): per granule, per channel, scalefactors + Huffman.
    final md = Mp3BitWriter();
    for (var g = 0; g < 2; g++) {
      for (var ch = 0; ch < nch; ch++) {
        _writeScalefactors(md, gr[g][ch]);
        mp3EncodeGranule(md, gr[g][ch].ix, gr[g][ch].regions, srIndex);
      }
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

void _writeHeader(
  Mp3BitWriter w,
  int bitrate,
  int sampleRate,
  bool pad,
  Mp3ChannelMode mode,
) {
  w
    ..writeBits(0x7FF, 11)
    ..writeBits(0x3, 2) // MPEG-1
    ..writeBits(0x1, 2) // Layer III
    ..writeBits(1, 1) // no CRC
    ..writeBits(mp3BitrateIndex(bitrate), 4)
    ..writeBits(mp3SampleRateIndex(sampleRate), 2)
    ..writeBits(pad ? 1 : 0, 1)
    ..writeBits(0, 1) // private
    ..writeBits(mode.index, 2)
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

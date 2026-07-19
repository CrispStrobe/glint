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
import 'mp3_short.dart';
import 'mp3_subband.dart';

/// Encode mono [pcm] (−1..1) to an MPEG-1 Layer III (mono, CBR) `.mp3`.
/// [bitrate] kbps must be a valid MPEG-1 rate; [sampleRate] one of 44100/48000/
/// 32000.
/// [shortBlocks] (opt-in) enables transient/short-block switching to cut
/// pre-echo on percussive content. Default off keeps the standard long-block
/// path (byte-identical to a plain encode). When on, a transient scheduler emits
/// start→short→stop granules over attacks; verified against our own decoder and
/// the ffmpeg oracle (>69 dB, beating long-only on transients).
Uint8List mp3EncodeMono(
  Float64List pcm, {
  int sampleRate = 44100,
  int bitrate = 128,
  bool shortBlocks = false,
}) =>
    _mp3Encode(
      [pcm],
      sampleRate,
      bitrate,
      Mp3ChannelMode.mono,
      shortBlocks: shortBlocks,
    );

/// Encode stereo [left]/[right] (−1..1) to an MPEG-1 Layer III (stereo, CBR)
/// `.mp3`. Channels may differ in length (the shorter is zero-padded).
/// [shortBlocks] (opt-in, default off) enables per-channel transient/short-block
/// switching to cut pre-echo on percussive content; off is byte-identical.
Uint8List mp3EncodeStereo(
  Float64List left,
  Float64List right, {
  int sampleRate = 44100,
  int bitrate = 128,
  bool shortBlocks = false,
}) =>
    _mp3Encode(
      [left, right],
      sampleRate,
      bitrate,
      Mp3ChannelMode.stereo,
      shortBlocks: shortBlocks,
    );

/// Encode **joint (mid/side) stereo** — usually smaller/cleaner than plain
/// stereo for correlated material, since the side channel of centred content
/// needs far fewer bits. Decoders reconstruct L/R transparently.
/// [shortBlocks] (opt-in, default off) enables per-channel transient/short-block
/// switching; off is byte-identical.
Uint8List mp3EncodeJointStereo(
  Float64List left,
  Float64List right, {
  int sampleRate = 44100,
  int bitrate = 128,
  bool shortBlocks = false,
}) =>
    _mp3Encode(
      [left, right],
      sampleRate,
      bitrate,
      Mp3ChannelMode.jointStereo,
      shortBlocks: shortBlocks,
    );

/// 1/√2, the mid/side scale factor (glint `kInvSqrt2`).
const double _kInvSqrt2 = 0.7071067811865476;

/// Test hook: if non-null, each encoded granule appends
/// `[blockType, bigValues, globalGain, part23Length, nonzeroIx]`.
List<List<int>>? mp3EncoderDebugLog;

/// Test hook: full ix per granule.
List<Int16List>? mp3EncoderIxLog;

/// VBR target global_gain per quality (glint `vbr_target_gain`): 0 = best
/// (finest), 9 = smallest. Each step is ~1.1 dB of quantization noise.
const List<int> _kVbrTargetGain = [
  134,
  140,
  144,
  148,
  152,
  156,
  161,
  166,
  172,
  178,
];

/// Encode mono [pcm] to a **variable-bitrate** MP3 at constant [quality] (0 =
/// best/largest … 9 = smallest). Each frame picks the smallest bitrate that
/// holds its content, so quiet passages shrink and busy ones keep quality.
Uint8List mp3EncodeMonoVbr(
  Float64List pcm, {
  int sampleRate = 44100,
  int quality = 4,
}) =>
    _mp3Encode(
      [pcm],
      sampleRate,
      320,
      Mp3ChannelMode.mono,
      vbrQuality: quality,
    );

/// Encode stereo [left]/[right] to a variable-bitrate MP3 at constant [quality].
Uint8List mp3EncodeStereoVbr(
  Float64List left,
  Float64List right, {
  int sampleRate = 44100,
  int quality = 4,
}) =>
    _mp3Encode(
      [left, right],
      sampleRate,
      320,
      Mp3ChannelMode.stereo,
      vbrQuality: quality,
    );

/// Channel-general MPEG-1 Layer III encoder (mono or L/R stereo). CBR uses the
/// bit reservoir; VBR ([vbrQuality] set) writes self-contained frames, each at
/// the smallest bitrate that fits, quantized to the quality's target gain.
/// Each channel runs its own subband+MDCT; per frame the budget is split evenly
/// across the 2 granules × nch channels.
Uint8List _mp3Encode(
  List<Float64List> channels,
  int sampleRate,
  int bitrate,
  Mp3ChannelMode mode, {
  int? vbrQuality,
  bool shortBlocks = false,
}) {
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
  // Joint (M/S) stereo: transform L/R subbands to mid/side; mode_ext = 2.
  final joint = mode == Mp3ChannelMode.jointStereo && nch == 2;
  final modeExt = joint ? 2 : 0;
  final vbr = vbrQuality != null;
  final gainFloor = vbr ? _kVbrTargetGain[vbrQuality.clamp(0, 9)] : 0;
  // VBR quantizes against the largest possible frame; the gain floor sets the
  // actual quality and the per-frame bitrate is chosen to fit.
  final vbrCeilBits = (mp3FrameSize(320, sampleRate) - 4 - sideInfoBytes) * 8;
  var nSamples = 0;
  for (final c in channels) {
    if (c.length > nSamples) nSamples = c.length;
  }

  final sb = [for (var c = 0; c < nch; c++) Mp3SubbandAnalysis()];
  final mdct = [for (var c = 0; c < nch; c++) Mp3Mdct()];
  // Raw (pre-frequency-inversion) subband per channel, held so M/S can combine
  // both channels of a granule before the MDCT.
  final rawSub = [for (var c = 0; c < nch; c++) Float64List(576)];
  final subband = Float64List(576);
  final mdctBuf = Float64List(576);
  final slot = Float64List(32);
  final so = Float64List(32);
  final out = BytesBuilder(copy: false);
  // Bit reservoir: main data spills across frame slots so a hard granule can
  // spend more than one slot (finer shaping) while easy granules bank the rest.
  final reservoir = Mp3ReservoirStream(511);
  final frameOffsets =
      <int>[]; // VBR: byte offset of each audio frame (for TOC)
  // Opt-in transient/short blocks (mono OR stereo/joint): one scheduler per
  // channel (each keeps its own long→start→short→stop→long chain) + scratch.
  final useShort = shortBlocks;
  final schedulers =
      useShort ? [for (var c = 0; c < nch; c++) Mp3BlockScheduler()] : null;
  final shortBuf = Float64List(576);
  // Raw (pre-inversion) subband per (granule, channel), held so M/S can combine
  // channels and the transient scheduler can look across both granules first.
  final rawSub2 = useShort
      ? [
          for (var g = 0; g < 2; g++)
            [for (var c = 0; c < nch; c++) Float64List(576)],
        ]
      : const <List<Float64List>>[];

  final brBps = bitrate * 1000;
  final frameBase = 144 * brBps ~/ sampleRate;
  final rem = (144 * brBps) % sampleRate;
  var padAcc = 0;

  var pos = 0;
  while (pos < nSamples) {
    int mdb;
    int availPer;
    var pad = false;
    if (vbr) {
      mdb = 0; // VBR frames are self-contained
      availPer = vbrCeilBits ~/ (2 * nch);
    } else {
      // Padding bit averages the bitrate across frames (fractional frame size).
      padAcc += rem;
      pad = padAcc >= sampleRate;
      if (pad) padAcc -= sampleRate;
      final frameSize = frameBase + (pad ? 1 : 0);
      final thisFrameBits = (frameSize - 4 - sideInfoBytes) * 8;
      mdb = reservoir.mainDataBegin();
      final borrow = 8 * mdb < thisFrameBits ? 8 * mdb : thisFrameBits;
      availPer = (thisFrameBits + borrow) ~/ (2 * nch);
    }

    // Quantize 2 granules x nch channels.
    final gr = List.generate(2, (_) => <Mp3GranuleInfo>[]);
    if (useShort) {
      // Transient path (mono or stereo/joint). Compute the raw subband + energy
      // for BOTH granules and ALL channels first (the scheduler looks across the
      // frame), apply M/S per granule if joint, then schedule block types per
      // channel and MDCT+quantize per type — in granule order so each channel's
      // MDCT overlap chain stays consistent.
      final energy = [for (var g = 0; g < 2; g++) List.filled(nch, 0.0)];
      for (var g = 0; g < 2; g++) {
        for (var ch = 0; ch < nch; ch++) {
          final pcm = channels[ch];
          var e = 0.0;
          for (var ts = 0; ts < 18; ts++) {
            for (var i = 0; i < 32; i++) {
              final idx = pos + g * 576 + ts * 32 + i;
              slot[i] = idx < pcm.length ? pcm[idx] : 0.0;
            }
            sb[ch].processSlot(slot, so);
            for (var b = 0; b < 32; b++) {
              final v = so[b];
              e += v * v;
              rawSub2[g][ch][b * 18 + ts] = v;
            }
          }
          energy[g][ch] = e;
        }
        // M/S on the subband samples (per granule), exactly like the long path;
        // the SIDE channel is not psy-shaped below.
        if (joint) {
          for (var k = 0; k < 576; k++) {
            final l = rawSub2[g][0][k];
            final r = rawSub2[g][1][k];
            rawSub2[g][0][k] = (l + r) * _kInvSqrt2;
            rawSub2[g][1][k] = (l - r) * _kInvSqrt2;
          }
        }
      }
      // Each channel schedules its own frame chain over both granules.
      final types = [
        for (var ch = 0; ch < nch; ch++)
          schedulers![ch].schedule([energy[0][ch], energy[1][ch]]),
      ];
      for (var g = 0; g < 2; g++) {
        for (var ch = 0; ch < nch; ch++) {
          final bt = types[ch][g];
          for (var b = 0; b < 32; b++) {
            for (var ts = 0; ts < 18; ts++) {
              final v = rawSub2[g][ch][b * 18 + ts];
              subband[b * 18 + ts] = ((b & 1) != 0 && (ts & 1) != 0) ? -v : v;
            }
          }
          if (bt == 2) {
            mdct[ch].processShort(subband, shortBuf);
            final flat = mp3ReorderShort(shortBuf, srIndex);
            gr[g].add(mp3QuantizeGranuleWs(flat, availPer, srIndex, 2));
          } else if (bt == 1 || bt == 3) {
            mdct[ch].process(subband, mdctBuf, blockType: bt);
            mdct[ch].aliasReduce(mdctBuf);
            gr[g].add(mp3QuantizeGranuleWs(mdctBuf, availPer, srIndex, bt));
          } else {
            mdct[ch].process(subband, mdctBuf);
            mdct[ch].aliasReduce(mdctBuf);
            gr[g].add(
              mp3QuantizeGranule(
                mdctBuf,
                availPer,
                srIndex,
                gainFloor: gainFloor,
                vbrShaping: vbr,
                allowPsy: !(joint && ch == 1),
              ),
            );
          }
        }
      }
      // fall through to frame assembly below
    } else {
      for (var g = 0; g < 2; g++) {
        // 1. Raw subband analysis for every channel (no frequency inversion yet).
        for (var ch = 0; ch < nch; ch++) {
          final pcm = channels[ch];
          for (var ts = 0; ts < 18; ts++) {
            for (var i = 0; i < 32; i++) {
              final idx = pos + g * 576 + ts * 32 + i;
              slot[i] = idx < pcm.length ? pcm[idx] : 0.0;
            }
            sb[ch].processSlot(slot, so);
            for (var b = 0; b < 32; b++) {
              rawSub[ch][b * 18 + ts] = so[b];
            }
          }
        }
        // 2. M/S transform on the subband samples (mid/side). MDCT is linear, so
        //    this equals the decoder's frequency-line M/S; mode_ext = 2.
        if (joint) {
          for (var k = 0; k < 576; k++) {
            final l = rawSub[0][k];
            final r = rawSub[1][k];
            rawSub[0][k] = (l + r) * _kInvSqrt2;
            rawSub[1][k] = (l - r) * _kInvSqrt2;
          }
        }
        // 3. Per channel: frequency inversion → MDCT → quantize. The M/S SIDE
        //    channel (ch 1) is not psy-shaped (its noise leaks into L/R).
        for (var ch = 0; ch < nch; ch++) {
          for (var b = 0; b < 32; b++) {
            for (var ts = 0; ts < 18; ts++) {
              final v = rawSub[ch][b * 18 + ts];
              subband[b * 18 + ts] = ((b & 1) != 0 && (ts & 1) != 0) ? -v : v;
            }
          }
          mdct[ch].process(subband, mdctBuf);
          mdct[ch].aliasReduce(mdctBuf);
          gr[g].add(
            mp3QuantizeGranule(
              mdctBuf,
              availPer,
              srIndex,
              gainFloor: gainFloor,
              vbrShaping: vbr,
              allowPsy: !(joint && ch == 1),
            ),
          );
        }
      }
    }

    // Main data (byte-aligned): per granule, per channel — scalefactors +
    // Huffman (long) or the window-switching Huffman layout (short blocks).
    final md = Mp3BitWriter();
    for (var g = 0; g < 2; g++) {
      for (var ch = 0; ch < nch; ch++) {
        final gi = gr[g][ch];
        if (mp3EncoderDebugLog != null) {
          var nz = 0;
          for (var i = 0; i < 576; i++) {
            if (gi.ix[i] != 0) nz++;
          }
          mp3EncoderDebugLog!.add([
            gi.blockType,
            gi.regions.bigValues,
            gi.globalGain,
            gi.part23Length,
            nz,
          ]);
        }
        mp3EncoderIxLog?.add(Int16List.fromList(gi.ix));
        if (gi.blockType != 0) {
          mp3EncodeGranuleWs(md, gi.ix, gi.regions);
        } else {
          _writeScalefactors(md, gi);
          mp3EncodeGranule(md, gi.ix, gi.regions, srIndex);
        }
      }
    }
    md.byteAlign();
    final mdBytes = md.takeBytes();

    // VBR: pick the smallest bitrate whose frame holds this frame's main data.
    var frameBitrate = bitrate;
    var frameSize = frameBase + (pad ? 1 : 0);
    if (vbr) {
      final needed = 4 + sideInfoBytes + mdBytes.length;
      final picked = _vbrPickFrameSize(needed, sampleRate);
      frameBitrate = picked.$1;
      frameSize = picked.$2;
    }

    // Header + side info carrying main_data_begin.
    final si = Mp3BitWriter();
    _writeHeader(si, frameBitrate, sampleRate, pad, mode, modeExt: modeExt);
    si.writeBits(mdb, 9); // main_data_begin (0 for VBR)
    si.writeBits(0, nch == 1 ? 5 : 3); // private bits
    si.writeBits(0, nch * 4); // scfsi (no scalefactor sharing)
    for (var g = 0; g < 2; g++) {
      for (var ch = 0; ch < nch; ch++) {
        _writeGranuleSideInfo(si, gr[g][ch]);
      }
    }
    si.byteAlign();
    final headerSi = si.takeBytes();

    if (vbr) {
      // Self-contained frame: header + side info + main data, zero-padded.
      frameOffsets.add(out.length);
      out
        ..add(headerSi)
        ..add(mdBytes);
      final padLen = frameSize - headerSi.length - mdBytes.length;
      if (padLen > 0) out.add(Uint8List(padLen));
    } else {
      reservoir.addFrame(headerSi, mdBytes, frameSize - headerSi.length, out);
    }
    pos += 1152;
  }
  if (!vbr) {
    reservoir.flush(out);
    return out.toBytes();
  }
  // VBR: prepend a Xing header frame so players report exact duration + can seek.
  final audio = out.toBytes();
  final xingSize = mp3FrameSize(64, sampleRate);
  final total = xingSize + audio.length;
  final xing = _buildXingFrame(
    nch,
    sampleRate,
    mode,
    frameOffsets.length,
    total,
    frameOffsets,
    xingSize,
  );
  return (BytesBuilder(copy: false)
        ..add(xing)
        ..add(audio))
      .toBytes();
}

/// Build the leading Xing/VBR-info frame: a silent 64 kbps frame whose main
/// data carries the "Xing" tag (frame count + byte count + 100-entry seek TOC),
/// so decoders can report exact duration and seek into a VBR stream.
Uint8List _buildXingFrame(
  int nch,
  int sampleRate,
  Mp3ChannelMode mode,
  int frameCount,
  int totalBytes,
  List<int> frameOffsets,
  int xingSize,
) {
  final frame = Uint8List(xingSize); // all-zero → decodes as silence
  final w = Mp3BitWriter();
  _writeHeader(w, 64, sampleRate, false, mode);
  final hdr = w.takeBytes();
  frame.setRange(0, 4, hdr); // header; side info stays zero (silent frame)

  final sideInfoBytes = nch == 1 ? 17 : 32;
  var off = 4 + sideInfoBytes; // Xing tag sits right after the side info
  frame.setRange(off, off + 4, 'Xing'.codeUnits);
  off += 4;
  frame[off + 3] = 0x07; // flags: frames | bytes | TOC (big-endian 0x00000007)
  off += 4;
  void be32(int v) {
    frame[off++] = (v >> 24) & 0xFF;
    frame[off++] = (v >> 16) & 0xFF;
    frame[off++] = (v >> 8) & 0xFF;
    frame[off++] = v & 0xFF;
  }

  be32(frameCount);
  be32(totalBytes);
  // 100-entry TOC: byte position at each 1/100 of the stream, as 0..255.
  for (var i = 0; i < 100; i++) {
    var v = 0;
    if (frameCount > 0 && totalBytes > 0) {
      var f = i * frameCount ~/ 100;
      if (f >= frameCount) f = frameCount - 1;
      final absOffset = xingSize + frameOffsets[f];
      v = absOffset * 256 ~/ totalBytes;
      if (v > 255) v = 255;
    }
    frame[off++] = v;
  }
  return frame;
}

/// Smallest MPEG-1 (bitrate, frameSize) whose frame holds [neededBytes] of
/// header+side-info+main-data (glint `vbr_pick_frame_size`).
(int, int) _vbrPickFrameSize(int neededBytes, int sampleRate) {
  for (final kbps in kMp3Bitrates) {
    final size = 144 * kbps * 1000 ~/ sampleRate;
    if (size >= neededBytes) return (kbps, size);
  }
  final maxKbps = kMp3Bitrates.last;
  return (maxKbps, 144 * maxKbps * 1000 ~/ sampleRate);
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
  Mp3ChannelMode mode, {
  int modeExt = 0,
}) {
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
    ..writeBits(modeExt, 2) // mode extension (M/S = 2 for joint stereo)
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
    ..writeBits(gi.scalefacCompress, 4);
  if (gi.blockType != 0) {
    // Window-switching layout (block_type 1/2/3): 2 tables + subblock_gain,
    // implied region boundaries (decoder hardwires region0 = line 36).
    w
      ..writeBits(1, 1) // window_switching_flag
      ..writeBits(gi.blockType, 2)
      ..writeBits(0, 1) // mixed_block_flag
      ..writeBits(r.tableSelect[0], 5)
      ..writeBits(r.tableSelect[1], 5)
      ..writeBits(gi.subblockGain[0], 3)
      ..writeBits(gi.subblockGain[1], 3)
      ..writeBits(gi.subblockGain[2], 3);
  } else {
    w
      ..writeBits(0, 1) // window_switching_flag = 0
      ..writeBits(r.tableSelect[0], 5)
      ..writeBits(r.tableSelect[1], 5)
      ..writeBits(r.tableSelect[2], 5)
      ..writeBits(r.region0Count, 4)
      ..writeBits(r.region1Count, 3);
  }
  w
    ..writeBits(gi.preflag, 1)
    ..writeBits(gi.scalefacScale, 1)
    ..writeBits(r.count1Table, 1);
}

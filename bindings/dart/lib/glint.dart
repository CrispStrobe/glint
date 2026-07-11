/// Dart FFI bindings for the glint MP3 encoder.
library glint;

import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

// --- FFI type definitions ---

final class GlintConfig extends Struct {
  @Int32()
  external int sampleRate;

  @Int32()
  external int numChannels;

  @Int32()
  external int mode;

  @Int32()
  external int bitrate;

  @Int32()
  external int path;

  @Int32()
  external int simd;

  // quality/vbr fields exist in the C struct; omitting them made
  // glint_create read uninitialized memory. Keep in sync with glint.h.
  @Int32()
  external int quality;

  @Int32()
  external int vbr;

  @Int32()
  external int vbrQuality;
}

/// Mirror of struct glint_aac_config (zero-init the reserved tail).
final class GlintAacConfig extends Struct {
  @Int32()
  external int sampleRate;

  @Int32()
  external int numChannels;

  @Int32()
  external int bitrate;

  @Int32()
  external int quality;

  @Int32()
  external int vbr;

  @Int32()
  external int vbrQuality;

  @Array(4)
  external Array<Int32> reserved;
}

// Native function signatures
typedef GlintCheckConfigNative = Int32 Function(Int32 sampleRate, Int32 bitrate);
typedef GlintCheckConfig = int Function(int sampleRate, int bitrate);

typedef GlintCreateNative = Pointer<Void> Function(Pointer<GlintConfig> cfg);
typedef GlintCreate = Pointer<Void> Function(Pointer<GlintConfig> cfg);

typedef GlintSamplesPerFrameNative = Int32 Function(Pointer<Void> enc);
typedef GlintSamplesPerFrame = int Function(Pointer<Void> enc);

typedef GlintEncodeNative = Pointer<Uint8> Function(
    Pointer<Void> enc, Pointer<Pointer<Int16>> channelData, Pointer<Int32> outSize);
typedef GlintEncode = Pointer<Uint8> Function(
    Pointer<Void> enc, Pointer<Pointer<Int16>> channelData, Pointer<Int32> outSize);

typedef GlintEncodeFloatNative = Pointer<Uint8> Function(
    Pointer<Void> enc, Pointer<Pointer<Float>> channelData, Pointer<Int32> outSize);
typedef GlintEncodeFloat = Pointer<Uint8> Function(
    Pointer<Void> enc, Pointer<Pointer<Float>> channelData, Pointer<Int32> outSize);

typedef GlintFlushNative = Pointer<Uint8> Function(Pointer<Void> enc, Pointer<Int32> outSize);
typedef GlintFlush = Pointer<Uint8> Function(Pointer<Void> enc, Pointer<Int32> outSize);

typedef GlintDestroyNative = Void Function(Pointer<Void> enc);
typedef GlintDestroy = void Function(Pointer<Void> enc);

typedef GlintAacCreateNative = Pointer<Void> Function(Pointer<GlintAacConfig> cfg);
typedef GlintAacCreate = Pointer<Void> Function(Pointer<GlintAacConfig> cfg);

// --- Library loader ---

DynamicLibrary _loadLibrary() {
  if (Platform.isLinux) {
    return DynamicLibrary.open('libglint.so');
  } else if (Platform.isMacOS) {
    return DynamicLibrary.open('libglint.dylib');
  } else if (Platform.isWindows) {
    return DynamicLibrary.open('glint.dll');
  } else if (Platform.isAndroid) {
    return DynamicLibrary.open('libglint.so');
  } else if (Platform.isIOS) {
    return DynamicLibrary.process();
  }
  throw UnsupportedError('Unsupported platform: ${Platform.operatingSystem}');
}

// --- Encoder class ---

/// A safe wrapper around the glint MP3 encoder for use via Dart FFI.
///
/// Usage:
/// ```dart
/// final encoder = GlintEncoder(sampleRate: 44100, channels: 1, bitrate: 128);
/// final mp3Bytes = encoder.encode(pcmSamples);
/// final remaining = encoder.flush();
/// encoder.dispose();
/// ```
class GlintEncoder {
  late final DynamicLibrary _lib;
  late final Pointer<Void> _handle;
  late final int samplesPerFrame;
  final int channels;

  late final GlintEncode _glintEncode;
  late final GlintEncodeFloat _glintEncodeFloat;
  late final GlintFlush _glintFlush;
  late final GlintDestroy _glintDestroy;

  bool _disposed = false;

  /// Create a new MP3 encoder.
  ///
  /// Throws [ArgumentError] if the configuration is not supported.
  /// Throws [StateError] if the encoder could not be created.
  GlintEncoder({
    int sampleRate = 44100,
    int channels = 1,
    int bitrate = 128,
  }) : channels = channels {
    _lib = _loadLibrary();

    final checkConfig = _lib
        .lookupFunction<GlintCheckConfigNative, GlintCheckConfig>('glint_check_config');
    final create =
        _lib.lookupFunction<GlintCreateNative, GlintCreate>('glint_create');
    final spfFn = _lib.lookupFunction<GlintSamplesPerFrameNative,
        GlintSamplesPerFrame>('glint_samples_per_frame');

    _glintEncode =
        _lib.lookupFunction<GlintEncodeNative, GlintEncode>('glint_encode');
    _glintEncodeFloat = _lib
        .lookupFunction<GlintEncodeFloatNative, GlintEncodeFloat>('glint_encode_float');
    _glintFlush =
        _lib.lookupFunction<GlintFlushNative, GlintFlush>('glint_flush');
    _glintDestroy =
        _lib.lookupFunction<GlintDestroyNative, GlintDestroy>('glint_destroy');

    // Validate configuration
    final rc = checkConfig(sampleRate, bitrate);
    if (rc != 0) {
      throw ArgumentError('Unsupported sample rate ($sampleRate) or bitrate ($bitrate)');
    }

    // Allocate and fill config struct
    final cfgPtr = calloc<GlintConfig>();
    cfgPtr.ref.sampleRate = sampleRate;
    cfgPtr.ref.numChannels = channels;
    cfgPtr.ref.mode = channels == 1 ? 0 : 2; // MONO or JOINT
    cfgPtr.ref.bitrate = bitrate;
    cfgPtr.ref.path = 0;
    cfgPtr.ref.simd = 0;
    cfgPtr.ref.quality = 1; // NORMAL
    // vbr fields left zero (CBR); calloc zeroed the struct.

    _handle = create(cfgPtr);
    calloc.free(cfgPtr);

    if (_handle == nullptr) {
      throw StateError('glint_create returned null');
    }

    samplesPerFrame = spfFn(_handle);
  }

  /// Encode one frame of interleaved 16-bit PCM samples.
  ///
  /// [pcm] should contain [samplesPerFrame] * [channels] samples.
  /// Returns the encoded MP3 bytes for this frame (may be empty).
  Uint8List encode(Int16List pcm) {
    _checkNotDisposed();

    // De-interleave into per-channel buffers
    final channelPtrs = calloc<Pointer<Int16>>(channels);
    final buffers = <Pointer<Int16>>[];

    for (var ch = 0; ch < channels; ch++) {
      final buf = calloc<Int16>(samplesPerFrame);
      buffers.add(buf);
      channelPtrs[ch] = buf;

      for (var s = 0; s < samplesPerFrame && (s * channels + ch) < pcm.length; s++) {
        buf[s] = pcm[s * channels + ch];
      }
    }

    final outSizePtr = calloc<Int32>();
    final result = _glintEncode(_handle, channelPtrs, outSizePtr);
    final outSize = outSizePtr.value;

    Uint8List output;
    if (result == nullptr || outSize <= 0) {
      output = Uint8List(0);
    } else {
      output = Uint8List.fromList(result.asTypedList(outSize));
    }

    // Free temporary allocations
    for (final buf in buffers) {
      calloc.free(buf);
    }
    calloc.free(channelPtrs);
    calloc.free(outSizePtr);

    return output;
  }

  /// Encode one frame of interleaved 32-bit float PCM samples.
  ///
  /// Samples should be in the range [-1.0, 1.0].
  Uint8List encodeFloat(Float32List pcm) {
    _checkNotDisposed();

    final channelPtrs = calloc<Pointer<Float>>(channels);
    final buffers = <Pointer<Float>>[];

    for (var ch = 0; ch < channels; ch++) {
      final buf = calloc<Float>(samplesPerFrame);
      buffers.add(buf);
      channelPtrs[ch] = buf;

      for (var s = 0; s < samplesPerFrame && (s * channels + ch) < pcm.length; s++) {
        buf[s] = pcm[s * channels + ch];
      }
    }

    final outSizePtr = calloc<Int32>();
    final result = _glintEncodeFloat(_handle, channelPtrs, outSizePtr);
    final outSize = outSizePtr.value;

    Uint8List output;
    if (result == nullptr || outSize <= 0) {
      output = Uint8List(0);
    } else {
      output = Uint8List.fromList(result.asTypedList(outSize));
    }

    for (final buf in buffers) {
      calloc.free(buf);
    }
    calloc.free(channelPtrs);
    calloc.free(outSizePtr);

    return output;
  }

  /// Flush the encoder, returning any remaining MP3 data.
  Uint8List flush() {
    _checkNotDisposed();

    final outSizePtr = calloc<Int32>();
    final result = _glintFlush(_handle, outSizePtr);
    final outSize = outSizePtr.value;

    Uint8List output;
    if (result == nullptr || outSize <= 0) {
      output = Uint8List(0);
    } else {
      output = Uint8List.fromList(result.asTypedList(outSize));
    }

    calloc.free(outSizePtr);
    return output;
  }

  /// Release all native resources. Must be called when done encoding.
  void dispose() {
    if (!_disposed) {
      _glintDestroy(_handle);
      _disposed = true;
    }
  }

  void _checkNotDisposed() {
    if (_disposed) {
      throw StateError('GlintEncoder has been disposed');
    }
  }
}

// Allocation uses package:ffi's calloc (zero-initialised).


/// AAC-LC encoder (ADTS output). Same interleaved-PCM conventions as
/// [GlintEncoder]; one [encode] call consumes [samplesPerFrame] (1024)
/// samples per channel and returns one ADTS frame. [flush] returns the two
/// tail frames and must be called at end of stream (encoder delay: 2048
/// samples; the first frame is a silence priming frame).
class GlintAacEncoder {
  late final DynamicLibrary _lib;
  late final Pointer<Void> _handle;
  late final int samplesPerFrame;
  final int channels;

  late final GlintEncode _aacEncode;
  late final GlintFlush _aacFlush;
  late final GlintDestroy _aacDestroy;

  bool _disposed = false;

  /// [quality]: 0 = speed, 1 = normal (default), 2 = best.
  GlintAacEncoder({
    int sampleRate = 44100,
    int channels = 2,
    int bitrate = 128,
    int quality = 1,
  }) : channels = channels {
    _lib = _loadLibrary();

    final create = _lib
        .lookupFunction<GlintAacCreateNative, GlintAacCreate>('glint_aac_create');
    final spfFn = _lib.lookupFunction<GlintSamplesPerFrameNative,
        GlintSamplesPerFrame>('glint_aac_samples_per_frame');
    _aacEncode =
        _lib.lookupFunction<GlintEncodeNative, GlintEncode>('glint_aac_encode');
    _aacFlush =
        _lib.lookupFunction<GlintFlushNative, GlintFlush>('glint_aac_flush');
    _aacDestroy =
        _lib.lookupFunction<GlintDestroyNative, GlintDestroy>('glint_aac_destroy');

    final cfgPtr = calloc<GlintAacConfig>(); // calloc zeroes reserved[]
    cfgPtr.ref.sampleRate = sampleRate;
    cfgPtr.ref.numChannels = channels;
    cfgPtr.ref.bitrate = bitrate;
    cfgPtr.ref.quality = quality;

    _handle = create(cfgPtr);
    calloc.free(cfgPtr);

    if (_handle == nullptr) {
      throw StateError('glint_aac_create returned null');
    }
    samplesPerFrame = spfFn(_handle);
  }

  /// Encode one frame of interleaved 16-bit PCM samples.
  Uint8List encode(Int16List pcm) {
    _checkNotDisposed();

    final channelPtrs = calloc<Pointer<Int16>>(channels);
    final buffers = <Pointer<Int16>>[];
    for (var ch = 0; ch < channels; ch++) {
      final buf = calloc<Int16>(samplesPerFrame);
      buffers.add(buf);
      channelPtrs[ch] = buf;
      for (var s = 0;
          s < samplesPerFrame && (s * channels + ch) < pcm.length;
          s++) {
        buf[s] = pcm[s * channels + ch];
      }
    }

    final outSizePtr = calloc<Int32>();
    final result = _aacEncode(_handle, channelPtrs, outSizePtr);
    final outSize = outSizePtr.value;

    Uint8List output;
    if (result == nullptr || outSize <= 0) {
      output = Uint8List(0);
    } else {
      output = Uint8List.fromList(result.asTypedList(outSize));
    }

    for (final buf in buffers) {
      calloc.free(buf);
    }
    calloc.free(channelPtrs);
    calloc.free(outSizePtr);
    return output;
  }

  /// Flush the encoder, returning the two tail ADTS frames.
  Uint8List flush() {
    _checkNotDisposed();
    final outSizePtr = calloc<Int32>();
    final result = _aacFlush(_handle, outSizePtr);
    final outSize = outSizePtr.value;
    Uint8List output;
    if (result == nullptr || outSize <= 0) {
      output = Uint8List(0);
    } else {
      output = Uint8List.fromList(result.asTypedList(outSize));
    }
    calloc.free(outSizePtr);
    return output;
  }

  /// Destroy the encoder and free native resources.
  void dispose() {
    if (!_disposed) {
      _aacDestroy(_handle);
      _disposed = true;
    }
  }

  void _checkNotDisposed() {
    if (_disposed) {
      throw StateError('GlintAacEncoder has been disposed');
    }
  }
}


// --- Opus codec (CELT encoder + full decoder) ---

typedef _OpusEncCreateNative = Pointer<Void> Function(Int32, Int32, Int32);
typedef _OpusEncCreate = Pointer<Void> Function(int, int, int);
typedef _OpusEncodeNative = Int32 Function(
    Pointer<Void>, Pointer<Float>, Int32, Pointer<Uint8>, Int32);
typedef _OpusEncode = int Function(
    Pointer<Void>, Pointer<Float>, int, Pointer<Uint8>, int);
typedef _OpusDecCreateNative = Pointer<Void> Function(Int32, Int32);
typedef _OpusDecCreate = Pointer<Void> Function(int, int);
typedef _OpusDecodeNative = Int32 Function(
    Pointer<Void>, Pointer<Uint8>, Int32, Pointer<Float>, Int32);
typedef _OpusDecode = int Function(
    Pointer<Void>, Pointer<Uint8>, int, Pointer<Float>, int);
typedef _OpusRangeNative = Uint32 Function(Pointer<Void>);
typedef _OpusRange = int Function(Pointer<Void>);
typedef _OpusDestroyNative = Void Function(Pointer<Void>);
typedef _OpusDestroy = void Function(Pointer<Void>);

/// CELT-only Opus encoder: 48 kHz interleaved float32 PCM in, complete
/// Opus packets out. Frame sizes 120/240/480/960 samples per channel.
class GlintOpusEncoder {
  late final DynamicLibrary _lib;
  late final Pointer<Void> _handle;
  final int channels;

  late final _OpusEncode _encode;
  late final _OpusRange _finalRange;
  late final _OpusDestroy _destroy;

  bool _disposed = false;

  GlintOpusEncoder({int channels = 2, int bitrate = 96000, bool vbr = false})
      : channels = channels {
    _lib = _loadLibrary();
    final create = _lib.lookupFunction<_OpusEncCreateNative, _OpusEncCreate>(
        'glint_opus_enc_create');
    _encode = _lib
        .lookupFunction<_OpusEncodeNative, _OpusEncode>('glint_opus_encode');
    _finalRange = _lib.lookupFunction<_OpusRangeNative, _OpusRange>(
        'glint_opus_enc_final_range');
    _destroy = _lib.lookupFunction<_OpusDestroyNative, _OpusDestroy>(
        'glint_opus_enc_destroy');
    _handle = create(channels, bitrate, vbr ? 1 : 0);
    if (_handle == nullptr) {
      throw StateError('glint_opus_enc_create returned null');
    }
  }

  /// Encode one frame of interleaved float PCM
  /// (frameSize * channels values; frameSize in {120, 240, 480, 960}).
  Uint8List encode(Float32List pcm) {
    _checkNotDisposed();
    final frame = pcm.length ~/ channels;
    final inPtr = calloc<Float>(pcm.length);
    inPtr.asTypedList(pcm.length).setAll(0, pcm);
    final outPtr = calloc<Uint8>(1500);
    final n = _encode(_handle, inPtr, frame, outPtr, 1500);
    Uint8List out;
    if (n <= 0) {
      out = Uint8List(0);
    } else {
      out = Uint8List.fromList(outPtr.asTypedList(n));
    }
    calloc.free(inPtr);
    calloc.free(outPtr);
    if (n < 0) {
      throw StateError('opus encode failed ($n)');
    }
    return out;
  }

  int finalRange() => _finalRange(_handle);

  void dispose() {
    if (!_disposed) {
      _destroy(_handle);
      _disposed = true;
    }
  }

  void _checkNotDisposed() {
    if (_disposed) throw StateError('GlintOpusEncoder has been disposed');
  }
}

/// Opus decoder (SILK/CELT/hybrid, PLC, SILK in-band FEC). Output rates
/// 48000/24000/16000/12000/8000; interleaved float32 PCM out.
class GlintOpusDecoder {
  late final DynamicLibrary _lib;
  late final Pointer<Void> _handle;
  final int channels;

  late final _OpusDecode _decode;
  late final _OpusDecode _decodeFec;
  late final _OpusRange _finalRange;
  late final _OpusDestroy _destroy;

  bool _disposed = false;

  GlintOpusDecoder({int channels = 2, int sampleRate = 48000})
      : channels = channels {
    _lib = _loadLibrary();
    final create = _lib.lookupFunction<_OpusDecCreateNative, _OpusDecCreate>(
        'glint_opus_dec_create');
    _decode = _lib
        .lookupFunction<_OpusDecodeNative, _OpusDecode>('glint_opus_decode');
    _decodeFec = _lib.lookupFunction<_OpusDecodeNative, _OpusDecode>(
        'glint_opus_decode_fec');
    _finalRange = _lib.lookupFunction<_OpusRangeNative, _OpusRange>(
        'glint_opus_dec_final_range');
    _destroy = _lib.lookupFunction<_OpusDestroyNative, _OpusDestroy>(
        'glint_opus_dec_destroy');
    _handle = create(channels, sampleRate);
    if (_handle == nullptr) {
      throw StateError('glint_opus_dec_create returned null');
    }
  }

  /// Decode one packet; returns interleaved float PCM.
  Float32List decode(Uint8List packet) {
    _checkNotDisposed();
    final inPtr = calloc<Uint8>(packet.length);
    inPtr.asTypedList(packet.length).setAll(0, packet);
    final outPtr = calloc<Float>(2 * 5760);
    final n = _decode(_handle, inPtr, packet.length, outPtr, 5760);
    Float32List out;
    if (n <= 0) {
      out = Float32List(0);
    } else {
      out = Float32List.fromList(outPtr.asTypedList(n * channels));
    }
    calloc.free(inPtr);
    calloc.free(outPtr);
    if (n < 0) {
      throw StateError('opus decode failed ($n)');
    }
    return out;
  }

  /// Conceal a LOST packet of [frameSize] samples per channel;
  /// [nextPacket] (the packet after the loss) supplies SILK in-band FEC
  /// when present.
  Float32List decodeLost(int frameSize, [Uint8List? nextPacket]) {
    _checkNotDisposed();
    final outPtr = calloc<Float>(2 * 5760);
    int n;
    if (nextPacket != null && nextPacket.isNotEmpty) {
      final inPtr = calloc<Uint8>(nextPacket.length);
      inPtr.asTypedList(nextPacket.length).setAll(0, nextPacket);
      n = _decodeFec(_handle, inPtr, nextPacket.length, outPtr, frameSize);
      calloc.free(inPtr);
    } else {
      n = _decodeFec(_handle, nullptr, 0, outPtr, frameSize);
    }
    Float32List out;
    if (n <= 0) {
      out = Float32List(0);
    } else {
      out = Float32List.fromList(outPtr.asTypedList(n * channels));
    }
    calloc.free(outPtr);
    if (n < 0) {
      throw StateError('opus concealment failed ($n)');
    }
    return out;
  }

  int finalRange() => _finalRange(_handle);

  void dispose() {
    if (!_disposed) {
      _destroy(_handle);
      _disposed = true;
    }
  }

  void _checkNotDisposed() {
    if (_disposed) throw StateError('GlintOpusDecoder has been disposed');
  }
}


// --- MP3 + AAC-LC decoders ---

final class GlintDecFrameInfo extends Struct {
  @Int32()
  external int sampleRate;
  @Int32()
  external int channels;
  @Int32()
  external int samples;
  @Int32()
  external int frameBytes;
}

typedef _DecCreateNative = Pointer<Void> Function();
typedef _DecCreate = Pointer<Void> Function();
typedef _DecodeNative = Int32 Function(Pointer<Void>, Pointer<Uint8>, Int32,
    Pointer<Float>, Pointer<GlintDecFrameInfo>);
typedef _Decode = int Function(Pointer<Void>, Pointer<Uint8>, int,
    Pointer<Float>, Pointer<GlintDecFrameInfo>);
typedef _FrameInfoNative = Int32 Function(
    Pointer<Uint8>, Int32, Pointer<GlintDecFrameInfo>);
typedef _FrameInfo = int Function(
    Pointer<Uint8>, int, Pointer<GlintDecFrameInfo>);

/// Shared MP3/AAC decoder frontend. Feed a whole stream to [decode] or
/// one frame at a time to [decodeFrame]; output is interleaved float PCM.
abstract class _FrameDecoder {
  late final DynamicLibrary _lib;
  late final Pointer<Void> _handle;
  late final _Decode _decode;
  late final _FrameInfo _frameInfo;
  late final _DecCreate _create;
  late final void Function(Pointer<Void>) _destroy;
  bool _disposed = false;

  String get _prefix;

  _FrameDecoder() {
    _lib = _loadLibrary();
    _create = _lib.lookupFunction<_DecCreateNative, _DecCreate>(
        'glint_${_prefix}_dec_create');
    _decode = _lib.lookupFunction<_DecodeNative, _Decode>(
        'glint_${_prefix}_decode');
    _frameInfo = _lib.lookupFunction<_FrameInfoNative, _FrameInfo>(
        'glint_${_prefix}_frame_info');
    _destroy = _lib.lookupFunction<
        Void Function(Pointer<Void>),
        void Function(Pointer<Void>)>('glint_${_prefix}_dec_destroy');
    _handle = _create();
    if (_handle == nullptr) {
      throw StateError('glint_${_prefix}_dec_create returned null');
    }
  }

  /// Decode a whole stream (walks frames, skips ID3v2). Returns
  /// interleaved float PCM.
  Float32List decode(Uint8List data) {
    _checkNotDisposed();
    var off = 0;
    if (data.length > 10 &&
        data[0] == 0x49 && data[1] == 0x44 && data[2] == 0x33) {
      final sz = ((data[6] & 0x7F) << 21) |
          ((data[7] & 0x7F) << 14) |
          ((data[8] & 0x7F) << 7) |
          (data[9] & 0x7F);
      off = 10 + sz;
    }
    final inPtr = calloc<Uint8>(data.length);
    inPtr.asTypedList(data.length).setAll(0, data);
    final outPtr = calloc<Float>(2 * 1152);
    final fi = calloc<GlintDecFrameInfo>();
    final acc = <double>[];
    try {
      while (off + 7 <= data.length) {
        if (_frameInfo((inPtr + off), data.length - off, fi) < 0) {
          off++;
          continue;
        }
        final fb = fi.ref.frameBytes;
        if (fb == 0 || off + fb > data.length) break;
        final n = _decode(_handle, (inPtr + off),
            data.length - off, outPtr, fi);
        if (n > 0) {
          final ch = fi.ref.channels == 0 ? 1 : fi.ref.channels;
          acc.addAll(outPtr.asTypedList(n * ch));
        }
        off += fb;
      }
    } finally {
      calloc.free(inPtr);
      calloc.free(outPtr);
      calloc.free(fi);
    }
    return Float32List.fromList(acc);
  }

  void dispose() {
    if (!_disposed) {
      _destroy(_handle);
      _disposed = true;
    }
  }

  void _checkNotDisposed() {
    if (_disposed) throw StateError('decoder has been disposed');
  }
}

/// MPEG-1/2 Layer III decoder.
class GlintMp3Decoder extends _FrameDecoder {
  @override
  String get _prefix => 'mp3';
}

/// ADTS AAC-LC decoder.
class GlintAacDecoder extends _FrameDecoder {
  @override
  String get _prefix => 'aac';
}

// ---------------------------------------------------------------------------
// High-level convenience: resample + whole-file decode (PLAN buckets A+B)
// ---------------------------------------------------------------------------

typedef _ResampleNative = Pointer<Float> Function(Pointer<Float>, Int32,
    Int32, Int32, Int32, Pointer<Int32>);
typedef _Resample = Pointer<Float> Function(Pointer<Float>, int, int, int,
    int, Pointer<Int32>);
typedef _FreeNative = Void Function(Pointer<Void>);
typedef _Free = void Function(Pointer<Void>);
typedef _DecodeAudioNative = Pointer<Float> Function(Pointer<Uint8>, Int32,
    Pointer<Int32>, Pointer<Int32>, Pointer<Int32>);
typedef _DecodeAudio = Pointer<Float> Function(Pointer<Uint8>, int,
    Pointer<Int32>, Pointer<Int32>, Pointer<Int32>);
typedef _DecodeExNative = Pointer<Void> Function(Pointer<Uint8>, Int32,
    Int32, Int32, Pointer<Int32>, Pointer<Int32>, Pointer<Int32>);
typedef _DecodeEx = Pointer<Void> Function(Pointer<Uint8>, int, int, int,
    Pointer<Int32>, Pointer<Int32>, Pointer<Int32>);
typedef _OpusEncFileNative = Pointer<Uint8> Function(Pointer<Float>, Int32,
    Int32, Int32, Int32, Pointer<Int32>);
typedef _OpusEncFile = Pointer<Uint8> Function(Pointer<Float>, int, int, int,
    int, Pointer<Int32>);
typedef _WavReadNative = Pointer<Float> Function(Pointer<Uint8>, Int32,
    Pointer<Int32>, Pointer<Int32>, Pointer<Int32>);
typedef _WavRead = Pointer<Float> Function(Pointer<Uint8>, int,
    Pointer<Int32>, Pointer<Int32>, Pointer<Int32>);
typedef _WavWriteNative = Pointer<Uint8> Function(Pointer<Float>, Int32,
    Int32, Int32, Int32, Int32, Pointer<Int32>);
typedef _WavWrite = Pointer<Uint8> Function(Pointer<Float>, int, int, int,
    int, int, Pointer<Int32>);
typedef _EncAudioNative = Pointer<Uint8> Function(Pointer<Float>, Int32,
    Int32, Int32, Int32, Int32, Int32, Int32, Pointer<Int32>);
typedef _EncAudio = Pointer<Uint8> Function(Pointer<Float>, int, int, int,
    int, int, int, int, Pointer<Int32>);

/// Decoded audio: interleaved float PCM plus its stream parameters.
class GlintDecodedAudio {
  final Float32List pcm;
  final int sampleRate;
  final int channels;
  GlintDecodedAudio(this.pcm, this.sampleRate, this.channels);
}

/// Resample interleaved float PCM (±1.0) from [srIn] to [srOut] with a
/// Kaiser-windowed sinc kernel (anti-aliased, unity passband). [pcm] is
/// `frames * channels` interleaved samples. Pass-through (a copy) when the
/// rates match.
Float32List glintResample(Float32List pcm, int channels, int srIn, int srOut) {
  if (channels <= 0 || pcm.isEmpty) return Float32List(0);
  final lib = _loadLibrary();
  final fn = lib.lookupFunction<_ResampleNative, _Resample>('glint_resample');
  final free = lib.lookupFunction<_FreeNative, _Free>('glint_free');
  final inPtr = calloc<Float>(pcm.length);
  inPtr.asTypedList(pcm.length).setAll(0, pcm);
  final outFrames = calloc<Int32>();
  try {
    final inFrames = pcm.length ~/ channels;
    final ptr = fn(inPtr, inFrames, channels, srIn, srOut, outFrames);
    if (ptr == nullptr) return Float32List(0);
    final total = outFrames.value * channels;
    final out = Float32List.fromList(ptr.asTypedList(total));
    free(ptr.cast<Void>());
    return out;
  } finally {
    calloc.free(inPtr);
    calloc.free(outFrames);
  }
}

/// Decode a whole encoded stream (MP3 / AAC-LC / Ogg-Opus, format auto-
/// detected from the header) to interleaved float PCM. Throws
/// [StateError] on unrecognized or corrupt input.
/// Decoded int16 audio.
class GlintDecodedAudioI16 {
  final Int16List pcm;
  final int sampleRate;
  final int channels;
  GlintDecodedAudioI16(this.pcm, this.sampleRate, this.channels);
}

/// Decode a whole encoded stream (MP3 / AAC-LC / Ogg-Opus, format auto-
/// detected, Opus surround included) to interleaved float PCM. [rate]
/// resamples the output (0 = keep native). Throws on unrecognized input.
GlintDecodedAudio glintDecodeAudio(Uint8List data, {int rate = 0}) {
  if (data.isEmpty) throw StateError('empty input');
  final lib = _loadLibrary();
  final fn = lib.lookupFunction<_DecodeExNative, _DecodeEx>(
      'glint_decode_audio_ex');
  final free = lib.lookupFunction<_FreeNative, _Free>('glint_free');
  final inPtr = calloc<Uint8>(data.length);
  inPtr.asTypedList(data.length).setAll(0, data);
  final sr = calloc<Int32>();
  final ch = calloc<Int32>();
  final fr = calloc<Int32>();
  try {
    final ptr = fn(inPtr, data.length, rate, 0, sr, ch, fr);
    if (ptr == nullptr || ch.value <= 0) {
      throw StateError('decode failed (unrecognized or corrupt input)');
    }
    final total = fr.value * ch.value;
    final pcm = Float32List.fromList(ptr.cast<Float>().asTypedList(total));
    free(ptr);
    return GlintDecodedAudio(pcm, sr.value, ch.value);
  } finally {
    calloc.free(inPtr);
    calloc.free(sr);
    calloc.free(ch);
    calloc.free(fr);
  }
}

/// Like [glintDecodeAudio] but returns interleaved int16 PCM.
GlintDecodedAudioI16 glintDecodeAudioI16(Uint8List data, {int rate = 0}) {
  if (data.isEmpty) throw StateError('empty input');
  final lib = _loadLibrary();
  final fn = lib.lookupFunction<_DecodeExNative, _DecodeEx>(
      'glint_decode_audio_ex');
  final free = lib.lookupFunction<_FreeNative, _Free>('glint_free');
  final inPtr = calloc<Uint8>(data.length);
  inPtr.asTypedList(data.length).setAll(0, data);
  final sr = calloc<Int32>();
  final ch = calloc<Int32>();
  final fr = calloc<Int32>();
  try {
    final ptr = fn(inPtr, data.length, rate, 1, sr, ch, fr);
    if (ptr == nullptr || ch.value <= 0) {
      throw StateError('decode failed (unrecognized or corrupt input)');
    }
    final total = fr.value * ch.value;
    final pcm = Int16List.fromList(ptr.cast<Int16>().asTypedList(total));
    free(ptr);
    return GlintDecodedAudioI16(pcm, sr.value, ch.value);
  } finally {
    calloc.free(inPtr);
    calloc.free(sr);
    calloc.free(ch);
    calloc.free(fr);
  }
}

/// Encode interleaved 48 kHz float PCM (±1.0, [channels] = 1 or 2) to a
/// complete Ogg-Opus file (CELT-only, 20 ms frames). [vbr] selects
/// unconstrained VBR. Input MUST be 48 kHz — resample first with
/// [glintResample]. Returns the .opus file bytes.
Uint8List glintEncodeOpus(Float32List pcm, int channels,
    {int bitrate = 96000, bool vbr = false}) {
  if (channels < 1 || channels > 2 || pcm.isEmpty) {
    throw StateError('opus encode: bad channels/empty input');
  }
  final lib = _loadLibrary();
  final fn = lib.lookupFunction<_OpusEncFileNative, _OpusEncFile>(
      'glint_opus_encode_file');
  final free = lib.lookupFunction<_FreeNative, _Free>('glint_free');
  final inPtr = calloc<Float>(pcm.length);
  inPtr.asTypedList(pcm.length).setAll(0, pcm);
  final outSize = calloc<Int32>();
  try {
    final frames = pcm.length ~/ channels;
    final ptr = fn(inPtr, frames, channels, bitrate, vbr ? 1 : 0, outSize);
    if (ptr == nullptr || outSize.value <= 0) {
      throw StateError('opus encode failed');
    }
    final out = Uint8List.fromList(ptr.asTypedList(outSize.value));
    free(ptr.cast<Void>());
    return out;
  } finally {
    calloc.free(inPtr);
    calloc.free(outSize);
  }
}

/// Read a WAV buffer (PCM 8/16/24/32, IEEE float 32/64, A-law, mu-law,
/// EXTENSIBLE) into interleaved float PCM. Throws [StateError] on
/// malformed or unsupported input.
GlintDecodedAudio glintReadWav(Uint8List data) {
  if (data.isEmpty) throw StateError('empty WAV');
  final lib = _loadLibrary();
  final fn = lib.lookupFunction<_WavReadNative, _WavRead>('glint_wav_read');
  final free = lib.lookupFunction<_FreeNative, _Free>('glint_free');
  final inPtr = calloc<Uint8>(data.length);
  inPtr.asTypedList(data.length).setAll(0, data);
  final sr = calloc<Int32>();
  final ch = calloc<Int32>();
  final fr = calloc<Int32>();
  try {
    final ptr = fn(inPtr, data.length, sr, ch, fr);
    if (ptr == nullptr || ch.value <= 0) {
      throw StateError('WAV read failed (unsupported format?)');
    }
    final total = fr.value * ch.value;
    final pcm = Float32List.fromList(ptr.asTypedList(total));
    free(ptr.cast<Void>());
    return GlintDecodedAudio(pcm, sr.value, ch.value);
  } finally {
    calloc.free(inPtr);
    calloc.free(sr);
    calloc.free(ch);
    calloc.free(fr);
  }
}

/// Encode interleaved float PCM (±1.0) to a WAV file buffer. [bits]:
/// 8/16/24/32 integer PCM, or 32/64 with [floatFmt] for IEEE float
/// (invalid combos fall back to 16-bit).
Uint8List glintWriteWav(Float32List pcm, int channels, int sampleRate,
    {int bits = 16, bool floatFmt = false}) {
  if (channels < 1) throw StateError('bad channels');
  final lib = _loadLibrary();
  final fn = lib.lookupFunction<_WavWriteNative, _WavWrite>('glint_wav_write');
  final free = lib.lookupFunction<_FreeNative, _Free>('glint_free');
  final inPtr = calloc<Float>(pcm.length);
  inPtr.asTypedList(pcm.length).setAll(0, pcm);
  final outSize = calloc<Int32>();
  try {
    final frames = pcm.length ~/ channels;
    final ptr = fn(inPtr, frames, channels, sampleRate, bits,
        floatFmt ? 1 : 0, outSize);
    if (ptr == nullptr || outSize.value <= 0) {
      throw StateError('WAV write failed');
    }
    final out = Uint8List.fromList(ptr.asTypedList(outSize.value));
    free(ptr.cast<Void>());
    return out;
  } finally {
    calloc.free(inPtr);
    calloc.free(outSize);
  }
}

/// Output codec for [glintEncodeAudio].
enum GlintCodec { mp3, aac, opus }

/// One-call encode: interleaved float PCM (±1.0) at any rate / 1-2
/// channels -> a complete MP3 / AAC-LC / Ogg-Opus stream. The input is
/// auto-resampled to a codec-valid rate (Opus->48k, MP3/AAC->nearest
/// supported). [bitrate] in kbps; [vbrQuality] 0..9 selects VBR.
Uint8List glintEncodeAudio(Float32List pcm, int channels, int sampleRate,
    GlintCodec codec,
    {int bitrate = 128, int? vbrQuality, int quality = 1}) {
  if (channels < 1 || channels > 2 || pcm.isEmpty) {
    throw StateError('encode: bad channels/empty input');
  }
  final lib = _loadLibrary();
  final fn = lib.lookupFunction<_EncAudioNative, _EncAudio>(
      'glint_encode_audio');
  final free = lib.lookupFunction<_FreeNative, _Free>('glint_free');
  final inPtr = calloc<Float>(pcm.length);
  inPtr.asTypedList(pcm.length).setAll(0, pcm);
  final outSize = calloc<Int32>();
  try {
    final frames = pcm.length ~/ channels;
    final ptr = fn(inPtr, frames, channels, sampleRate, codec.index, bitrate,
        vbrQuality ?? -1, quality, outSize);
    if (ptr == nullptr || outSize.value <= 0) {
      throw StateError('encode failed');
    }
    final out = Uint8List.fromList(ptr.asTypedList(outSize.value));
    free(ptr.cast<Void>());
    return out;
  } finally {
    calloc.free(inPtr);
    calloc.free(outSize);
  }
}

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

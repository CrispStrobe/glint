/// Dart FFI bindings for the glint MP3 encoder.
library glint;

import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

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

// --- Simple calloc allocator ---

class _Calloc {
  late final _mallocFn = DynamicLibrary.process()
      .lookupFunction<Pointer<Void> Function(IntPtr), Pointer<Void> Function(int)>('malloc');
  late final _memsetFn = DynamicLibrary.process().lookupFunction<
      Pointer<Void> Function(Pointer<Void>, Int32, IntPtr),
      Pointer<Void> Function(Pointer<Void>, int, int)>('memset');
  late final _freeFn = DynamicLibrary.process()
      .lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>('free');

  Pointer<T> call<T extends NativeType>([int count = 1]) {
    final size = sizeOf<T>() * count;
    final ptr = _mallocFn(size);
    _memsetFn(ptr, 0, size);
    return ptr.cast<T>();
  }

  void free(Pointer ptr) {
    _freeFn(ptr.cast<Void>());
  }
}

final calloc = _Calloc();

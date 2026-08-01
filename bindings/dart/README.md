# glint_audio

Dart FFI bindings for the
[glint](https://github.com/CrispStrobe/glint) codec suite.

The package encodes MP3, AAC-LC and Opus, decodes those plus Ogg-Vorbis and
FLAC, and adds whole-file decode/encode helpers, WAV read/write helpers and a
Kaiser-windowed sinc resampler. It loads the native `glint` library at runtime:

- Linux and Android: `libglint.so`
- macOS: `libglint.dylib`
- Windows: `glint.dll`
- iOS: symbols from the process image

This package does not ship prebuilt native libraries. Build or bundle the
native library from the glint repository for the target platform before
using the Dart bindings.

## Usage

```dart
import 'dart:typed_data';

import 'package:glint_audio/glint_audio.dart';

final pcm = Float32List(48000 * 2); // interleaved stereo, +/-1.0

final opus = glintEncodeAudio(
  pcm,
  2,
  48000,
  GlintCodec.opus,
  bitrate: 96000,
);

final decoded = glintDecodeAudio(opus);
final wav = glintWriteWav(decoded.pcm, decoded.channels, decoded.sampleRate);
```

`glintDecodeAudio` auto-detects MP3, AAC (ADTS), Ogg-Opus, Ogg-Vorbis, FLAC
and WAV from the header.

For lower-level APIs, use `GlintEncoder`, `GlintAacEncoder` and
`GlintOpusEncoder` (streaming encode), `GlintMp3Decoder`, `GlintAacDecoder`
and `GlintOpusDecoder` (frame/packet decode), or `GlintVorbisDecoder` and
`GlintFlacDecoder` (whole-buffer decode).

## Native library

The bindings track the C ABI of the glint repository at the same version, so
build the native library from the matching tag: a call into a symbol the
installed library predates (for example `glint_flac_decode` against a
pre-0.11.0 build) fails at lookup time, not at compile time.

## License

MIT. See `LICENSE`.

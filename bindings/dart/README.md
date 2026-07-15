# glint_audio

Dart FFI bindings for the
[glint](https://github.com/CrispStrobe/glint) codec suite.

The package exposes MP3, AAC-LC and Opus encode/decode APIs, whole-file
decode and encode helpers, WAV read/write helpers, and a Kaiser-windowed
sinc resampler. It loads the native `glint` library at runtime:

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

For lower-level streaming APIs, use `GlintEncoder`, `GlintAacEncoder`,
`GlintOpusEncoder`, `GlintOpusDecoder`, `GlintMp3Decoder` and
`GlintAacDecoder`.

## License

MIT. See `LICENSE`.

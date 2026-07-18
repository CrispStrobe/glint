## 0.10.0

- Add `GlintVorbisDecoder` — whole-buffer Ogg-Vorbis I decode
  (`decode(Uint8List ogg) -> ({int sampleRate, int channels, Float32List pcm})`)
  via `glint_vorbis_decode`, mirroring `GlintOpusDecoder`.
- `glintDecodeAudio` now also decodes Ogg-Vorbis transparently (the native
  auto-detect splits `OggS` into Opus vs Vorbis by the first packet's codec
  id). Matches ffmpeg and sox(libvorbis) at the float-precision floor.

## 0.9.0

- Initial pub.dev release as `glint_audio`.
- Exposes Dart FFI bindings for glint MP3, AAC-LC and Opus encode/decode APIs.
- Adds whole-file audio encode/decode helpers, WAV read/write helpers and resampling.

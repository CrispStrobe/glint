## 0.11.0

- Add `GlintFlacDecoder` — whole-buffer native FLAC decode
  (`decode(Uint8List flac) -> ({int sampleRate, int channels, Float32List pcm})`)
  via `glint_flac_decode`, mirroring `GlintVorbisDecoder`.
- `glintDecodeAudio` now also decodes FLAC transparently, so the whole-file
  helper covers MP3, AAC, Ogg-Opus, Ogg-Vorbis, FLAC and WAV from the header
  alone.

Requires a native `glint` library built from this repo at 0.11.0 or later —
`glint_flac_decode` does not exist in earlier builds.

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

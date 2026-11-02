## 0.12.0

- **Gapless MP3 decode.** `glintDecodeAudio` / `glintDecodeAudioI16` now skip
  the Xing/Info tag frame and trim the LAME encoder delay (+529 decoder
  latency) and end padding, so an MP3 decodes to exactly the samples that were
  encoded — sample-exact with ffmpeg. Before, every MP3 came back ~25 ms late
  and one granule long.
- Add `glintVersion()` so callers can require a library with the fix
  (`glintVersion() >= 0x000801`).

Requires a native `glint` library built from this repo at 0.10.0 or later for
the gapless decode; `glintVersion()` returns 0 on libraries older than 0.8.

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

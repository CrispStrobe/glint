# glint-audio-sys

Raw FFI bindings to the [glint](https://github.com/CrispStrobe/glint) audio
codec suite (MP3, AAC-LC, Opus, Ogg-Vorbis, FLAC, WAV, resampling).

This is the unsafe `-sys` layer: `extern "C"` declarations and `#[repr(C)]`
structs mirroring `include/glint/glint.h`. Most users want
[`glint-audio`](https://crates.io/crates/glint-audio), the safe wrapper.

## Building

The crate vendors the glint C++ sources and compiles them with `cc`, so it
needs a **C++17 compiler** but no system codec libraries, no `pkg-config` and
no prebuilt binaries. It exports `links = "glint"`.

## Versioning

The crate version tracks the glint C ABI. Keeping `glint-audio` and
`glint-audio-sys` on the same version is intentional — they are released
together from one repository.

## License

MIT. See `LICENSE`.

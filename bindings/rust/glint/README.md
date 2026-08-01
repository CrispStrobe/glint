# glint-audio

Safe Rust wrapper for the [glint](https://github.com/CrispStrobe/glint) audio
codec suite — clean-room C++17 codecs with no system codec libraries and no
runtime dependencies. The C++ sources are compiled from source by
[`glint-audio-sys`](https://crates.io/crates/glint-audio-sys), so a build needs
only a C++17 compiler; there is nothing to install and nothing to link against.

| Format | Encode | Decode |
| --- | --- | --- |
| MP3 (MPEG-1/2 Layer III) | yes | yes |
| AAC-LC (ADTS) | yes | yes |
| Opus | yes (CELT) | yes (SILK + CELT + hybrid, multistream/surround) |
| Ogg-Vorbis | — | yes |
| FLAC | — | yes |
| WAV (PCM 8/16/24/32, float, A-law, mu-law) | yes | yes |

The crate is named `glint-audio` but the library is `glint`, so imports read
`use glint::...`.

## Usage

```toml
[dependencies]
glint-audio = "0.11"
```

Whole-file decode, resample and re-encode:

```rust
let data = std::fs::read("input.mp3")?;

// Format auto-detected from the header.
let audio = glint::decode_audio(&data).expect("decode failed");

// Auto-resampled to a codec-valid rate (Opus -> 48 kHz). Bitrate is kbps;
// `None` selects CBR, `Some(q)` constant-quality VBR. Trailing 1 = quality
// preset (0 speed / 1 normal / 2 best).
let opus = glint::encode_audio(
    &audio.pcm,
    audio.channels,
    audio.sample_rate,
    glint::Codec::Opus,
    96,
    None,
    1,
)
.expect("encode failed");
std::fs::write("output.opus", opus)?;
```

Streaming encode:

```rust
let mut enc = glint::Encoder::new(44_100, 2, 192)?;
let mut mp3 = Vec::new();
for chunk in pcm.chunks(enc.samples_per_frame() * enc.channels()) {
    mp3.extend(enc.encode(chunk));
}
mp3.extend(enc.flush()); // required — the bit reservoir defers frames
```

`Encoder`, `AacEncoder`, `OpusEncoder` and `OpusDecoder` cover streaming use;
`Mp3Decoder` and `AacDecoder` decode frame by frame; `decode_vorbis`,
`decode_flac`, `read_wav`, `write_wav` and `resample` handle whole buffers.

`flush()` is required at end of stream for the MP3 and AAC encoders: both defer
output (MP3 through the bit reservoir, AAC by a one-block window lookahead), so
the final frames only appear when you flush.

## License

MIT. See `LICENSE`.

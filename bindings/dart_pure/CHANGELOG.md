## 0.6.0

- **Short/transient blocks now work in stereo and joint (M/S) too** — the
  `shortBlocks` flag is on `mp3EncodeStereo`/`mp3EncodeJointStereo` as well as
  mono. Each channel schedules its own block-type chain; joint M/S combines the
  channels before the MDCT. Default stays OFF (byte-identical). A stereo
  transient reconstructs ~57–59 dB per channel, beating long-only.

## 0.5.0

- **Short / transient block encoding** (opt-in `shortBlocks: true` on
  `mp3EncodeMono`): a transient scheduler emits short/start/stop granules over
  attacks (long→start→short→stop→long), fixing pre-echo on percussive material.
  Default stays OFF — long-only output is byte-identical to 0.4.0. Fixes two
  window-switching quantizer bugs (ESC Huffman table selection for large
  coefficients; a missing anti-clip gain bound) so short blocks now reconstruct
  at ~70–78 dB and beat long-only on transients; the ffmpeg oracle agrees.
- **Decoder now handles every block type** (long / short / mixed / start / stop),
  so it decodes any real-world MP3, not just long-block streams.

## 0.4.0

- **Decoder** (`mp3Decode`): pure-Dart MPEG-1 Layer III decode (mono/stereo/
  joint), so the codec now round-trips without any external tool. Matches
  ffmpeg's output bit-for-bit in quality.
- **Joint (M/S) stereo** encoding (`mp3EncodeJointStereo`).

## 0.3.0

- VBR streams now carry a Xing header (frame count + byte count + seek TOC),
  so players report exact duration and can seek.

## 0.2.0

- Stereo encoding (`mp3EncodeStereo`).
- Variable-bitrate (`mp3EncodeMonoVbr` / `mp3EncodeStereoVbr`, quality 0–9).
- Bit reservoir (main data spills across frame slots for better noise shaping).
- Huffman region optimizer (rate-optimal region/table selection).
- Quality: SNR 35 dB, NMR −7 dB on speech 128k (glint's own measure_audio.py).

## 0.1.0

- Initial release: pure-Dart MPEG-1 Layer III (MP3) encoder (mono, CBR).
- Full pipeline: subband analysis → MDCT (alias reduction + frequency inversion)
  → rate/distortion quantization with psychoacoustic noise shaping → Huffman →
  CBR frame assembly. No native code, no FFI, no runtime dependencies.
- Ported clean-room from the glint C++ codec suite.

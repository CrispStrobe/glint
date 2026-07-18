## 0.1.0

- Initial release: pure-Dart MPEG-1 Layer III (MP3) encoder.
- Mono, constant-bitrate, long blocks. `mp3EncodeMono(pcm, sampleRate, bitrate)`.
- Full pipeline: 32-band polyphase analysis → MDCT (alias reduction + frequency
  inversion) → rate/distortion quantization with psychoacoustic noise shaping
  (scalefactors, scalefac_scale, preflag) → Huffman coding → CBR frame assembly.
- No native code, no FFI, no runtime dependencies — runs on native and web.
- Ported clean-room from the glint C++ codec suite; the DSP front-end is
  machine-equivalent to glint (subband 5e-15, MDCT 7e-16 relative error) and the
  quantizer reproduces glint's per-granule decisions.
- Bundled `readWavPcm16` / `wavToMonoFloat` WAV helpers.

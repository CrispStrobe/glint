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

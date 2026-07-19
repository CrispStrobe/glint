/// Pure-Dart MP3 (MPEG-1 Layer III) encoder — no native code, no FFI, so it
/// runs identically on native AND the web. A pure-Dart sibling of the
/// FFI-backed [`glint_audio`](https://pub.dev/packages/glint_audio), ported
/// clean-room from the [glint](https://github.com/CrispStrobe/glint) codec
/// suite.
///
/// ```dart
/// import 'dart:typed_data';
/// import 'package:glint_audio_pure/glint_audio_pure.dart';
///
/// // pcm: mono samples in `-1..1`
/// final Uint8List mp3 = mp3EncodeMono(pcm, sampleRate: 44100, bitrate: 128);
/// ```
///
/// The encoder covers the full MPEG-1 Layer III pipeline: 32-band polyphase
/// analysis → MDCT (with alias reduction + frequency inversion) → a
/// rate/distortion quantization loop with psychoacoustic noise shaping
/// (scalefactors under a masking threshold) → Huffman coding → CBR frame
/// assembly. Output is a standard `.mp3` that any decoder (ffmpeg, etc.) plays.
///
/// Current scope: mono, constant-bitrate, long blocks. See the README for the
/// quality benchmark against the reference C++ encoder.
library;

export 'src/mp3_encoder.dart'
    show
        mp3EncodeMono,
        mp3EncodeStereo,
        mp3EncodeJointStereo,
        mp3EncodeMonoVbr,
        mp3EncodeStereoVbr;
export 'src/mp3_frame.dart'
    show
        kMp3Bitrates,
        kMp3SampleRates,
        kMp3SamplesPerFrame,
        Mp3ChannelMode,
        mp3FrameSize;
export 'src/mp3_decoder.dart' show mp3Decode, Mp3Pcm;
export 'src/wav_io.dart' show WavData, readWavPcm16, wavToMonoFloat;

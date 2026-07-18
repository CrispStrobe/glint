// Ogg-Vorbis decode example: decodes a .ogg with the dedicated
// GlintVorbisDecoder AND the whole-file auto-detect path (which routes
// Vorbis via the C detect), and checks they agree.
//
// usage: dart run example/vorbis_decode.dart <in.ogg>
import 'dart:io';
import 'dart:typed_data';
import 'package:glint_audio/glint_audio.dart';

void main(List<String> args) {
  if (args.isEmpty) {
    print('usage: dart run example/vorbis_decode.dart <in.ogg>');
    exit(2);
  }
  final ogg = Uint8List.fromList(File(args[0]).readAsBytesSync());

  // Dedicated Vorbis decoder.
  final v = GlintVorbisDecoder().decode(ogg);
  print('GlintVorbisDecoder: ${ogg.length} B -> ${v.pcm.length ~/ v.channels}'
      ' frames, ${v.sampleRate} Hz, ${v.channels} ch');
  if (v.pcm.isEmpty) throw StateError('no PCM decoded');

  // Whole-file auto-detect path must decode the same Vorbis stream.
  final w = glintDecodeAudio(ogg);
  print('glintDecodeAudio (auto-detect): '
      '${w.pcm.length ~/ w.channels} frames, ${w.sampleRate} Hz, '
      '${w.channels} ch');
  if (w.sampleRate != v.sampleRate || w.channels != v.channels ||
      w.pcm.length != v.pcm.length) {
    throw StateError('dedicated and auto-detect paths disagree');
  }
  print('OK');
}

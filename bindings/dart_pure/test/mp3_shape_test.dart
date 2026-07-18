// Stage-by-stage A/B of the pure-Dart MP3 quantizer/psycho/NMR loop against
// glint. Feeds the EXACT mdct_flat[576] glint saw (test/fixtures/mp3/) to
// mp3QuantizeGranule and compares the observable GranuleInfo boundary:
// global_gain, scalefac[21], scalefac_scale, preflag. glint's internal Huffman
// region optimizer isn't ported, so part2_3_length/ix differ slightly; the
// psychoacoustic DECISIONS (which sfbs get amplified) are what must match.

import 'dart:io';

import 'package:glint_audio_pure/src/mp3_shape.dart';
import 'dart:typed_data';
import 'package:test/test.dart';

const _dir = 'test/fixtures/mp3';

Float64List _loadMdct(String sig) {
  final lines = File('$_dir/mdct_$sig.txt').readAsLinesSync()
    ..removeWhere((l) => l.trim().isEmpty);
  final v = Float64List(576);
  for (var i = 0; i < 576; i++) {
    v[i] = double.parse(lines[i].trim());
  }
  return v;
}

Map<String, dynamic> _loadGi(String sig) {
  final gi = <String, dynamic>{'scalefac': List<int>.filled(21, 0)};
  for (final l in File('$_dir/gi_$sig.txt').readAsLinesSync()) {
    final parts = l.split(' ');
    if (parts.length < 2) continue;
    final k = parts[0];
    if (k.startsWith('sf') &&
        k.length > 2 &&
        int.tryParse(k.substring(2)) != null) {
      (gi['scalefac'] as List<int>)[int.parse(k.substring(2))] =
          int.parse(parts[1]);
    } else if (k == 'ix') {
      gi['ix'] = parts.sublist(1).map(int.parse).toList();
    } else {
      gi[k] = int.tryParse(parts[1]) ?? parts[1];
    }
  }
  return gi;
}

void main() {
  const grBits = 1584; // matches the fixtures (gr_bits)

  for (final sig in ['noise', 'tone', 'chord', 'speech']) {
    test('quantizer matches glint decisions — $sig', () {
      final mdct = _loadMdct(sig);
      final ref = _loadGi(sig);
      final got = mp3QuantizeGranule(mdct, grBits, 0);

      final refSf = ref['scalefac'] as List<int>;
      final refGain = ref['global_gain'] as int;

      // 1) global_gain within a couple of steps (region optimizer differs).
      expect(
        (got.globalGain - refGain).abs(),
        lessThanOrEqualTo(4),
        reason: '$sig global_gain: got ${got.globalGain} vs glint $refGain',
      );

      // 2) Which sfbs get amplified — the psychoacoustic decision. For the
      // tonal signals glint shapes the HF bands; we must shape the SAME bands.
      final refShaped = {
        for (var b = 0; b < 21; b++)
          if (refSf[b] > 0) b,
      };
      final gotShaped = {
        for (var b = 0; b < 21; b++)
          if (got.scalefac[b] > 0) b,
      };
      if (refShaped.isEmpty) {
        // Flat/masked signal: we must not over-shape into a worse stream.
        expect(
          gotShaped.length,
          lessThanOrEqualTo(3),
          reason: '$sig should stay ~flat, shaped $gotShaped',
        );
      } else {
        // Overlap: at least half of glint's shaped bands are shaped by us too.
        final overlap = refShaped.intersection(gotShaped).length;
        expect(
          overlap,
          greaterThanOrEqualTo((refShaped.length / 2).ceil()),
          reason: '$sig shaped bands: got $gotShaped vs glint $refShaped',
        );
      }

      // 3) The output is a usable granule that fits the budget.
      expect(got.part23Length, lessThanOrEqualTo(grBits));
      expect(got.ix.length, 576);
    });
  }

  test('scalefac_scale flips to 1 on a strongly tonal signal (like glint)', () {
    // glint sets scalefac_scale=1 for tone/chord.
    final tone = mp3QuantizeGranule(_loadMdct('tone'), grBits, 0);
    final chord = mp3QuantizeGranule(_loadMdct('chord'), grBits, 0);
    expect(
      tone.scalefacScale == 1 || chord.scalefacScale == 1,
      isTrue,
      reason: 'expected at least one of tone/chord to use scalefac_scale=1',
    );
  });
}

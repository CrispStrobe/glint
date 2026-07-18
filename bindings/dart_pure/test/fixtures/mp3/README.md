# MP3 quantizer A/B fixtures (vs glint)

Frozen reference for the pure-Dart MP3 quantizer port. Each signal has:
- `mdct_<sig>.txt` — 576 `mdct_flat` coefficients (band-major, post-alias),
  produced by the app's own subband+MDCT (`bin/gen_mdct.dart`), granule 40.
- `gi_<sig>.txt` — glint's `GranuleInfo` for that exact input at gr_bits=1584,
  quality=best(2), long block, from `bench/glint_quant.cpp` +
  `~/code/glint/build/libglint.a`.

Signals: `tone`/`chord` exercise the NMR scalefactor-shaping loop (glint
amplifies HF sfbs); `noise`/`speech` stay flat (sf all-zero). Regenerate:
`c++ -O3 -std=c++17 bench/glint_quant.cpp -I~/code/glint/src ~/code/glint/build/libglint.a -o glint_quant`
then `dart run bin/gen_mdct.dart <sig> mdct_<sig>.txt && glint_quant mdct_<sig>.txt 1584 2 0 > gi_<sig>.txt`.

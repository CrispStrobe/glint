# glint — working notes

Clean-room MP3 encoder in C++17. See `README.md` for the full feature list,
architecture, and CLI/API usage.

## Git workflow

- **Merge directly to `main`. Do not open pull requests.** Commit (on a short
  branch if you like, but it's not required), then fast-forward / merge into
  `main` and `git push origin main`.

## Build & test

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cd build && ctest --output-on-failure        # or run ./glint_test directly
```

Signal-path modes (`-DGLINT_MODE=`): `double` (default), `fixed` (Q31, no FPU),
`both` (runtime `-p` switch). The fixed-point build has its own unit-test count.

## Encoding paths and the short-block invariant

There are six CBR combinations that must all stay correct:
`{double, fixed} × {-q speed, -q normal, -q best}`.

- Short blocks (`block_type == 2`) are only emitted when `quality_mode >= 1`
  (i.e. `-q normal`/`-q best`) and only on the **double** path. The fixed path
  always uses long blocks.
- **Invariant:** the gain search in `quantize_granule()` must count bits with
  the *same* Huffman region layout that the bitstream actually uses. Long and
  short blocks have different layouts (`huffman_determine_regions` vs
  `huffman_determine_regions_short`); the `short_block` flag threads this
  through. If you fit the gain with one layout and encode with another,
  `part2_3_length` can balloon past the 12-bit side-info field (max 4095),
  wrap, and desync the decoder ("invalid new backstep" on decode).
- A budget-guarantee loop at the end of `quantize_granule()` /
  `quantize_granule_vbr()` coarsens `global_gain` until `part2_3_length` fits
  both the bit budget and the 12-bit field. Keep it as a safety net.

## Verifying bitstream correctness

The bit reservoir is currently disabled (`main_data_begin == 0`), so every
frame must be self-contained. To check a build doesn't emit corrupt frames:

```bash
ffmpeg -v error -i out.mp3 -f null -   # any "invalid new backstep" => broken
```

Known open issue: **VBR** has a separate, pre-existing backstep problem (visible
even in `-q speed` VBR, which uses no short blocks). It stems from the disabled
bit reservoir, not from block switching. Out of scope until the reservoir is
re-enabled/fixed.

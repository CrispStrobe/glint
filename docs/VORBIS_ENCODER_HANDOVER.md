# Handover: a clean-room Ogg-Vorbis I **encoder** for glint

**Status:** unclaimed, and *deliberately not started*. Sibling of the finished
decoder track in `PLAN.md` (**# Vorbis track**, slices 1–7, merged 2026-07-19)
— read that section first, because it is both the model for how this gets
built and the tool that verifies it.

---

## Read this before writing any code: should this exist?

**Probably not yet.** Be talked out of it cheaply rather than expensively.

glint already encodes MP3, AAC-LC and Opus. For *audio export* Vorbis is
strictly redundant: Opus beats it at every bitrate and is the codec Xiph
themselves positioned as its successor. Shipping a Vorbis encoder would give
users a worse option behind the same `.ogg` extension.

There is exactly one use case that Opus cannot cover:

> **Writing `.sf3` SoundFonts.** `.sf3` *is* "sf2 with Vorbis-compressed
> samples". The format is fixed — it is Vorbis or nothing. CometBeat currently
> **reads** `.sf3` (that is why the decoder exists) and cannot write one.

So the honest trigger for this project is: *someone wants to author, shrink or
round-trip SoundFonts.* If that is not on the table, close this document. If it
is, the rest of this is the plan.

A secondary, weaker motive: a legacy target that cannot take Opus. Treat that as
insufficient on its own.

## Clean-room affidavit — non-negotiable

Identical terms to the decoder track. If you cannot work this way, do not take
this task.

- Implement from the **Vorbis I specification** (xiph.org) ONLY.
- **Do not read or copy** libvorbis, libvorbisenc, stb_vorbis, tremor, or
  ffmpeg's codec sources. Not "for reference", not "just the tables".
- ffmpeg / sox(libvorbis) are permitted **strictly as black-box binaries** —
  bytes in, PCM out. Never as source.
- MIT headers on every new file. State the affidavit in `PLAN.md` when you open
  the track, as the decoder track does.

⚠️ **The codebook question is where clean-room encoders usually die, and it does
not apply here — see below.** Do not let anyone tell you the tuned libvorbis
codebooks must be copied. They must not, and they need not.

## Why this is far more tractable than it looks

**Vorbis transmits its codebooks in the setup header.** libvorbis's famously
hand-trained books are *its* private choice, not part of the format. Any
decoder reads whatever books your stream declares. So a clean-room encoder does
not have to reproduce, reverse-engineer or approximate libvorbis's tuning — it
generates **its own** books and ships them in the header.

That single fact converts "reimplement a decade of Xiph tuning" into "write a
VQ trainer and emit correct headers".

The second advantage is bigger than it sounds:

> **glint already has a verified Vorbis decoder.** It is your oracle. Every
> stage of the encoder can be checked by decoding your own output — before any
> third-party decoder is involved. This is exactly how the Opus encoder track
> was built (`tools/test_opus_encoder.py`: libopus's own decoder verifies every
> stream glint emits).

## What already exists (do not rebuild it)

| Needed by the encoder | Already in glint |
| --- | --- |
| MDCT + windowing | `mdct.cpp`, `aac_mdct.cpp`, `opus_mdct.cpp` — three, pick/adapt |
| Psychoacoustic model | `psycho.cpp`, `aac_psy.cpp` (masking curves, ATH, spreading) |
| Bit writer | `bitstream.cpp`; **note Vorbis is LSB-first** — `vorbis_bits.hpp` has the reader, you need the writing counterpart |
| Ogg page muxing + CRC | `opus_ogg.cpp` (`glint::opus::ogg_crc`, page writer) |
| Codebook **decode**, VQ lookup 1/2 | `vorbis_decoder.cpp` §3.2.1 — the exact inverse of what you must emit |
| Floor 0/1, residue 0/1/2 **decode** | same file — read it as the specification of your output |
| Ogg-Vorbis demux | `vorbis_ogg.hpp` |
| Decode CLI (harness) | `tools/vorbis_dec_cli.cpp` |
| Decoder gate | `tools/test_vorbis_decoder.py`, ctest `vorbis_decoder_vs_ffmpeg` |

`src/vorbis_decoder.cpp` is ~1200 lines and covers codebooks (scalar + VQ
lookup 1/2), floors 0 and 1, residues 0/1/2 and inverse channel coupling. **You
are writing the mirror of a file you already own.**

## Staging — each slice green before the next

Same discipline as the decoder track. Every slice ends with a gate, not a
"looks right".

- **Slice 1 — Ogg + the three headers.** LSB-first `BitWriter` (mirror of
  `vorbis_bits.hpp`), identification / comment / setup headers, correct page
  framing (id header alone on the first page). **Gate:** glint's own decoder
  parses your headers and reports the right channels/rate/blocksizes; ffmpeg
  accepts the file as Vorbis even with no audio packets.
- **Slice 2 — codebook GENERATION + emission.** The genuinely new component.
  Start with the simplest legal books (scalar Huffman from measured symbol
  statistics, no VQ). **Gate:** `read_codebook` round-trips every book you
  emit — write books, decode them back, assert structural equality. This is a
  pure in-repo gate needing no external tool, so build it first and lean on it.
- **Slice 3 — floor 1 fitting.** Choose line-segment breakpoints approximating
  the spectral envelope. This is a fitting/search problem, not a spec problem.
  **Do NOT implement floor 0** (see gotchas). **Gate:** decoded floor curve
  matches the curve you intended within tolerance.
- **Slice 4 — residue + first end-to-end audio.** Residue type 2 with a simple
  partition classifier is enough to start; mono first. **Gate:** glint decodes
  your stream and the PCM correlates with the input. Expect this slice to be
  where the real bugs live.
- **Slice 5 — the reference gate.** ffmpeg **and** sox must both decode your
  stream. Two independent references, exactly as the decoder track used them.
  Add a ctest `vorbis_encoder_vs_reference` with an SNR/NMR floor.
- **Slice 6 — stereo + coupling.** Square-polar coupling per spec. **Gate:**
  hard-panned L/R neither collapses nor swaps — this is the classic silent
  failure and the decoder track's own tooling already measures it.
- **Slice 7 — quality tuning.** Wire the psychoacoustic model into the residue
  quantisation and iterate on `tests/measure_audio.py` (SNR / NMR / ODG), the
  same instrument every other glint codec is judged with. **Judge by NMR, not
  SNR** — see the MP3/Opus tracks for why raw SNR misleads here.
- **Slice 8 — `.sf3` end to end.** The actual point: encode SoundFont samples,
  write a `.sf3`, and have CometBeat read it back in tune. Only now is this
  worth shipping.

## Gates and invariants

- **Three references, in increasing order of authority:** glint's own decoder
  (fastest loop) → ffmpeg → sox(libvorbis). Ship nothing that only glint can
  decode; that is how you enshrine a shared misreading of the spec.
- **`sox`, not ffmpeg, is the libvorbis driver for corpus work.** ffmpeg 8.1 on
  the maintainer's box has only the experimental native Vorbis encoder and
  fails to emit. `sox in.wav -C <q> out.ogg` drives real libvorbis. (You need
  libvorbis-encoded files as *inputs* for comparison, never as source to read.)
- **Vorbis bit order is LSB-first**, unlike MP3/AAC. The decoder's
  `vorbis_bits.hpp` is the authority; mirror it exactly or every header is
  subtly wrong.
- **Do not implement floor 0.** The decoder supports it, but *no real encoder
  emits it* — libvorbis has used floor 1 exclusively since before 1.0. Emitting
  floor 0 would produce streams nothing in the wild exercises. Floor 1 only.
- **Blocksize/lapping rules are strict.** Window shape depends on the
  previous/next block sizes; get this wrong and you get clicks that SNR will
  show but a casual listen may not.
- **Fuzz what you write, too.** `tools/fuzz_vorbis.cpp` fuzzes the decoder; an
  encoder that can be driven with hostile *PCM* (NaNs, ±inf, denormals, zero
  frames, 1-sample input) deserves the same treatment.

## Scope boundaries — what NOT to do

- **Do not add Vorbis to the audio-export UI.** Opus already covers that better
  and two `.ogg` producers is a UX trap. This encoder exists for `.sf3`.
- **Do not chase libvorbis quality parity.** The bar is "transparent enough for
  SoundFont samples at a sane bitrate", which is a far easier target than
  general music encoding.
- **Do not touch the decoder** except to extend its tests. It is merged, gated
  and depended on by CometBeat.
- **Do not vendor anything into CometBeat until slice 8.** The app's plugin
  (`native/glint`) is vendored verbatim by `sync_glint.sh`; adding an
  unfinished encoder to that closure only grows the binary.

## Realistic cost

Be honest with whoever is funding the time:

- A **minimal correct** encoder (own simple codebooks, floor 1, residue 2, mono
  then stereo, no tuning) is comparable to slices 1–4 of the decoder track.
- A **good** one — competitive with libvorbis at the same bitrate — is a
  project on the scale of glint's AAC track, and most of that is slice 7.
- Slice 2 (codebook generation) is the part with no prior art inside this repo.
  Budget for it accordingly; everything else has a mirror you already own.

If time is tight, **slices 1–5 mono-only** is a legitimate shipping point for
`.sf3` authoring, since SoundFont samples are overwhelmingly mono.

## Done means

- `glint_vorbis_encode` in the C ABI, mirroring `glint_encode_audio`'s shape.
- ffmpeg **and** sox both decode glint's output; ctest gate with an SNR/NMR
  floor, green.
- Hard-panned stereo neither collapses nor swaps.
- Fuzz target for the encoder, green under ASan+UBSan.
- `PLAN.md` gains a **# Vorbis encoder track** section with the affidavit and
  the measured numbers, in the style of the existing tracks.
- CometBeat can write a `.sf3` and read it back in tune — and
  `docs/AUDIO_CODEC_MATRIX.md` there loses its "no Vorbis encoder" row.
- This file is deleted or marked done.

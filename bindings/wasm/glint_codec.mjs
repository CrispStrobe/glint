// Ergonomic wrapper around glint.mjs (Emscripten). One-shot codec calls:
// interleaved Float32 PCM (±1.0) <-> compressed bytes. format: 0=MP3 1=AAC
// 2=Opus. VORBIS (3) and FLAC (4) are DECODE-ONLY (no encoder) —
// decodeAudio auto-detects them from their headers; the values label decoded
// streams.
import createGlint from './glint.mjs';
let _mod = null;
export async function loadGlint() { if (!_mod) _mod = await createGlint(); return _mod; }
export const FORMAT = { MP3: 0, AAC: 1, OPUS: 2, VORBIS: 3, FLAC: 4 };

// Dedicated whole-buffer Ogg-Vorbis decode (mirrors decodeAudio, calls
// glint_vorbis_decode directly). decodeAudio(bytes) also decodes Vorbis via
// the C auto-detect; this is the explicit path.
export async function decodeVorbis(bytes) {
  const m = await loadGlint();
  const inPtr = m._malloc(bytes.length);
  m.HEAPU8.set(bytes, inPtr);
  const sr = m._malloc(4), ch = m._malloc(4), fr = m._malloc(4);
  const ptr = m._glint_vorbis_decode(inPtr, bytes.length, sr, ch, fr);
  m._free(inPtr);
  if (!ptr) { m._free(sr); m._free(ch); m._free(fr); throw new Error('glint vorbis decode failed'); }
  const sampleRate = m.getValue(sr, 'i32'), channels = m.getValue(ch, 'i32'), frames = m.getValue(fr, 'i32');
  m._free(sr); m._free(ch); m._free(fr);
  const pcm = new Float32Array(m.HEAPF32.buffer, ptr, frames * channels).slice();
  m._glint_free(ptr);
  return { pcm, sampleRate, channels, frames };
}

// Dedicated whole-buffer native FLAC decode. decodeAudio(bytes) also decodes
// FLAC via the C auto-detect; this is the explicit path.
export async function decodeFlac(bytes) {
  const m = await loadGlint();
  const inPtr = m._malloc(bytes.length);
  m.HEAPU8.set(bytes, inPtr);
  const sr = m._malloc(4), ch = m._malloc(4), fr = m._malloc(4);
  const ptr = m._glint_flac_decode(inPtr, bytes.length, sr, ch, fr);
  m._free(inPtr);
  if (!ptr) { m._free(sr); m._free(ch); m._free(fr); throw new Error('glint flac decode failed'); }
  const sampleRate = m.getValue(sr, 'i32'), channels = m.getValue(ch, 'i32'), frames = m.getValue(fr, 'i32');
  m._free(sr); m._free(ch); m._free(fr);
  const pcm = new Float32Array(m.HEAPF32.buffer, ptr, frames * channels).slice();
  m._glint_free(ptr);
  return { pcm, sampleRate, channels, frames };
}

export async function encodeAudio(pcm, channels, sampleRate, format, opts = {}) {
  const { bitrateKbps = 192, vbrQuality = -1, quality = 1 } = opts;
  const m = await loadGlint();
  const frames = pcm.length / channels;
  const pcmPtr = m._malloc(pcm.length * 4);
  m.HEAPF32.set(pcm, pcmPtr >> 2);
  const outSizePtr = m._malloc(4);
  const ptr = m._glint_encode_audio(pcmPtr, frames, channels, sampleRate, format, bitrateKbps, vbrQuality, quality, outSizePtr);
  m._free(pcmPtr);
  if (!ptr) { m._free(outSizePtr); throw new Error('glint encode failed'); }
  const size = m.getValue(outSizePtr, 'i32'); m._free(outSizePtr);
  const out = new Uint8Array(m.HEAPU8.buffer, ptr, size).slice();
  m._glint_free(ptr);
  return out;
}

export async function decodeAudio(bytes) {
  const m = await loadGlint();
  const inPtr = m._malloc(bytes.length);
  m.HEAPU8.set(bytes, inPtr);
  const sr = m._malloc(4), ch = m._malloc(4), fr = m._malloc(4);
  const ptr = m._glint_decode_audio(inPtr, bytes.length, sr, ch, fr);
  m._free(inPtr);
  if (!ptr) { m._free(sr); m._free(ch); m._free(fr); throw new Error('glint decode failed'); }
  const sampleRate = m.getValue(sr, 'i32'), channels = m.getValue(ch, 'i32'), frames = m.getValue(fr, 'i32');
  m._free(sr); m._free(ch); m._free(fr);
  const pcm = new Float32Array(m.HEAPF32.buffer, ptr, frames * channels).slice();
  m._glint_free(ptr);
  return { pcm, sampleRate, channels, frames };
}

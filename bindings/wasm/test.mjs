import { readFileSync } from 'node:fs';
import { encodeAudio, decodeAudio, decodeVorbis, decodeFlac, FORMAT } from './glint_codec.mjs';
const SR = 44100, CH = 2, secs = 1;
const frames = SR * secs;
const pcm = new Float32Array(frames * CH);
for (let i = 0; i < frames; i++) {
  const s = Math.sin(2 * Math.PI * 440 * i / SR) * 0.5;
  pcm[i*2] = s; pcm[i*2+1] = s;
}
function rms(a){ let x=0; for(let i=0;i<a.length;i++) x+=a[i]*a[i]; return Math.sqrt(x/a.length); }
const magic = (b) => Array.from(b.slice(0,4)).map(x=>x.toString(16).padStart(2,'0')).join(' ');
const srcRms = rms(pcm);
for (const [name, fmt] of [['MP3',FORMAT.MP3],['AAC',FORMAT.AAC],['Opus',FORMAT.OPUS]]) {
  const enc = await encodeAudio(pcm, CH, SR, fmt, { bitrateKbps: 192 });
  const dec = await decodeAudio(enc);
  const decRms = rms(dec.pcm);
  const ratio = (encodeAudio ? enc.length : 0);
  console.log(`${name}: encoded ${enc.length}B (magic ${magic(enc)}) -> decoded sr=${dec.sampleRate} ch=${dec.channels} frames=${dec.frames} | srcRMS=${srcRms.toFixed(3)} decRMS=${decRms.toFixed(3)} ${Math.abs(decRms-srcRms)<0.15?'OK':'RMS-DRIFT'}`);
}
// Ogg-Vorbis DECODE path (web is first-class). Vorbis has no encoder here;
// pass a .ogg via argv (e.g. sox in.wav -C 6 out.ogg) to exercise both the
// auto-detect decodeAudio and the dedicated decodeVorbis.
const oggPath = process.argv[2];
if (oggPath && oggPath !== '-') {
  const ogg = new Uint8Array(readFileSync(oggPath));
  const a = await decodeAudio(ogg);       // auto-detect (OggS + \x01vorbis)
  const d = await decodeVorbis(ogg);       // dedicated glint_vorbis_decode
  const ok = a.sampleRate === d.sampleRate && a.channels === d.channels &&
             a.frames === d.frames && d.frames > 0 && rms(d.pcm) > 0.01;
  console.log(`Vorbis: ${ogg.length}B -> sr=${d.sampleRate} ch=${d.channels} ` +
              `frames=${d.frames} decRMS=${rms(d.pcm).toFixed(3)} ` +
              `${ok ? 'OK' : 'FAIL'}`);
  if (!ok) process.exit(1);
} else {
  console.log('Vorbis: (skipped — pass a .ogg path to test decode)');
}
const flacPath = process.argv[3];
if (flacPath) {
  const flac = new Uint8Array(readFileSync(flacPath));
  const a = await decodeAudio(flac);       // auto-detect (fLaC)
  const d = await decodeFlac(flac);        // dedicated glint_flac_decode
  const ok = a.sampleRate === d.sampleRate && a.channels === d.channels &&
             a.frames === d.frames && d.frames > 0 && rms(d.pcm) > 0.01;
  console.log(`FLAC: ${flac.length}B -> sr=${d.sampleRate} ch=${d.channels} ` +
              `frames=${d.frames} decRMS=${rms(d.pcm).toFixed(3)} ` +
              `${ok ? 'OK' : 'FAIL'}`);
  if (!ok) process.exit(1);
} else {
  console.log('FLAC: (skipped — pass an ignored .ogg arg plus a .flac path to test decode)');
}
console.log('done');

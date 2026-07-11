//! Safe Rust wrapper for the glint MP3 encoder.

use glint_sys::*;
use std::slice;

/// A safe wrapper around the glint MP3 encoder.
pub struct Encoder {
    handle: glint_t,
    samples_per_frame: usize,
    channels: usize,
}

// SAFETY: The glint encoder uses no thread-local state and all access
// goes through &mut self, so it is safe to send across threads.
unsafe impl Send for Encoder {}

impl Encoder {
    /// Create a new MP3 encoder.
    ///
    /// Returns an error if the configuration is invalid (e.g. unsupported
    /// sample rate or bitrate).
    pub fn new(sample_rate: u32, channels: u32, bitrate: u32) -> Result<Self, &'static str> {
        unsafe {
            let rc = glint_check_config(sample_rate as i32, bitrate as i32);
            if rc != 0 {
                return Err("unsupported sample rate or bitrate");
            }

            let mode = if channels == 1 { 0 } else { 2 }; // MONO or JOINT
            let cfg = glint_config {
                sample_rate: sample_rate as i32,
                num_channels: channels as i32,
                mode,
                bitrate: bitrate as i32,
                path: 0,    // default
                simd: 0,    // auto
                quality: 1, // NORMAL
                vbr: 0,
                vbr_quality: 0,
            };

            let handle = glint_create(&cfg);
            if handle.is_null() {
                return Err("glint_create returned null");
            }

            let spf = glint_samples_per_frame(handle) as usize;

            Ok(Encoder {
                handle,
                samples_per_frame: spf,
                channels: channels as usize,
            })
        }
    }

    /// Number of samples per channel expected by each call to `encode`.
    pub fn samples_per_frame(&self) -> usize {
        self.samples_per_frame
    }

    /// Number of channels this encoder was configured with.
    pub fn channels(&self) -> usize {
        self.channels
    }

    /// Encode one frame of interleaved 16-bit PCM audio.
    ///
    /// `pcm` should contain `samples_per_frame * channels` samples in
    /// interleaved order. If fewer samples are provided, the remainder
    /// is zero-padded.
    pub fn encode(&mut self, pcm: &[i16]) -> Vec<u8> {
        let spf = self.samples_per_frame;
        let ch = self.channels;

        // De-interleave into per-channel buffers
        let mut channel_bufs: Vec<Vec<i16>> = (0..ch).map(|_| vec![0i16; spf]).collect();
        for (i, &sample) in pcm.iter().enumerate() {
            let c = i % ch;
            let s = i / ch;
            if s < spf {
                channel_bufs[c][s] = sample;
            }
        }

        let ptrs: Vec<*const i16> = channel_bufs.iter().map(|b| b.as_ptr()).collect();
        let mut out_size: i32 = 0;

        unsafe {
            let data = glint_encode(self.handle, ptrs.as_ptr(), &mut out_size);
            if data.is_null() || out_size <= 0 {
                return Vec::new();
            }
            slice::from_raw_parts(data, out_size as usize).to_vec()
        }
    }

    /// Encode one frame of interleaved 32-bit float PCM audio.
    ///
    /// Samples should be in the range [-1.0, 1.0]. Layout is the same
    /// as `encode`.
    pub fn encode_float(&mut self, pcm: &[f32]) -> Vec<u8> {
        let spf = self.samples_per_frame;
        let ch = self.channels;

        let mut channel_bufs: Vec<Vec<f32>> = (0..ch).map(|_| vec![0.0f32; spf]).collect();
        for (i, &sample) in pcm.iter().enumerate() {
            let c = i % ch;
            let s = i / ch;
            if s < spf {
                channel_bufs[c][s] = sample;
            }
        }

        let ptrs: Vec<*const f32> = channel_bufs.iter().map(|b| b.as_ptr()).collect();
        let mut out_size: i32 = 0;

        unsafe {
            let data = glint_encode_float(self.handle, ptrs.as_ptr(), &mut out_size);
            if data.is_null() || out_size <= 0 {
                return Vec::new();
            }
            slice::from_raw_parts(data, out_size as usize).to_vec()
        }
    }

    /// Flush the encoder, returning any remaining MP3 data.
    pub fn flush(&mut self) -> Vec<u8> {
        let mut out_size: i32 = 0;
        unsafe {
            let data = glint_flush(self.handle, &mut out_size);
            if data.is_null() || out_size <= 0 {
                return Vec::new();
            }
            slice::from_raw_parts(data, out_size as usize).to_vec()
        }
    }
}

impl Drop for Encoder {
    fn drop(&mut self) {
        unsafe {
            glint_destroy(self.handle);
        }
    }
}

/// AAC-LC encoder (ADTS output). Same interleaved-PCM conventions as
/// [`Encoder`]; one `encode` call consumes `samples_per_frame()` (1024)
/// samples per channel and returns one ADTS frame. `flush` returns the two
/// tail frames and must be called at end of stream (encoder delay: 2048
/// samples; the first frame is a silence priming frame).
pub struct AacEncoder {
    handle: glint_t,
    samples_per_frame: usize,
    channels: usize,
}

unsafe impl Send for AacEncoder {}

impl AacEncoder {
    /// quality: 0 = speed, 1 = normal (default), 2 = best.
    pub fn new(
        sample_rate: u32,
        channels: u32,
        bitrate: u32,
        quality: u32,
    ) -> Result<Self, &'static str> {
        unsafe {
            let cfg = glint_aac_config {
                sample_rate: sample_rate as i32,
                num_channels: channels as i32,
                bitrate: bitrate as i32,
                quality: quality as i32,
                vbr: 0,
                vbr_quality: 0,
                reserved: [0; 4],
            };
            let handle = glint_aac_create(&cfg);
            if handle.is_null() {
                return Err("glint_aac_create returned null");
            }
            let spf = glint_aac_samples_per_frame(handle) as usize;
            Ok(AacEncoder { handle, samples_per_frame: spf, channels: channels as usize })
        }
    }

    pub fn samples_per_frame(&self) -> usize {
        self.samples_per_frame
    }

    pub fn channels(&self) -> usize {
        self.channels
    }

    /// Encode one frame of interleaved 16-bit PCM (zero-padded if short).
    pub fn encode(&mut self, pcm: &[i16]) -> Vec<u8> {
        let spf = self.samples_per_frame;
        let ch = self.channels;
        let mut channel_bufs: Vec<Vec<i16>> = (0..ch).map(|_| vec![0i16; spf]).collect();
        for (i, &sample) in pcm.iter().enumerate() {
            let c = i % ch;
            let s = i / ch;
            if s < spf {
                channel_bufs[c][s] = sample;
            }
        }
        let ptrs: Vec<*const i16> = channel_bufs.iter().map(|b| b.as_ptr()).collect();
        let mut out_size: i32 = 0;
        unsafe {
            let data = glint_aac_encode(self.handle, ptrs.as_ptr(), &mut out_size);
            if data.is_null() || out_size <= 0 {
                return Vec::new();
            }
            slice::from_raw_parts(data, out_size as usize).to_vec()
        }
    }

    /// Flush the encoder, returning the two tail ADTS frames.
    pub fn flush(&mut self) -> Vec<u8> {
        let mut out_size: i32 = 0;
        unsafe {
            let data = glint_aac_flush(self.handle, &mut out_size);
            if data.is_null() || out_size <= 0 {
                return Vec::new();
            }
            slice::from_raw_parts(data, out_size as usize).to_vec()
        }
    }
}

impl Drop for AacEncoder {
    fn drop(&mut self) {
        unsafe {
            glint_aac_destroy(self.handle);
        }
    }
}

/// Convenience function: encode an entire buffer of interleaved 16-bit PCM
/// to AAC (ADTS) in one call. quality: 0 speed / 1 normal / 2 best.
pub fn encode_pcm_aac(
    pcm: &[i16],
    sample_rate: u32,
    channels: u32,
    bitrate: u32,
    quality: u32,
) -> Vec<u8> {
    let mut enc = match AacEncoder::new(sample_rate, channels, bitrate, quality) {
        Ok(e) => e,
        Err(_) => return Vec::new(),
    };
    let frame_len = enc.samples_per_frame() * enc.channels();
    let mut aac = Vec::new();
    for chunk in pcm.chunks(frame_len) {
        aac.extend(enc.encode(chunk));
    }
    aac.extend(enc.flush());
    aac
}

/// Convenience function: encode an entire buffer of interleaved 16-bit PCM
/// to MP3 in one call.
pub fn encode_pcm(pcm: &[i16], sample_rate: u32, channels: u32, bitrate: u32) -> Vec<u8> {
    let mut enc = match Encoder::new(sample_rate, channels, bitrate) {
        Ok(e) => e,
        Err(_) => return Vec::new(),
    };

    let frame_len = enc.samples_per_frame() * enc.channels();
    let mut mp3 = Vec::new();

    for chunk in pcm.chunks(frame_len) {
        mp3.extend(enc.encode(chunk));
    }
    mp3.extend(enc.flush());
    mp3
}


/// CELT-only Opus encoder: 48 kHz interleaved f32 PCM in, complete Opus
/// packets out. Frame sizes 120/240/480/960 samples per channel.
pub struct OpusEncoder {
    handle: glint_sys::glint_t,
    channels: usize,
}

unsafe impl Send for OpusEncoder {}

impl OpusEncoder {
    pub fn new(channels: u32, bitrate: u32, vbr: bool) -> Result<Self, &'static str> {
        let handle = unsafe {
            glint_sys::glint_opus_enc_create(channels as i32, bitrate as i32, vbr as i32)
        };
        if handle.is_null() {
            return Err("invalid Opus encoder config");
        }
        Ok(OpusEncoder { handle, channels: channels as usize })
    }

    /// Encode one frame of interleaved f32 PCM (len = frame_size * channels,
    /// frame_size in {120, 240, 480, 960}). Returns the packet.
    pub fn encode(&mut self, pcm: &[f32]) -> Result<Vec<u8>, &'static str> {
        let frame = pcm.len() / self.channels;
        let mut out = vec![0u8; 1500];
        let n = unsafe {
            glint_sys::glint_opus_encode(
                self.handle, pcm.as_ptr(), frame as i32, out.as_mut_ptr(), out.len() as i32)
        };
        if n < 0 {
            return Err("opus encode failed (frame must be 120/240/480/960)");
        }
        out.truncate(n as usize);
        Ok(out)
    }

    pub fn final_range(&self) -> u32 {
        unsafe { glint_sys::glint_opus_enc_final_range(self.handle) }
    }
}

impl Drop for OpusEncoder {
    fn drop(&mut self) {
        unsafe { glint_sys::glint_opus_enc_destroy(self.handle) };
    }
}

/// Opus decoder (SILK/CELT/hybrid, PLC, SILK in-band FEC), output rates
/// 8/12/16/24/48 kHz, interleaved f32 PCM out.
pub struct OpusDecoder {
    handle: glint_sys::glint_t,
    channels: usize,
}

unsafe impl Send for OpusDecoder {}

impl OpusDecoder {
    pub fn new(channels: u32, sample_rate: u32) -> Result<Self, &'static str> {
        let handle = unsafe {
            glint_sys::glint_opus_dec_create(channels as i32, sample_rate as i32)
        };
        if handle.is_null() {
            return Err("invalid Opus decoder config");
        }
        Ok(OpusDecoder { handle, channels: channels as usize })
    }

    /// Decode one packet to interleaved f32 PCM.
    pub fn decode(&mut self, packet: &[u8]) -> Result<Vec<f32>, &'static str> {
        let mut pcm = vec![0f32; 2 * 5760];
        let n = unsafe {
            glint_sys::glint_opus_decode(
                self.handle, packet.as_ptr(), packet.len() as i32, pcm.as_mut_ptr(), 5760)
        };
        if n < 0 {
            return Err("opus decode failed");
        }
        pcm.truncate(n as usize * self.channels);
        Ok(pcm)
    }

    /// Conceal a LOST packet of `frame_size` samples per channel;
    /// `next_packet` (the one after the loss) supplies SILK in-band FEC
    /// when present.
    pub fn decode_lost(
        &mut self,
        frame_size: usize,
        next_packet: Option<&[u8]>,
    ) -> Result<Vec<f32>, &'static str> {
        let mut pcm = vec![0f32; 2 * 5760];
        let n = unsafe {
            match next_packet {
                Some(p) => glint_sys::glint_opus_decode_fec(
                    self.handle, p.as_ptr(), p.len() as i32, pcm.as_mut_ptr(),
                    frame_size as i32),
                None => glint_sys::glint_opus_decode_fec(
                    self.handle, std::ptr::null(), 0, pcm.as_mut_ptr(),
                    frame_size as i32),
            }
        };
        if n < 0 {
            return Err("opus concealment failed");
        }
        pcm.truncate(n as usize * self.channels);
        Ok(pcm)
    }

    pub fn final_range(&self) -> u32 {
        unsafe { glint_sys::glint_opus_dec_final_range(self.handle) }
    }
}

impl Drop for OpusDecoder {
    fn drop(&mut self) {
        unsafe { glint_sys::glint_opus_dec_destroy(self.handle) };
    }
}

#[cfg(test)]
mod opus_tests {
    use super::*;

    #[test]
    fn opus_roundtrip_final_range() {
        let mut enc = OpusEncoder::new(2, 96000, false).unwrap();
        let mut dec = OpusDecoder::new(2, 48000).unwrap();
        for f in 0..5 {
            let mut pcm = vec![0f32; 960 * 2];
            for i in 0..960 {
                let t = (f * 960 + i) as f32 / 48000.0;
                pcm[i * 2] = 0.4 * (2.0 * std::f32::consts::PI * 440.0 * t).sin();
                pcm[i * 2 + 1] = 0.3 * (2.0 * std::f32::consts::PI * 660.0 * t).sin();
            }
            let pkt = enc.encode(&pcm).unwrap();
            let out = dec.decode(&pkt).unwrap();
            assert_eq!(out.len(), 960 * 2);
            assert_eq!(enc.final_range(), dec.final_range());
        }
        let lost = dec.decode_lost(960, None).unwrap();
        assert_eq!(lost.len(), 960 * 2);
    }
}


/// A decoded frame's stream parameters.
#[derive(Debug, Clone, Copy)]
pub struct DecFrameInfo {
    pub sample_rate: u32,
    pub channels: u32,
    pub samples: u32,
    pub frame_bytes: u32,
}

macro_rules! frame_decoder {
    ($name:ident, $create:ident, $decode:ident, $info:ident,
     $destroy:ident, $doc:literal) => {
        #[doc = $doc]
        pub struct $name {
            handle: glint_sys::glint_t,
        }
        unsafe impl Send for $name {}
        impl $name {
            pub fn new() -> Result<Self, &'static str> {
                let handle = unsafe { glint_sys::$create() };
                if handle.is_null() {
                    return Err("decoder create failed");
                }
                Ok($name { handle })
            }

            /// Parse one frame header; None if data does not start with a
            /// valid frame sync.
            pub fn frame_info(&self, data: &[u8]) -> Option<DecFrameInfo> {
                let mut fi = glint_sys::GlintDecFrameInfo::default();
                let ok = unsafe {
                    glint_sys::$info(data.as_ptr(), data.len() as i32, &mut fi)
                };
                if ok < 0 {
                    return None;
                }
                Some(DecFrameInfo {
                    sample_rate: fi.sample_rate as u32,
                    channels: fi.channels as u32,
                    samples: fi.samples as u32,
                    frame_bytes: fi.frame_bytes as u32,
                })
            }

            /// Decode ONE frame at data[0]. Returns interleaved f32 PCM
            /// (empty while an MP3 reservoir fills) and the frame info.
            pub fn decode_frame(
                &mut self,
                data: &[u8],
            ) -> Result<(Vec<f32>, DecFrameInfo), &'static str> {
                let mut pcm = vec![0f32; 2 * 1152];
                let mut fi = glint_sys::GlintDecFrameInfo::default();
                let n = unsafe {
                    glint_sys::$decode(self.handle, data.as_ptr(),
                        data.len() as i32, pcm.as_mut_ptr(), &mut fi)
                };
                if n < 0 {
                    return Err("decode failed");
                }
                let ch = fi.channels.max(1) as usize;
                pcm.truncate(n as usize * ch);
                Ok((pcm, DecFrameInfo {
                    sample_rate: fi.sample_rate as u32,
                    channels: fi.channels as u32,
                    samples: n as u32,
                    frame_bytes: fi.frame_bytes as u32,
                }))
            }

            /// Decode a whole stream (walks frames, skips ID3v2). Returns
            /// interleaved f32 PCM.
            pub fn decode(&mut self, data: &[u8]) -> Vec<f32> {
                let mut off = 0usize;
                if data.len() > 10 && &data[..3] == b"ID3" {
                    let sz = ((data[6] as usize & 0x7f) << 21)
                        | ((data[7] as usize & 0x7f) << 14)
                        | ((data[8] as usize & 0x7f) << 7)
                        | (data[9] as usize & 0x7f);
                    off = 10 + sz;
                }
                let mut out = Vec::new();
                while off + 7 <= data.len() {
                    let info = match self.frame_info(&data[off..]) {
                        Some(i) => i,
                        None => {
                            off += 1;
                            continue;
                        }
                    };
                    let fb = info.frame_bytes as usize;
                    if fb == 0 || off + fb > data.len() {
                        break;
                    }
                    if let Ok((pcm, _)) = self.decode_frame(&data[off..off + fb]) {
                        out.extend_from_slice(&pcm);
                    }
                    off += fb;
                }
                out
            }
        }
        impl Drop for $name {
            fn drop(&mut self) {
                unsafe { glint_sys::$destroy(self.handle) };
            }
        }
    };
}

frame_decoder!(Mp3Decoder, glint_mp3_dec_create, glint_mp3_decode,
    glint_mp3_frame_info, glint_mp3_dec_destroy,
    "MPEG-1/2 Layer III decoder (keeps a bit reservoir across frames).");
frame_decoder!(AacDecoder, glint_aac_dec_create, glint_aac_decode,
    glint_aac_frame_info, glint_aac_dec_destroy,
    "ADTS AAC-LC decoder.");

#[cfg(test)]
mod decoder_tests {
    use super::*;

    #[test]
    fn mp3_encode_decode_roundtrip() {
        let mut enc = Encoder::new(44100, 2, 128).unwrap();
        let spf = enc.samples_per_frame();
        let mut stream = Vec::new();
        let mut phase = 0.0f64;
        for _ in 0..40 {
            let mut pcm = vec![0i16; spf * 2];
            for i in 0..spf {
                let s = 0.4 * (2.0 * std::f64::consts::PI * 440.0 * phase
                    / 44100.0).sin();
                phase += 1.0;
                pcm[i * 2] = (s * 20000.0) as i16;
                pcm[i * 2 + 1] = (s * 15000.0) as i16;
            }
            stream.extend_from_slice(&enc.encode(&pcm));
        }
        stream.extend_from_slice(&enc.flush());
        let mut dec = Mp3Decoder::new().unwrap();
        let out = dec.decode(&stream);
        assert!(out.len() > 40000, "decoded {} samples", out.len());
    }
}


// ---------------------------------------------------------------------------
// High-level convenience: resample + whole-file decode (PLAN buckets A+B)
// ---------------------------------------------------------------------------

/// Resample interleaved f32 PCM (±1.0) from `sr_in` to `sr_out` with a
/// Kaiser-windowed sinc kernel (anti-aliased, unity passband). `pcm` is
/// `frames * channels` interleaved samples; returns the resampled buffer.
/// Pass-through (a clone) when the rates match.
pub fn resample(pcm: &[f32], channels: u32, sr_in: u32, sr_out: u32) -> Vec<f32> {
    if channels == 0 || pcm.is_empty() {
        return Vec::new();
    }
    let ch = channels as i32;
    let in_frames = pcm.len() as i32 / ch;
    let mut out_frames: core::ffi::c_int = 0;
    let ptr = unsafe {
        glint_sys::glint_resample(pcm.as_ptr(), in_frames, ch,
            sr_in as i32, sr_out as i32, &mut out_frames)
    };
    if ptr.is_null() {
        return Vec::new();
    }
    let total = out_frames as usize * channels as usize;
    let out = unsafe { std::slice::from_raw_parts(ptr, total) }.to_vec();
    unsafe { glint_sys::glint_free(ptr as *mut core::ffi::c_void) };
    out
}

/// Decoded audio: interleaved f32 PCM plus its stream parameters.
#[derive(Debug, Clone)]
pub struct DecodedAudio {
    pub pcm: Vec<f32>,
    pub sample_rate: u32,
    pub channels: u32,
}

/// Decode a whole encoded stream (MP3 / AAC-LC / Ogg-Opus, format auto-
/// detected from the header) to interleaved f32 PCM. Returns `None` on
/// unrecognized or corrupt input.
pub fn decode_audio(data: &[u8]) -> Option<DecodedAudio> {
    if data.is_empty() {
        return None;
    }
    let mut sr: core::ffi::c_int = 0;
    let mut ch: core::ffi::c_int = 0;
    let mut frames: core::ffi::c_int = 0;
    let ptr = unsafe {
        glint_sys::glint_decode_audio(data.as_ptr(), data.len() as i32,
            &mut sr, &mut ch, &mut frames)
    };
    if ptr.is_null() || ch <= 0 {
        return None;
    }
    let total = frames as usize * ch as usize;
    let pcm = unsafe { std::slice::from_raw_parts(ptr, total) }.to_vec();
    unsafe { glint_sys::glint_free(ptr as *mut core::ffi::c_void) };
    Some(DecodedAudio { pcm, sample_rate: sr as u32, channels: ch as u32 })
}

/// Encode interleaved 48 kHz f32 PCM (±1.0, `frames` per channel, 1-2
/// channels) to a complete Ogg-Opus file (CELT-only, 20 ms frames). `vbr`
/// selects unconstrained VBR. Input MUST be 48 kHz — resample first with
/// [`resample`]. Returns `None` on error.
pub fn encode_opus_file(pcm: &[f32], channels: u32, bitrate_bps: u32,
    vbr: bool) -> Option<Vec<u8>> {
    if channels == 0 || channels > 2 || pcm.is_empty() {
        return None;
    }
    let frames = pcm.len() as i32 / channels as i32;
    let mut out_size: core::ffi::c_int = 0;
    let ptr = unsafe {
        glint_sys::glint_opus_encode_file(pcm.as_ptr(), frames,
            channels as i32, bitrate_bps as i32, vbr as i32, &mut out_size)
    };
    if ptr.is_null() || out_size <= 0 {
        return None;
    }
    let data = unsafe { std::slice::from_raw_parts(ptr, out_size as usize) }
        .to_vec();
    unsafe { glint_sys::glint_free(ptr as *mut core::ffi::c_void) };
    Some(data)
}

/// Output codec for [`encode_audio`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Codec {
    Mp3 = 0,
    Aac = 1,
    Opus = 2,
}

/// One-call encode: interleaved f32 PCM (±1.0) at any rate / 1-2 channels
/// -> a complete MP3 / AAC-LC / Ogg-Opus stream. The input is auto-
/// resampled to a codec-valid rate (Opus->48k, MP3/AAC->nearest
/// supported). `bitrate_kbps` is the CBR/target rate; `vbr_quality`
/// Some(0..9) selects VBR. Returns `None` on error.
pub fn encode_audio(pcm: &[f32], channels: u32, sample_rate: u32,
    codec: Codec, bitrate_kbps: u32, vbr_quality: Option<u32>,
    quality: u32) -> Option<Vec<u8>> {
    if channels == 0 || channels > 2 || pcm.is_empty() {
        return None;
    }
    let frames = pcm.len() as i32 / channels as i32;
    let mut out_size: core::ffi::c_int = 0;
    let ptr = unsafe {
        glint_sys::glint_encode_audio(pcm.as_ptr(), frames, channels as i32,
            sample_rate as i32, codec as i32, bitrate_kbps as i32,
            vbr_quality.map(|q| q as i32).unwrap_or(-1), quality as i32,
            &mut out_size)
    };
    if ptr.is_null() || out_size <= 0 {
        return None;
    }
    let data = unsafe { std::slice::from_raw_parts(ptr, out_size as usize) }
        .to_vec();
    unsafe { glint_sys::glint_free(ptr as *mut core::ffi::c_void) };
    Some(data)
}

/// Read a WAV buffer (PCM 8/16/24/32, IEEE float 32/64, A-law, mu-law,
/// EXTENSIBLE) into interleaved f32 PCM. Returns `None` on malformed or
/// unsupported input.
pub fn read_wav(data: &[u8]) -> Option<DecodedAudio> {
    if data.is_empty() {
        return None;
    }
    let mut sr: core::ffi::c_int = 0;
    let mut ch: core::ffi::c_int = 0;
    let mut frames: core::ffi::c_int = 0;
    let ptr = unsafe {
        glint_sys::glint_wav_read(data.as_ptr(), data.len() as i32,
            &mut sr, &mut ch, &mut frames)
    };
    if ptr.is_null() || ch <= 0 {
        return None;
    }
    let total = frames as usize * ch as usize;
    let pcm = unsafe { std::slice::from_raw_parts(ptr, total) }.to_vec();
    unsafe { glint_sys::glint_free(ptr as *mut core::ffi::c_void) };
    Some(DecodedAudio { pcm, sample_rate: sr as u32, channels: ch as u32 })
}

/// Encode interleaved f32 PCM (±1.0) to a WAV file buffer. `bits`:
/// 8/16/24/32 integer PCM, or 32/64 with `float_fmt` for IEEE float
/// (invalid combos fall back to 16-bit). Returns `None` on error.
pub fn write_wav(pcm: &[f32], channels: u32, sample_rate: u32, bits: u32,
    float_fmt: bool) -> Option<Vec<u8>> {
    if channels == 0 {
        return None;
    }
    let frames = pcm.len() as i32 / channels as i32;
    let mut out_size: core::ffi::c_int = 0;
    let ptr = unsafe {
        glint_sys::glint_wav_write(pcm.as_ptr(), frames, channels as i32,
            sample_rate as i32, bits as i32, float_fmt as i32, &mut out_size)
    };
    if ptr.is_null() || out_size <= 0 {
        return None;
    }
    let data = unsafe { std::slice::from_raw_parts(ptr, out_size as usize) }
        .to_vec();
    unsafe { glint_sys::glint_free(ptr as *mut core::ffi::c_void) };
    Some(data)
}

#[cfg(test)]
mod buckets_ab_tests {
    use super::*;

    #[test]
    fn resample_length_and_passthrough() {
        let n = 4410usize;
        let pcm: Vec<f32> = (0..n)
            .map(|i| (2.0 * std::f32::consts::PI * 200.0 * i as f32 / 44100.0).sin())
            .collect();
        let up = resample(&pcm, 1, 44100, 48000);
        let expect = (n as f64 * 48000.0 / 44100.0).round() as usize;
        assert!((up.len() as i64 - expect as i64).abs() <= 2,
            "got {} want ~{}", up.len(), expect);
        let same = resample(&pcm, 1, 44100, 44100);
        assert_eq!(same.len(), n);
    }

    #[test]
    fn resample_preserves_amplitude() {
        let n = 8820usize;
        let pcm: Vec<f32> = (0..n)
            .map(|i| 0.5 * (2.0 * std::f32::consts::PI * 300.0 * i as f32 / 44100.0).sin())
            .collect();
        let out = resample(&pcm, 1, 44100, 22050);
        let peak = out[100..out.len() - 100].iter().fold(0f32, |m, &x| m.max(x.abs()));
        assert!(peak > 0.45 && peak < 0.55, "peak {}", peak);
    }

    #[test]
    fn decode_audio_mp3_and_aac() {
        // Build a short sine, encode to MP3 and AAC, decode both back.
        let spf = 1152usize;
        let mut mp3enc = Encoder::new(44100, 2, 128).unwrap();
        let mut mp3 = Vec::new();
        let mut aacenc = AacEncoder::new(44100, 2, 128, 1).unwrap();
        let mut aac = Vec::new();
        let mut phase = 0.0f64;
        for _ in 0..50 {
            let mut pcm = vec![0i16; spf * 2];
            for i in 0..spf {
                let s = (0.4 * (2.0 * std::f64::consts::PI * 440.0 * phase / 44100.0).sin()
                    * 20000.0) as i16;
                phase += 1.0;
                pcm[i * 2] = s;
                pcm[i * 2 + 1] = s;
            }
            mp3.extend_from_slice(&mp3enc.encode(&pcm));
            // AAC frame is 1024; feed the first 1024 of this 1152 block.
            let mut af = vec![0i16; 1024 * 2];
            af.copy_from_slice(&pcm[..1024 * 2]);
            aac.extend_from_slice(&aacenc.encode(&af));
        }
        mp3.extend_from_slice(&mp3enc.flush());
        aac.extend_from_slice(&aacenc.flush());

        let d1 = decode_audio(&mp3).expect("mp3 decodes");
        assert_eq!(d1.channels, 2);
        assert!(d1.pcm.len() > 40000, "mp3 {} samples", d1.pcm.len());

        let d2 = decode_audio(&aac).expect("aac decodes");
        assert_eq!(d2.channels, 2);
        assert!(d2.pcm.len() > 40000, "aac {} samples", d2.pcm.len());
    }

    #[test]
    fn encode_audio_all_codecs_odd_rate() {
        // 37 kHz (invalid for MP3/AAC) sine -> each codec, decode back.
        let (sr, ch, n) = (37000u32, 2usize, 37000usize);
        let mut pcm = vec![0f32; n * ch];
        for i in 0..n {
            let v = 0.4 * (2.0 * std::f32::consts::PI * 440.0 * i as f32 / sr as f32).sin();
            pcm[i * 2] = v;
            pcm[i * 2 + 1] = v;
        }
        for (codec, want) in [(Codec::Mp3, 32000u32), (Codec::Aac, 32000),
            (Codec::Opus, 48000)] {
            let data = encode_audio(&pcm, ch as u32, sr, codec, 128, None, 1)
                .expect("encode");
            assert!(data.len() > 1000, "{:?} {}", codec, data.len());
            let d = decode_audio(&data).expect("decode");
            assert_eq!(d.sample_rate, want, "{:?} rate", codec);
            assert_eq!(d.channels, 2, "{:?} ch", codec);
        }
    }

    #[test]
    fn wav_bit_depths_roundtrip() {
        let n = 4096usize;
        let pcm: Vec<f32> = (0..n)
            .map(|i| 0.5 * (2.0 * std::f32::consts::PI * 300.0 * i as f32 / 44100.0).sin())
            .collect();
        for &(bits, flt, tol) in &[(8u32, false, 0.02f32), (16, false, 2e-4),
            (24, false, 1e-5), (32, false, 1e-6), (32, true, 1e-6)] {
            let wav = write_wav(&pcm, 1, 44100, bits, flt).expect("write");
            let a = read_wav(&wav).expect("read");
            assert_eq!((a.sample_rate, a.channels), (44100, 1));
            let err = pcm.iter().zip(a.pcm.iter())
                .map(|(x, y)| (x - y).abs()).fold(0f32, f32::max);
            assert!(err < tol, "{}-bit err {}", bits, err);
        }
    }

    #[test]
    fn opus_file_encode_decode() {
        // 48 kHz f32 sine -> encode_opus_file -> decode_audio.
        let n = 48000usize;
        let mut pcm = vec![0f32; n * 2];
        for i in 0..n {
            let v = 0.4 * (2.0 * std::f32::consts::PI * 440.0 * i as f32 / 48000.0).sin();
            pcm[i * 2] = v;
            pcm[i * 2 + 1] = v;
        }
        let opus = encode_opus_file(&pcm, 2, 96000, false).expect("opus encodes");
        assert_eq!(&opus[..4], b"OggS");
        let dec = decode_audio(&opus).expect("opus decodes");
        assert_eq!((dec.sample_rate, dec.channels), (48000, 2));
        assert!(dec.pcm.len() > 40000 * 2, "opus {} samples", dec.pcm.len());
    }
}

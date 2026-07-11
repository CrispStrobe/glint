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

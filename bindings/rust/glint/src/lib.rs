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
                path: 0, // default
                simd: 0, // auto
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

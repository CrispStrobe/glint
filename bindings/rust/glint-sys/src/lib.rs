//! Raw FFI bindings for glint MP3 encoder.
#![allow(non_camel_case_types)]

use std::os::raw::c_int;

pub type glint_t = *mut std::ffi::c_void;

#[repr(C)]
pub struct glint_config {
    pub sample_rate: c_int,
    pub num_channels: c_int,
    pub mode: c_int,
    pub bitrate: c_int,
    pub path: c_int,
    pub simd: c_int,
    // Missing from the original binding — the C struct has had these since
    // the quality/VBR features landed; omitting them made glint_create read
    // uninitialized memory. Keep in sync with include/glint/glint.h.
    pub quality: c_int,
    pub vbr: c_int,
    pub vbr_quality: c_int,
}

#[repr(C)]
pub struct glint_aac_config {
    pub sample_rate: c_int,
    pub num_channels: c_int,
    pub bitrate: c_int,
    pub quality: c_int,
    pub vbr: c_int,         // 0 = CBR, 1 = constant-quality VBR
    pub vbr_quality: c_int, // 0 (best) .. 9 (smallest), when vbr = 1
    pub reserved: [c_int; 4], // must be zero
}

extern "C" {
    pub fn glint_check_config(sample_rate: c_int, bitrate: c_int) -> c_int;
    pub fn glint_create(cfg: *const glint_config) -> glint_t;
    pub fn glint_samples_per_frame(enc: glint_t) -> c_int;
    pub fn glint_encode(
        enc: glint_t,
        channel_data: *const *const i16,
        out_size: *mut c_int,
    ) -> *const u8;
    pub fn glint_encode_float(
        enc: glint_t,
        channel_data: *const *const f32,
        out_size: *mut c_int,
    ) -> *const u8;
    pub fn glint_encode_int32(
        enc: glint_t,
        channel_data: *const *const i32,
        out_size: *mut c_int,
    ) -> *const u8;
    pub fn glint_flush(enc: glint_t, out_size: *mut c_int) -> *const u8;
    pub fn glint_destroy(enc: glint_t);

    // AAC-LC encoder (since 0.8)
    pub fn glint_version() -> c_int;
    pub fn glint_aac_create(cfg: *const glint_aac_config) -> glint_t;
    pub fn glint_aac_samples_per_frame(enc: glint_t) -> c_int;
    pub fn glint_aac_encode(
        enc: glint_t,
        channel_data: *const *const i16,
        out_size: *mut c_int,
    ) -> *const u8;
    pub fn glint_aac_encode_float(
        enc: glint_t,
        channel_data: *const *const f32,
        out_size: *mut c_int,
    ) -> *const u8;
    pub fn glint_aac_flush(enc: glint_t, out_size: *mut c_int) -> *const u8;
    pub fn glint_aac_destroy(enc: glint_t);

    // Opus codec (CELT encoder + full decoder)
    pub fn glint_opus_enc_create(channels: c_int, bitrate_bps: c_int, vbr: c_int) -> glint_t;
    pub fn glint_opus_encode(
        enc: glint_t,
        pcm: *const f32,
        frame_size: c_int,
        out: *mut u8,
        max_bytes: c_int,
    ) -> c_int;
    pub fn glint_opus_enc_final_range(enc: glint_t) -> u32;
    pub fn glint_opus_enc_destroy(enc: glint_t);

    pub fn glint_opus_dec_create(channels: c_int, sample_rate: c_int) -> glint_t;
    pub fn glint_opus_decode(
        dec: glint_t,
        packet: *const u8,
        len: c_int,
        pcm: *mut f32,
        max_samples: c_int,
    ) -> c_int;
    pub fn glint_opus_decode_fec(
        dec: glint_t,
        packet: *const u8,
        len: c_int,
        pcm: *mut f32,
        frame_size: c_int,
    ) -> c_int;
    pub fn glint_opus_dec_final_range(dec: glint_t) -> u32;
    pub fn glint_opus_dec_destroy(dec: glint_t);

    pub fn glint_opus_ms_dec_create(
        channels: c_int,
        streams: c_int,
        coupled: c_int,
        mapping: *const u8,
        sample_rate: c_int,
    ) -> glint_t;
    pub fn glint_opus_ms_decode(
        dec: glint_t,
        packet: *const u8,
        len: c_int,
        pcm: *mut f32,
        max_samples: c_int,
    ) -> c_int;
    pub fn glint_opus_ms_dec_destroy(dec: glint_t);

    // MP3 + AAC decoders
    pub fn glint_mp3_frame_info(data: *const u8, len: c_int, info: *mut GlintDecFrameInfo) -> c_int;
    pub fn glint_aac_frame_info(data: *const u8, len: c_int, info: *mut GlintDecFrameInfo) -> c_int;
    pub fn glint_mp3_dec_create() -> glint_t;
    pub fn glint_mp3_decode(dec: glint_t, data: *const u8, len: c_int, pcm: *mut f32, info: *mut GlintDecFrameInfo) -> c_int;
    pub fn glint_mp3_dec_destroy(dec: glint_t);
    pub fn glint_aac_dec_create() -> glint_t;
    pub fn glint_aac_decode(dec: glint_t, data: *const u8, len: c_int, pcm: *mut f32, info: *mut GlintDecFrameInfo) -> c_int;
    pub fn glint_aac_dec_destroy(dec: glint_t);

    pub fn glint_resample(
        input: *const f32,
        in_frames: c_int,
        channels: c_int,
        sr_in: c_int,
        sr_out: c_int,
        out_frames: *mut c_int,
    ) -> *mut f32;
    pub fn glint_free(p: *mut core::ffi::c_void);
    pub fn glint_decode_audio(
        data: *const u8,
        len: c_int,
        out_sr: *mut c_int,
        out_ch: *mut c_int,
        out_frames: *mut c_int,
    ) -> *mut f32;
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct GlintDecFrameInfo {
    pub sample_rate: c_int,
    pub channels: c_int,
    pub samples: c_int,
    pub frame_bytes: c_int,
}

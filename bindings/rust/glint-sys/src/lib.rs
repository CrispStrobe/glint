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
}

// glint - Fixed-point MPEG-1 Layer III encoder
// MIT License - Clean-room implementation

#ifndef GLINT_H
#define GLINT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct glint_context* glint_t;

enum glint_mode {
    GLINT_MONO   = 0,
    GLINT_DUAL   = 1,
    GLINT_JOINT  = 2,
    GLINT_STEREO = 3,
};

enum glint_path {
    GLINT_PATH_DEFAULT = 0,  // use compile-time default
    GLINT_PATH_DOUBLE  = 1,  // force double-precision
    GLINT_PATH_FIXED   = 2,  // force fixed-point Q31
};

enum glint_simd {
    GLINT_SIMD_AUTO   = 0,  // detect best available at runtime
    GLINT_SIMD_AVX    = 1,  // force AVX (crashes if unsupported!)
    GLINT_SIMD_SSE2   = 2,  // force SSE2
    GLINT_SIMD_NONE   = 3,  // scalar only
    GLINT_SIMD_NEON   = 4,  // force AArch64 NEON (crashes if unsupported!)
};

enum glint_quality {
    GLINT_QUALITY_SPEED  = 0,  // no masking, fastest
    GLINT_QUALITY_NORMAL = 1,  // gain correction + headroom SF (default)
    GLINT_QUALITY_BEST   = 2,  // multi-factor search + psychoacoustic masking
};

enum glint_vbr {
    GLINT_VBR_OFF = 0,  // CBR (default)
    GLINT_VBR_ON  = 1,  // VBR with quality target
};

struct glint_config {
    int sample_rate;
    int num_channels;
    enum glint_mode mode;
    int bitrate;
    enum glint_path path;  // signal path selection (0 = default)
    enum glint_simd simd;  // SIMD selection (0 = auto-detect)
    enum glint_quality quality;  // quality mode (0 = speed, 1 = normal)
    enum glint_vbr vbr;           // VBR mode (0 = CBR, 1 = VBR)
    int vbr_quality;               // VBR quality 0-9 (0=best, 9=worst), only used when vbr=1
};

// Callback: called with each encoded MP3 frame
typedef void (*glint_write_cb)(const uint8_t* data, int size, void* user_data);

// Set the worker-thread count for the per-granule scale-factor search,
// process-wide. 1 (the default) runs single-threaded. The output bitstream is
// byte-identical regardless of thread count — candidates are reduced in a
// fixed order — so threading is a pure throughput knob with no quality effect.
// Call once before encoding (it (re)creates the shared pool).
void           glint_set_threads(int num_threads);

int            glint_check_config(int sample_rate, int bitrate);
glint_t        glint_create(const struct glint_config* cfg);
glint_t        glint_create_streaming(const struct glint_config* cfg,
                                      glint_write_cb callback, void* user_data);
int            glint_samples_per_frame(glint_t enc);

// Encode one frame. channel_data[ch] points to samples_per_frame samples.
const uint8_t* glint_encode(glint_t enc, const int16_t** channel_data, int* out_size);
const uint8_t* glint_encode_float(glint_t enc, const float** channel_data, int* out_size);
const uint8_t* glint_encode_int32(glint_t enc, const int32_t** channel_data, int* out_size);

const uint8_t* glint_flush(glint_t enc, int* out_size);

// VBR only: fill `buf` with the finalized Xing header frame (frame count,
// byte count, 100-point seek TOC). Call AFTER glint_flush, then overwrite
// the beginning of the output file with it — frame 0 of a VBR stream is a
// silent placeholder of exactly this size. Returns the frame size, or 0
// when not applicable (CBR, no frames emitted, buf too small). Streaming
// consumers that cannot seek may skip this; the placeholder decodes as
// ~26 ms of leading silence.
int            glint_vbr_header(glint_t enc, uint8_t* buf, int buf_capacity);

void           glint_destroy(glint_t enc);

// ---------------------------------------------------------------------------
// AAC-LC encoder (phase 1: long blocks, CBR-average, ADTS output).
// Independent of the MP3 encoder above. One encode call consumes exactly
// glint_aac_samples_per_frame() samples per channel (1024) and returns one
// ADTS frame. Call glint_aac_flush once at end of stream — the MDCT looks
// back one block, so the final 1024 samples are emitted by the flush frame.
// Sample rates: 8000..96000 (the 12 standard AAC rates); 1-2 channels;
// bitrate in kbps.
// ---------------------------------------------------------------------------

typedef struct glint_aac_context* glint_aac_t;

// ZERO-INITIALIZE this struct before filling it (memset or `= {0}`): the
// reserved tail lets future releases add options without breaking the ABI,
// and zeroed reserved fields select defaults.
struct glint_aac_config {
    int sample_rate;
    int num_channels;
    int bitrate;        // kbps
    enum glint_quality quality;  // SPEED = no noise shaping; NORMAL/BEST = psy
                                 // NMR shaping loop (BEST iterates further)
    int reserved[6];    // must be zero
};

// Library version as (major << 16) | (minor << 8) | patch.
int            glint_version(void);

glint_aac_t    glint_aac_create(const struct glint_aac_config* cfg);
int            glint_aac_samples_per_frame(glint_aac_t enc);
const uint8_t* glint_aac_encode(glint_aac_t enc, const int16_t** channel_data, int* out_size);
const uint8_t* glint_aac_encode_float(glint_aac_t enc, const float** channel_data, int* out_size);
const uint8_t* glint_aac_flush(glint_aac_t enc, int* out_size);
void           glint_aac_destroy(glint_aac_t enc);

#ifdef __cplusplus
}
#endif

#endif // GLINT_H

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

struct glint_config {
    int sample_rate;
    int num_channels;
    enum glint_mode mode;
    int bitrate;
    enum glint_path path;  // signal path selection (0 = default)
    enum glint_simd simd;  // SIMD selection (0 = auto-detect)
};

int            glint_check_config(int sample_rate, int bitrate);
glint_t        glint_create(const struct glint_config* cfg);
int            glint_samples_per_frame(glint_t enc);

// Encode one frame. channel_data[ch] points to samples_per_frame samples.
const uint8_t* glint_encode(glint_t enc, const int16_t** channel_data, int* out_size);
const uint8_t* glint_encode_float(glint_t enc, const float** channel_data, int* out_size);
const uint8_t* glint_encode_int32(glint_t enc, const int32_t** channel_data, int* out_size);

const uint8_t* glint_flush(glint_t enc, int* out_size);
void           glint_destroy(glint_t enc);

#ifdef __cplusplus
}
#endif

#endif // GLINT_H

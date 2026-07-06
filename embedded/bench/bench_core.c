/* glint embedded benchmark core (C, portable).
 *
 * Encodes deterministic synthetic PCM through the MP3 and AAC encoders at
 * -q speed (the no-FPU integer hot paths in GLINT_MODE=fixed-style builds)
 * and reports throughput + an FNV-1a checksum of the bitstreams. On
 * platforms with (semihosted) file IO it also dumps the streams so a host
 * can decode-validate them with ffmpeg.
 *
 * Platform glue the target must provide:
 *   uint64_t bench_now_us(void);       // monotonic microseconds (0 if none)
 *   int      bench_write_files(void);  // nonzero: dump bench_out.{mp3,aac}
 */

#include <glint/glint.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint64_t bench_now_us(void);
extern int bench_write_files(void);

#define BENCH_SR 44100
#define BENCH_SECONDS 4

static uint32_t rng_state = 0x12345678u;
static int16_t rng_sample(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (int16_t)(rng_state >> 17); /* +-16k */
}

/* Integer sine via table so PCM generation itself is FPU-free. */
static const int16_t sin256[64] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739,
    9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151,
    16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683,
    28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113,
    31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678,
    32728, 32757
};
static int16_t isin(uint32_t phase) { /* phase in [0,256) */
    uint32_t q = (phase >> 6) & 3, i = phase & 63;
    int16_t v = (q & 1) ? sin256[63 - i] : sin256[i];
    return (q & 2) ? (int16_t)-v : v;
}

static void gen_block(int16_t* l, int16_t* r, int n, uint32_t* phase) {
    for (int i = 0; i < n; i++) {
        int16_t tone = (int16_t)(isin((*phase) & 255) / 2);
        int16_t noise = (int16_t)(rng_sample() / 8);
        l[i] = (int16_t)(tone + noise);
        r[i] = (int16_t)(tone / 2 + noise);
        *phase += 3; /* ~517 Hz at 44.1k */
    }
}

static uint32_t fnv1a(uint32_t h, const uint8_t* d, int n) {
    for (int i = 0; i < n; i++) h = (h ^ d[i]) * 16777619u;
    return h;
}

static int16_t bufL[1152], bufR[1152];

static void run_one(const char* name, int is_aac) {
    glint_t mp3 = 0;
    glint_aac_t aac = 0;
    int spf;
    if (is_aac) {
        struct glint_aac_config c;
        memset(&c, 0, sizeof(c));
        c.sample_rate = BENCH_SR;
        c.num_channels = 2;
        c.bitrate = 128;
        c.quality = GLINT_QUALITY_SPEED;
        aac = glint_aac_create(&c);
        if (!aac) { printf("%s: create FAILED\n", name); return; }
        spf = glint_aac_samples_per_frame(aac);
    } else {
        struct glint_config c;
        memset(&c, 0, sizeof(c));
        c.sample_rate = BENCH_SR;
        c.num_channels = 2;
        c.mode = GLINT_JOINT;
        c.bitrate = 128;
        c.quality = GLINT_QUALITY_SPEED;
        mp3 = glint_create(&c);
        if (!mp3) { printf("%s: create FAILED\n", name); return; }
        spf = glint_samples_per_frame(mp3);
    }

    FILE* f = 0;
    if (bench_write_files()) {
        f = fopen(is_aac ? "bench_out.aac" : "bench_out.mp3", "wb");
    }

    int frames = (BENCH_SR * BENCH_SECONDS) / spf;
    uint32_t phase = 0, hash = 2166136261u;
    long total = 0;
    rng_state = 0x12345678u;

    uint64_t t0 = bench_now_us();
    for (int fr = 0; fr < frames; fr++) {
        gen_block(bufL, bufR, spf, &phase);
        const int16_t* ch[2] = { bufL, bufR };
        int sz = 0;
        const uint8_t* d = is_aac ? glint_aac_encode(aac, ch, &sz)
                                  : glint_encode(mp3, ch, &sz);
        if (d && sz > 0) {
            hash = fnv1a(hash, d, sz);
            total += sz;
            if (f) fwrite(d, 1, sz, f);
        }
    }
    {
        int sz = 0;
        const uint8_t* d = is_aac ? glint_aac_flush(aac, &sz)
                                  : glint_flush(mp3, &sz);
        if (d && sz > 0) {
            hash = fnv1a(hash, d, sz);
            total += sz;
            if (f) fwrite(d, 1, sz, f);
        }
    }
    uint64_t us = bench_now_us() - t0;
    if (f) fclose(f);
    if (mp3) glint_destroy(mp3);
    if (aac) glint_aac_destroy(aac);

    printf("%s: %d frames, %ld bytes, checksum %08lx",
           name, frames, total, (unsigned long)hash);
    if (us > 0) {
        /* x-realtime in percent, integer math */
        uint64_t audio_us = (uint64_t)BENCH_SECONDS * 1000000u;
        printf(", %lu us -> %lu%% realtime", (unsigned long)us,
               (unsigned long)(audio_us * 100u / us));
    }
    printf("\n");
}

int bench_main(void) {
    printf("glint embedded bench (sr=%d, %ds, stereo 128k, -q speed)\n",
           BENCH_SR, BENCH_SECONDS);
    run_one("mp3", 0);
    run_one("aac", 1);
    printf("bench done\n");
    return 0;
}

# glint ESP-IDF Component

MP3 encoder for ESP32, ESP32-S3, and other ESP-IDF targets.
Uses fixed-point path with small buffers — **~42 KB RAM** footprint.

## Setup

```bash
cd your_esp_project/components
ln -s /path/to/glint/esp-idf glint
```

Or copy the entire `glint/` repo and use `esp-idf/` as the component root.

## Usage

```c
#include "glint/glint.h"

struct glint_config cfg = {
    .sample_rate = 16000,
    .num_channels = 1,
    .mode = GLINT_MONO,
    .bitrate = 64,
};

glint_t enc = glint_create(&cfg);

// Feed I2S microphone samples (int16_t) frame by frame
int16_t mic_buf[576];  // 576 samples for MPEG-2 at 16kHz
const int16_t* ch[] = { mic_buf };
int mp3_size;
const uint8_t* mp3 = glint_encode(enc, ch, &mp3_size);

// Write mp3 data to SD card, send over WiFi/BLE, etc.
```

## Memory

| Item | Size |
|---|---|
| Static tables | 8 KB (in .rodata/flash) |
| pow34 table | 8 KB (GLINT_SMALL_POW34, double) |
| Encoder state | ~30 KB |
| **Total RAM** | **~42 KB** |

ESP32 has 520 KB SRAM — glint uses <10% of it.

## Recommended settings for ESP32

- Sample rate: 16000 Hz (MPEG-2, 1 granule = less computation)
- Bitrate: 32-64 kbps (sufficient for voice)
- Mode: mono
- Quality: speed (no psychoacoustic overhead)

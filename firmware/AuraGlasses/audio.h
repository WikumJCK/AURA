#ifndef AURA_AUDIO_H
#define AURA_AUDIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AUDIO_SAMPLE_RATE 16000
// Manual clip cap; continuous segments rotate at g_cfg.seg_sec.
#define AUDIO_CLIP_MAX_SECONDS 600

// Starts the I2S PDM mic, allocates the PSRAM ring buffer, spawns the
// capture + SD-writer tasks. Call once from setup() after sd_init().
bool audio_init();

// Continuous logging to SD (/rec/rec_*.wav, rotating segments).
bool audio_continuous_start();
bool audio_continuous_stop();
bool audio_continuous_active();

// Manual clips (/rec/clip_*.wav). If continuous logging is running, it
// rotates into a clip file and back — nothing is lost.
bool audio_clip_start();
// Stops the clip; on success copies the finished file path into out_path.
// If the SD card is absent the clip lives in RAM: out_path is set to ""
// and audio_ram_clip()/audio_ram_clip_len() expose the PCM.
bool audio_clip_stop(char *out_path, size_t out_len);
bool audio_clip_active();

float audio_current_sec();           // seconds in the current file/clip
const char *audio_current_file();    // "" when idle
uint32_t audio_dropped_bytes();      // ring overflow counter (SD too slow)

const uint8_t *audio_ram_clip();
size_t audio_ram_clip_len();

#endif  // AURA_AUDIO_H

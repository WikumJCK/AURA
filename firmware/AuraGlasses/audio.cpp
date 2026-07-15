// PDM microphone pipeline for the XIAO ESP32-S3 Sense.
// CLK = GPIO42, DATA = GPIO41, 16 kHz / 16-bit mono.
// On ESP32-S3, PDM RX only exists on I2S0.
//
// capture task ── I2S reads, high-pass + gain ──► PSRAM ring buffer
// writer task ─── drains ring ──► WAV files on SD, rotated per segment
//
// Continuous mode writes /rec/rec_<ts>.wav segments of g_cfg.seg_sec each.
// Manual clips rotate into /rec/clip_<ts>.wav (and back, if continuous
// logging was running). Without an SD card, clips fall back to recording
// linearly into the ring buffer itself (~16 s max).

#include "audio.h"
#include "config.h"
#include "sdcard.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_arduino_version.h>
#include <time.h>

#define MIC_PIN_CLK  42
#define MIC_PIN_DATA 41

#define CHUNK_BYTES (AUDIO_SAMPLE_RATE / 10 * 2)  // 100 ms
#define RING_BYTES  (512 * 1024)                  // ~16 s of headroom for SD stalls
#define BYTES_PER_SEC (AUDIO_SAMPLE_RATE * 2)
// The MSM261 PDM mic sits on a DC offset and records quietly.
#define AUDIO_GAIN 4

#if ESP_ARDUINO_VERSION_MAJOR >= 3
#include <ESP_I2S.h>
static I2SClass s_i2s;
#else
#include <driver/i2s.h>
#endif

// ── State (guarded by s_state_mtx for control-path changes) ─────────────────
static SemaphoreHandle_t s_state_mtx;
static uint8_t *s_ring = NULL;
static volatile size_t s_head = 0, s_tail = 0;  // write/read indexes
static volatile uint32_t s_dropped = 0;

static volatile bool s_pipeline_on = false;  // capture task is pushing samples
static volatile bool s_continuous = false;
static volatile bool s_clip = false;
static volatile bool s_ram_clip_mode = false;  // no-SD fallback

static volatile bool s_rotate_req = false;   // writer: close current, open next
static char s_next_kind = 'r';               // 'r' = rec segment, 'c' = clip

static char s_cur_path[64] = "";
static char s_done_clip[64] = "";            // last finished clip path
static volatile bool s_clip_done = false;
static volatile size_t s_file_bytes = 0;     // PCM bytes in current file
static volatile size_t s_ram_len = 0;

static TaskHandle_t s_cap_task = NULL;
static TaskHandle_t s_wr_task = NULL;

// ── Ring helpers (single producer / single consumer) ────────────────────────
static inline size_t ring_avail() {
  size_t h = s_head, t = s_tail;
  return h >= t ? h - t : RING_BYTES - t + h;
}
static inline size_t ring_free() {
  return RING_BYTES - 1 - ring_avail();
}
static void ring_push(const uint8_t *src, size_t n) {
  if (ring_free() < n) {
    s_dropped += n;
    return;
  }
  size_t h = s_head;
  size_t first = min(n, RING_BYTES - h);
  memcpy(s_ring + h, src, first);
  if (n > first) {
    memcpy(s_ring, src + first, n - first);
  }
  s_head = (h + n) % RING_BYTES;
}

// ── Mic read + conditioning ─────────────────────────────────────────────────
static size_t mic_read(uint8_t *dst, size_t len) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  return s_i2s.readBytes((char *)dst, len);
#else
  size_t got = 0;
  i2s_read(I2S_NUM_0, dst, len, &got, pdMS_TO_TICKS(500));
  return got;
#endif
}

// One-pole high-pass (~20 Hz, kills the mic's DC offset) + digital gain.
static void condition(int16_t *pcm, size_t n) {
  static int32_t px = 0, py = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t x = pcm[i];
    int32_t y = x - px + py - (py >> 7);
    px = x;
    py = y;
    int32_t v = y * AUDIO_GAIN;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    pcm[i] = (int16_t)v;
  }
}

// ── Filenames ────────────────────────────────────────────────────────────────
static void make_path(char *out, size_t len, char kind) {
  const char *prefix = (kind == 'c') ? "clip" : "rec";
  time_t now = time(NULL);
  if (now > 1577836800) {  // real time is set (past 2020)
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(out, len, "/rec/%s_%04d%02d%02d_%02d%02d%02d.wav", prefix, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  } else {
    snprintf(out, len, "/rec/%s_boot_%lu.wav", prefix, (unsigned long)millis());
  }
}

// ── WAV framing ──────────────────────────────────────────────────────────────
static void wav_header(uint8_t *h, uint32_t pcm_len) {
  uint32_t sample_rate = AUDIO_SAMPLE_RATE;
  uint32_t byte_rate = BYTES_PER_SEC;
  uint32_t riff_len = 36 + pcm_len;
  memcpy(h, "RIFF", 4);
  memcpy(h + 4, &riff_len, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  uint32_t fmt_len = 16;
  memcpy(h + 16, &fmt_len, 4);
  uint16_t fmt = 1, channels = 1, block_align = 2, bits = 16;
  memcpy(h + 20, &fmt, 2);
  memcpy(h + 22, &channels, 2);
  memcpy(h + 24, &sample_rate, 4);
  memcpy(h + 28, &byte_rate, 4);
  memcpy(h + 32, &block_align, 2);
  memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &pcm_len, 4);
}

// ── Writer task ──────────────────────────────────────────────────────────────
static File s_file;
static char s_file_kind = 'r';

static void file_open(char kind) {
  make_path(s_cur_path, sizeof(s_cur_path), kind);
  s_file_kind = kind;
  sd_lock();
  s_file = SD.open(s_cur_path, FILE_WRITE);
  if (s_file) {
    uint8_t h[44];
    wav_header(h, 0);  // placeholder, patched on close
    s_file.write(h, 44);
  } else {
    log_e("audio: open %s failed", s_cur_path);
  }
  sd_unlock();
  s_file_bytes = 0;
}

static void file_close() {
  if (!s_file) {
    s_cur_path[0] = 0;
    return;
  }
  sd_lock();
  uint8_t h[44];
  wav_header(h, (uint32_t)s_file_bytes);
  s_file.seek(0);
  s_file.write(h, 44);
  s_file.close();
  sd_unlock();
  log_i("audio: closed %s (%u B)", s_cur_path, (unsigned)s_file_bytes);
  if (s_file_kind == 'c') {
    strlcpy(s_done_clip, s_cur_path, sizeof(s_done_clip));
    s_clip_done = true;
  }
  s_cur_path[0] = 0;
}

static void writer_task(void *arg) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(50));
    if (s_ram_clip_mode) {
      continue;  // no-SD fallback: samples stay in the ring
    }

    bool active = s_pipeline_on;
    size_t avail = ring_avail();

    if (!s_file && (active && (s_continuous || s_clip))) {
      file_open(s_clip ? 'c' : 'r');
    }

    if (s_file && avail > 0) {
      // Write directly out of the ring, up to the wrap point, 32 KB max.
      size_t t = s_tail;
      size_t n = min(avail, RING_BYTES - t);
      n = min(n, (size_t)32768);
      sd_lock();
      size_t written = s_file.write(s_ring + t, n);
      sd_unlock();
      s_tail = (t + written) % RING_BYTES;
      s_file_bytes += written;
      if (written < n) {
        log_e("audio: SD write short (%u/%u)", (unsigned)written, (unsigned)n);
      }
    }

    // Rotation / shutdown
    size_t seg_target = (size_t)g_cfg.seg_sec * BYTES_PER_SEC;
    size_t clip_cap = (size_t)AUDIO_CLIP_MAX_SECONDS * BYTES_PER_SEC;
    bool seg_full = s_file && s_file_kind == 'r' && s_file_bytes >= seg_target;
    bool clip_full = s_file && s_file_kind == 'c' && s_file_bytes >= clip_cap;

    if (s_rotate_req || seg_full || clip_full) {
      if (s_rotate_req && ring_avail() > 0) {
        continue;  // drain what belongs to the closing file first
      }
      s_rotate_req = false;
      file_close();
      if (clip_full) {
        s_clip = false;
      }
      if (s_pipeline_on && (s_continuous || s_clip)) {
        file_open(s_clip ? 'c' : (s_next_kind == 'c' ? 'c' : 'r'));
        s_next_kind = 'r';
      }
    }

    if (!active && s_file && ring_avail() == 0) {
      file_close();
    }
  }
}

// ── Capture task ─────────────────────────────────────────────────────────────
static void capture_task(void *arg) {
  static uint8_t scratch[CHUNK_BYTES];
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Flush stale DMA data and let the PDM mic settle (~100 ms).
    mic_read(scratch, sizeof(scratch));
    while (s_pipeline_on) {
      size_t got = mic_read(scratch, sizeof(scratch));
      if (!got) {
        continue;
      }
      condition((int16_t *)scratch, got / 2);
      if (s_ram_clip_mode) {
        // Linear record into the ring itself (head only, no wrap).
        size_t space = RING_BYTES - s_ram_len;
        size_t n = min(got, space);
        memcpy(s_ring + s_ram_len, scratch, n);
        s_ram_len += n;
        if (n < got) {
          s_pipeline_on = false;  // RAM full — stop
        }
      } else {
        ring_push(scratch, got);
      }
    }
  }
}

// ── Control API ──────────────────────────────────────────────────────────────
static void pipeline_start() {
  if (s_pipeline_on) {
    return;
  }
  s_head = s_tail = 0;
  s_pipeline_on = true;
  xTaskNotifyGive(s_cap_task);
}

bool audio_init() {
  s_state_mtx = xSemaphoreCreateMutex();
  s_ring = (uint8_t *)heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM);
  if (!s_ring) {
    log_e("audio: ring alloc failed");
    return false;
  }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  s_i2s.setPinsPdmRx(MIC_PIN_CLK, MIC_PIN_DATA);
  if (!s_i2s.begin(I2S_MODE_PDM_RX, AUDIO_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    log_e("audio: I2S PDM RX begin failed");
    return false;
  }
#else
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  cfg.sample_rate = AUDIO_SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) {
    return false;
  }
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_PIN_NO_CHANGE;
  pins.ws_io_num = MIC_PIN_CLK;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = MIC_PIN_DATA;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    return false;
  }
#endif

  xTaskCreatePinnedToCore(capture_task, "aura_mic", 4096, NULL, tskIDLE_PRIORITY + 3, &s_cap_task, 1);
  xTaskCreatePinnedToCore(writer_task, "aura_sdw", 6144, NULL, tskIDLE_PRIORITY + 2, &s_wr_task, 1);
  return s_cap_task && s_wr_task;
}

bool audio_continuous_start() {
  if (!sd_ok()) {
    return false;
  }
  xSemaphoreTake(s_state_mtx, portMAX_DELAY);
  if (!s_continuous) {
    s_continuous = true;
    pipeline_start();
  }
  xSemaphoreGive(s_state_mtx);
  return true;
}

bool audio_continuous_stop() {
  xSemaphoreTake(s_state_mtx, portMAX_DELAY);
  s_continuous = false;
  if (!s_clip) {
    s_pipeline_on = false;  // writer drains the ring, then closes the file
  }
  xSemaphoreGive(s_state_mtx);
  return true;
}

bool audio_continuous_active() {
  return s_continuous;
}

bool audio_clip_start() {
  xSemaphoreTake(s_state_mtx, portMAX_DELAY);
  if (s_clip) {
    xSemaphoreGive(s_state_mtx);
    return false;
  }
  s_clip_done = false;
  if (!sd_ok()) {
    // RAM fallback
    s_ram_len = 0;
    s_ram_clip_mode = true;
    s_clip = true;
    pipeline_start();
    xSemaphoreGive(s_state_mtx);
    return true;
  }
  s_clip = true;
  if (s_pipeline_on) {
    s_rotate_req = true;  // continuous → rotate into a clip file
  } else {
    pipeline_start();
  }
  xSemaphoreGive(s_state_mtx);
  return true;
}

bool audio_clip_stop(char *out_path, size_t out_len) {
  xSemaphoreTake(s_state_mtx, portMAX_DELAY);
  if (!s_clip) {
    xSemaphoreGive(s_state_mtx);
    return false;
  }
  if (s_ram_clip_mode) {
    s_pipeline_on = false;
    s_clip = false;
    vTaskDelay(pdMS_TO_TICKS(150));  // let the capture task finish its read
    s_ram_clip_mode = false;
    if (out_path && out_len) {
      out_path[0] = 0;
    }
    xSemaphoreGive(s_state_mtx);
    return s_ram_len > 0;
  }
  s_clip = false;
  if (s_continuous) {
    s_next_kind = 'r';
    s_rotate_req = true;  // clip → back to a rec segment
  } else {
    s_pipeline_on = false;
  }
  xSemaphoreGive(s_state_mtx);

  // Wait for the writer to drain and close the clip file (SD write speed
  // bound; ring holds ≤16 s ≈ a few seconds of flushing).
  uint32_t start = millis();
  while (!s_clip_done && millis() - start < 20000) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (!s_clip_done) {
    return false;
  }
  if (out_path && out_len) {
    strlcpy(out_path, s_done_clip, out_len);
  }
  return true;
}

bool audio_clip_active() {
  return s_clip;
}

float audio_current_sec() {
  if (s_ram_clip_mode) {
    return (float)s_ram_len / BYTES_PER_SEC;
  }
  return (float)(s_file_bytes + ring_avail()) / BYTES_PER_SEC;
}

const char *audio_current_file() {
  return s_cur_path;
}

uint32_t audio_dropped_bytes() {
  return s_dropped;
}

const uint8_t *audio_ram_clip() {
  return s_ring;
}

size_t audio_ram_clip_len() {
  return s_ram_len;
}

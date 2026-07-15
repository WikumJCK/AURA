// MAX98357A I2S amplifier output for the AURA glasses.
//
// The ESP32-S3 has two I2S controllers: the PDM mic claims I2S0 (PDM RX is
// I2S0-only), so this standard-I2S TX channel lands on I2S1 — both run at
// the same time. Pins: DIN=GPIO1, BCLK=GPIO2, LRC=GPIO3, Vin=5V.
//
// speaker_init() must be called AFTER audio_init() so channel auto-selection
// gives the mic I2S0 first.

#include "speaker.h"

#include <Arduino.h>
#include <math.h>
#include <esp_arduino_version.h>

#define SPK_PIN_DIN  1
#define SPK_PIN_BCLK 2
#define SPK_PIN_LRC  3

#define SPK_DEFAULT_RATE 16000

#if ESP_ARDUINO_VERSION_MAJOR >= 3
#include <ESP_I2S.h>
static I2SClass s_spk;
#else
#error "Speaker support requires ESP32 Arduino core 3.x (ESP_I2S.h)"
#endif

static bool s_ready = false;
static volatile bool s_busy = false;
static uint32_t s_rate = SPK_DEFAULT_RATE;
static int s_vol = 80;
static SemaphoreHandle_t s_mtx = NULL;

bool speaker_init() {
  s_mtx = xSemaphoreCreateMutex();
  s_spk.setPins(SPK_PIN_BCLK, SPK_PIN_LRC, SPK_PIN_DIN, -1, -1);  // bclk, ws, dout
  if (!s_spk.begin(I2S_MODE_STD, SPK_DEFAULT_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    log_e("speaker: I2S STD TX begin failed");
    return false;
  }
  s_ready = true;
  return true;
}

bool speaker_busy() {
  return s_busy;
}

static bool claim(uint32_t rate, int vol) {
  if (!s_ready) {
    return false;
  }
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (s_busy) {
    xSemaphoreGive(s_mtx);
    return false;
  }
  s_busy = true;
  xSemaphoreGive(s_mtx);
  s_vol = constrain(vol, 0, 100);
  if (rate != s_rate) {
    // Reconfigure the TX clock for this clip's sample rate.
    if (s_spk.configureTX(rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
      s_rate = rate;
    } else {
      log_e("speaker: configureTX(%u) failed, staying at %u", rate, s_rate);
    }
  }
  return true;
}

static void release() {
  // Let the DMA ring drain before releasing (~a few frames).
  delay(60);
  s_busy = false;
}

size_t speaker_write(const uint8_t *pcm, size_t len) {
  // Apply software volume in-place-safe chunks, then push to I2S (blocking).
  static int16_t buf[1024];
  const int16_t *src = (const int16_t *)pcm;
  size_t samples = len / 2;
  size_t done = 0;
  while (done < samples) {
    size_t n = min(samples - done, (size_t)1024);
    for (size_t i = 0; i < n; i++) {
      buf[i] = (int16_t)(((int32_t)src[done + i] * s_vol) / 100);
    }
    s_spk.write((uint8_t *)buf, n * 2);
    done += n;
  }
  return len;
}

bool speaker_begin_stream(uint32_t sample_rate, int vol) {
  return claim(sample_rate, vol);
}

void speaker_end_stream() {
  release();
}

bool speaker_tone(int freq_hz, int dur_ms, int vol) {
  if (!claim(SPK_DEFAULT_RATE, vol)) {
    return false;
  }
  freq_hz = constrain(freq_hz, 40, 8000);
  dur_ms = constrain(dur_ms, 20, 10000);
  const int total = (int)((int64_t)SPK_DEFAULT_RATE * dur_ms / 1000);
  static int16_t buf[512];
  int written = 0;
  while (written < total) {
    int n = min(total - written, 512);
    for (int i = 0; i < n; i++) {
      int idx = written + i;
      float amp = 12000.0f;
      // 5 ms fade in/out to avoid clicks
      int fade = SPK_DEFAULT_RATE / 200;
      if (idx < fade) amp *= (float)idx / fade;
      if (total - idx < fade) amp *= (float)(total - idx) / fade;
      buf[i] = (int16_t)(amp * sinf(2.0f * (float)M_PI * freq_hz * idx / SPK_DEFAULT_RATE));
    }
    speaker_write((uint8_t *)buf, n * 2);
    written += n;
  }
  release();
  return true;
}

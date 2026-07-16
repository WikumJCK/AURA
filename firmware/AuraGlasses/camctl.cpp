#include "camctl.h"

#include <Arduino.h>

static SemaphoreHandle_t s_mtx = NULL;
static volatile bool s_photo_pause = false;  // photo in progress
static volatile bool s_stream_open = false;
static volatile bool s_video_idle = true;    // video task parked ack

// Sensor state to restore after a photo.
static framesize_t s_prev_fs = FRAMESIZE_INVALID;
static int s_prev_q = -1;
static bool s_switched = false;

static void apply(framesize_t fs, int quality, int discard) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    return;
  }
  s->set_framesize(s, fs);
  s->set_quality(s, quality);
  // Flush frames captured at the old size / let AE settle.
  for (int i = 0; i < discard; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      esp_camera_fb_return(fb);
    }
  }
}

void camctl_init() {
  s_mtx = xSemaphoreCreateMutex();
}

static void wait_video_idle(uint32_t timeout_ms) {
  uint32_t start = millis();
  while (!s_video_idle && millis() - start < timeout_ms) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

bool camctl_photo_begin(framesize_t fs, int quality) {
  if (!s_mtx) {
    return false;
  }
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  s_photo_pause = true;
  wait_video_idle(400);

  sensor_t *s = esp_camera_sensor_get();
  s_switched = false;
  if (s && (s->status.framesize != fs || s->status.quality != quality)) {
    s_prev_fs = (framesize_t)s->status.framesize;
    s_prev_q = s->status.quality;
    s_switched = true;
    apply(fs, quality, 2);
  }
  return true;
}

void camctl_photo_end() {
  if (s_switched) {
    apply(s_prev_fs, s_prev_q, 1);
    s_switched = false;
  }
  s_photo_pause = false;
  xSemaphoreGive(s_mtx);
}

void camctl_stream_open() {
  s_stream_open = true;
  wait_video_idle(400);
}

void camctl_stream_close() {
  s_stream_open = false;
}

bool camctl_stream_active() {
  return s_stream_open;
}

bool camctl_video_may_run() {
  return !s_photo_pause && !s_stream_open;
}

void camctl_video_set_idle(bool idle) {
  s_video_idle = idle;
}

void camctl_set_sensor(framesize_t fs, int quality, int discard_frames) {
  if (!s_mtx) {
    return;
  }
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  apply(fs, quality, discard_frames);
  xSemaphoreGive(s_mtx);
}

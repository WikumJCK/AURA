// Rolling video buffer + manual AVI recording for the AURA glasses.
//
// Frames are grabbed by one task at VID_FPS while enabled/recording; the
// camera coordinator (camctl) parks this task during photo captures and
// live streams. Rolling frames live in a PSRAM ring; saves and manual
// clips are muxed into minimal MJPEG AVI files (hdrl/movi/idx1) that play
// in VLC/ffmpeg/Windows.

#include "video.h"
#include "camctl.h"
#include "config.h"
#include "sdcard.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <time.h>

#define VID_FRAMESIZE FRAMESIZE_VGA
#define VID_W 640
#define VID_H 480
#define VID_QUALITY 12
#define VID_RING_BYTES (2560 * 1024)  // ~10 s of VGA/q12 at 10 fps
#define VID_MAX_ENTS 256
#define VID_REC_MAX_FRAMES (VID_REC_MAX_SEC * VID_FPS)

// ── Rolling ring (only the video task writes; dumps freeze it) ──────────────
struct VEnt {
  uint32_t off, len;
};
static uint8_t *s_ring = NULL;
static VEnt s_ents[VID_MAX_ENTS];
static int s_e_first = 0, s_e_count = 0;  // FIFO of entries
static size_t s_w_off = 0;
static volatile bool s_enabled = false;
static volatile bool s_freeze = false;

// ── Manual recording state ───────────────────────────────────────────────────
static volatile bool s_rec_req = false;   // ask the task to start
static volatile bool s_rec_stop = false;  // ask the task to stop
static volatile bool s_recording = false;
static volatile uint32_t s_rec_frames = 0;
static char s_rec_path[64] = "";

// Sensor ownership: true while the task has switched the sensor to VGA.
static bool s_sensor_claimed = false;

static void ents_pop() {
  s_e_first = (s_e_first + 1) % VID_MAX_ENTS;
  s_e_count--;
}

static void ring_push_frame(const uint8_t *jpg, uint32_t len) {
  if (!s_ring || len == 0 || len > VID_RING_BYTES / 2) {
    return;
  }
  if (s_w_off + len > VID_RING_BYTES) {
    // Abandon the tail: entries still living there are now the oldest.
    while (s_e_count && s_ents[s_e_first].off >= s_w_off) {
      ents_pop();
    }
    s_w_off = 0;
  }
  // Evict oldest entries overlapping the write range.
  while (s_e_count) {
    VEnt &e = s_ents[s_e_first];
    if (e.off < s_w_off + len && e.off + e.len > s_w_off) {
      ents_pop();
    } else {
      break;
    }
  }
  if (s_e_count == VID_MAX_ENTS) {
    ents_pop();
  }
  memcpy(s_ring + s_w_off, jpg, len);
  int slot = (s_e_first + s_e_count) % VID_MAX_ENTS;
  s_ents[slot].off = (uint32_t)s_w_off;
  s_ents[slot].len = len;
  s_e_count++;
  s_w_off += len;
}

// ── Minimal AVI (MJPG) muxer ─────────────────────────────────────────────────
struct AviMux {
  File f;
  uint32_t frames = 0;
  uint32_t movi_bytes = 0;  // chunk bytes after the 'movi' fourcc
  uint32_t *idx = NULL;     // 4 dwords per frame
  uint32_t idx_cap = 0;
};

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

// Fixed header layout (offsets of the patched fields):
//   4: riff_size | 48: dwTotalFrames (avih+16) | 140: strh dwLength (strh+32)
//   216: movi list size | 220: 'movi' fourcc | 224: first chunk
#define AVI_OFF_RIFF_SIZE 4
#define AVI_OFF_TOTAL_FRAMES 48
#define AVI_OFF_STRH_LENGTH 140
#define AVI_OFF_MOVI_SIZE 216
#define AVI_HDR_BYTES 224

static bool avi_open(AviMux *m, const char *path, uint32_t max_frames) {
  uint8_t h[AVI_HDR_BYTES];
  memset(h, 0, sizeof(h));
  memcpy(h + 0, "RIFF", 4);                       // size @4 patched
  memcpy(h + 8, "AVI LIST", 8);
  put32(h + 16, 192);                             // hdrl list size
  memcpy(h + 20, "hdrlavih", 8);
  put32(h + 28, 56);                              // avih size
  uint8_t *avih = h + 32;
  put32(avih + 0, 1000000 / VID_FPS);             // dwMicroSecPerFrame
  put32(avih + 4, VID_FPS * 40000);               // dwMaxBytesPerSec
  put32(avih + 12, 0x10);                         // AVIF_HASINDEX
  put32(avih + 16, 0);                            // dwTotalFrames @48 patched
  put32(avih + 24, 1);                            // dwStreams
  put32(avih + 28, 65536);                        // dwSuggestedBufferSize
  put32(avih + 32, VID_W);
  put32(avih + 36, VID_H);
  memcpy(h + 88, "LIST", 4);
  put32(h + 92, 116);                             // strl list size
  memcpy(h + 96, "strlstrh", 8);
  put32(h + 104, 56);                             // strh size
  uint8_t *strh = h + 108;
  memcpy(strh + 0, "vids", 4);
  memcpy(strh + 4, "MJPG", 4);
  put32(strh + 20, 1);                            // dwScale
  put32(strh + 24, VID_FPS);                      // dwRate
  put32(strh + 32, 0);                            // dwLength @140 patched
  put32(strh + 36, 65536);                        // dwSuggestedBufferSize
  put32(strh + 40, (uint32_t)-1);                 // dwQuality
  put16(strh + 52, VID_W);                        // rcFrame right
  put16(strh + 54, VID_H);                        // rcFrame bottom
  memcpy(h + 164, "strf", 4);
  put32(h + 168, 40);                             // BITMAPINFOHEADER size
  uint8_t *bmi = h + 172;
  put32(bmi + 0, 40);
  put32(bmi + 4, VID_W);
  put32(bmi + 8, VID_H);
  put16(bmi + 12, 1);                             // biPlanes
  put16(bmi + 14, 24);                            // biBitCount
  memcpy(bmi + 16, "MJPG", 4);
  put32(bmi + 20, VID_W * VID_H * 3);             // biSizeImage
  memcpy(h + 212, "LIST", 4);
  put32(h + 216, 4);                              // movi size @216 patched
  memcpy(h + 220, "movi", 4);

  m->frames = 0;
  m->movi_bytes = 0;
  m->idx_cap = max_frames;
  m->idx = (uint32_t *)heap_caps_malloc(max_frames * 16, MALLOC_CAP_SPIRAM);
  if (!m->idx) {
    return false;
  }
  sd_lock();
  m->f = SD.open(path, FILE_WRITE);
  bool ok = m->f && m->f.write(h, sizeof(h)) == sizeof(h);
  sd_unlock();
  if (!ok) {
    free(m->idx);
    m->idx = NULL;
  }
  return ok;
}

static bool avi_frame(AviMux *m, const uint8_t *jpg, uint32_t len) {
  if (!m->f || m->frames >= m->idx_cap) {
    return false;
  }
  uint32_t padded = (len + 1) & ~1u;
  uint8_t ch[8];
  memcpy(ch, "00dc", 4);
  put32(ch + 4, len);
  sd_lock();
  bool ok = m->f.write(ch, 8) == 8 && m->f.write(jpg, len) == len;
  if (ok && padded != len) {
    uint8_t z = 0;
    ok = m->f.write(&z, 1) == 1;
  }
  sd_unlock();
  if (!ok) {
    return false;
  }
  uint32_t *e = m->idx + m->frames * 4;
  memcpy(e, "00dc", 4);
  e[1] = 0x10;                    // AVIIF_KEYFRAME
  e[2] = 4 + m->movi_bytes;      // from the 'movi' fourcc
  e[3] = len;
  m->movi_bytes += 8 + padded;
  m->frames++;
  return true;
}

static bool avi_close(AviMux *m) {
  if (!m->f) {
    if (m->idx) { free(m->idx); m->idx = NULL; }
    return false;
  }
  uint8_t ch[8];
  memcpy(ch, "idx1", 4);
  put32(ch + 4, m->frames * 16);
  sd_lock();
  bool ok = m->f.write(ch, 8) == 8 &&
            m->f.write((uint8_t *)m->idx, m->frames * 16) == m->frames * 16;
  // File.size() doesn't see unflushed writes — compute the total instead:
  // header + movi chunk data + idx1 chunk.
  uint32_t total = AVI_HDR_BYTES + m->movi_bytes + 8 + m->frames * 16;
  uint8_t v[4];
  put32(v, total - 8);
  m->f.seek(AVI_OFF_RIFF_SIZE); m->f.write(v, 4);
  put32(v, m->frames);
  m->f.seek(AVI_OFF_TOTAL_FRAMES); m->f.write(v, 4);
  m->f.seek(AVI_OFF_STRH_LENGTH); m->f.write(v, 4);
  put32(v, 4 + m->movi_bytes);
  m->f.seek(AVI_OFF_MOVI_SIZE); m->f.write(v, 4);
  m->f.close();
  sd_unlock();
  free(m->idx);
  m->idx = NULL;
  return ok;
}

// ── Filenames ────────────────────────────────────────────────────────────────
static void make_vid_path(char *out, size_t len, const char *prefix) {
  time_t now = time(NULL);
  if (now > 1577836800) {
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(out, len, "/video/%s_%04d%02d%02d_%02d%02d%02d.avi", prefix,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  } else {
    snprintf(out, len, "/video/%s_boot_%lu.avi", prefix, (unsigned long)millis());
  }
}

// ── Capture task ─────────────────────────────────────────────────────────────
static AviMux s_rec_mux;

static void sensor_claim() {
  if (!s_sensor_claimed) {
    camctl_set_sensor(VID_FRAMESIZE, VID_QUALITY, 2);
    s_sensor_claimed = true;
  }
}

static void sensor_release() {
  if (s_sensor_claimed) {
    camctl_set_sensor((framesize_t)g_cfg.framesize, g_cfg.quality, 1);
    s_sensor_claimed = false;
  }
}

static void video_task(void *arg) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000 / VID_FPS));

    bool want = s_enabled || s_rec_req || s_recording;
    if (!want || !camctl_video_may_run()) {
      if (s_recording) {  // stream/photo interrupted a manual recording — end it
        avi_close(&s_rec_mux);
        s_recording = false;
      }
      camctl_video_set_idle(true);
      if (!want && s_sensor_claimed && camctl_video_may_run()) {
        sensor_release();
      }
      continue;
    }
    camctl_video_set_idle(false);
    sensor_claim();

    // Manual recording start/stop requests
    if (s_rec_req && !s_recording) {
      s_rec_req = false;
      make_vid_path(s_rec_path, sizeof(s_rec_path), "clip");
      if (avi_open(&s_rec_mux, s_rec_path, VID_REC_MAX_FRAMES)) {
        s_recording = true;
        s_rec_frames = 0;
      } else {
        log_e("video: avi_open %s failed", s_rec_path);
        s_rec_path[0] = 0;
      }
    }
    if (s_recording && (s_rec_stop || s_rec_mux.frames >= VID_REC_MAX_FRAMES)) {
      s_rec_stop = false;
      avi_close(&s_rec_mux);
      s_recording = false;
      log_i("video: recorded %s (%u frames)", s_rec_path, (unsigned)s_rec_frames);
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      continue;
    }
    if (s_recording) {
      if (!avi_frame(&s_rec_mux, fb->buf, fb->len)) {
        avi_close(&s_rec_mux);
        s_recording = false;
        log_e("video: frame write failed — recording closed");
      }
      s_rec_frames = s_rec_mux.frames;
    } else if (s_enabled && !s_freeze) {
      ring_push_frame(fb->buf, fb->len);
    }
    esp_camera_fb_return(fb);
  }
}

// ── API ──────────────────────────────────────────────────────────────────────
void video_init() {
  xTaskCreatePinnedToCore(video_task, "aura_vid", 8192, NULL, tskIDLE_PRIORITY + 2, NULL, 0);
  if (g_cfg.vid_en) {
    video_set_enabled(true);
  }
}

bool video_set_enabled(bool en) {
  if (en == s_enabled) {
    return true;
  }
  if (en) {
    if (!s_ring) {
      s_ring = (uint8_t *)heap_caps_malloc(VID_RING_BYTES, MALLOC_CAP_SPIRAM);
      if (!s_ring) {
        log_e("video: ring alloc failed (%u KB PSRAM)", VID_RING_BYTES / 1024);
        return false;
      }
    }
    s_e_first = s_e_count = 0;
    s_w_off = 0;
    s_enabled = true;
    log_i("video: rolling buffer on (%u KB)", VID_RING_BYTES / 1024);
  } else {
    s_enabled = false;
    vTaskDelay(pdMS_TO_TICKS(150));  // let an in-flight push finish
    s_e_first = s_e_count = 0;
    s_w_off = 0;
    if (s_ring) {
      free(s_ring);
      s_ring = NULL;
    }
  }
  return true;
}

bool video_enabled() {
  return s_enabled;
}

int video_ring_frames() {
  return s_e_count;
}

float video_ring_sec() {
  return (float)s_e_count / VID_FPS;
}

bool video_save_ring(char *out_path, size_t out_len) {
  if (!s_enabled || !sd_ok()) {
    return false;
  }
  s_freeze = true;
  vTaskDelay(pdMS_TO_TICKS(2 * 1000 / VID_FPS));  // let an in-flight push finish
  int count = s_e_count;
  if (count == 0) {
    s_freeze = false;
    return false;
  }
  char path[64];
  make_vid_path(path, sizeof(path), "vid");
  AviMux m;
  bool ok = avi_open(&m, path, count);
  for (int i = 0; ok && i < count; i++) {
    VEnt &e = s_ents[(s_e_first + i) % VID_MAX_ENTS];
    ok = avi_frame(&m, s_ring + e.off, e.len);
    if ((i & 3) == 3) {
      vTaskDelay(1);  // yield so IDLE0 feeds the task watchdog (5 s limit)
    }
  }
  if (m.f || m.idx) {
    ok = avi_close(&m) && ok;
  }
  s_freeze = false;
  log_i("video: ring dump %s (%d frames) %s", path, count, ok ? "saved" : "FAILED");
  if (ok && out_path && out_len) {
    strlcpy(out_path, path, out_len);
  }
  return ok;
}

bool video_record_start() {
  if (s_recording || s_rec_req) {
    return false;
  }
  if (!sd_ok()) {
    return false;
  }
  s_rec_stop = false;
  s_rec_req = true;
  return true;
}

bool video_record_stop(char *out_path, size_t out_len) {
  if (!s_recording && !s_rec_req) {
    return false;
  }
  s_rec_req = false;
  s_rec_stop = true;
  uint32_t start = millis();
  while (s_recording && millis() - start < 3000) {
    vTaskDelay(pdMS_TO_TICKS(30));
  }
  if (s_recording) {
    return false;
  }
  if (out_path && out_len) {
    strlcpy(out_path, s_rec_path, out_len);
  }
  return s_rec_path[0] != 0;
}

bool video_recording() {
  return s_recording;
}

float video_record_sec() {
  return (float)s_rec_frames / VID_FPS;
}

int video_status_json(char *buf, size_t len) {
  return snprintf(
    buf, len,
    "{\"enabled\":%s,\"rolling_frames\":%d,\"rolling_sec\":%.1f,"
    "\"recording\":%s,\"file\":\"%s\",\"sec\":%.1f,\"max_sec\":%d}\n",
    s_enabled ? "true" : "false", s_e_count, (double)video_ring_sec(),
    s_recording ? "true" : "false", s_recording ? s_rec_path : "",
    (double)video_record_sec(), VID_REC_MAX_SEC
  );
}

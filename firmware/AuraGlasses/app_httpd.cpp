// AURA glasses HTTP server — based on the Espressif CameraWebServer example
// (Apache-2.0), trimmed to the endpoints the AURA app uses and extended with
// audio recording, health, and the embedded web app.
//
// Port 80: /            (AURA web app, gzipped from app_index.h)
//          /icon.png    (home-screen icon)
//          /capture     (single JPEG)
//          /status      (camera settings JSON)
//          /control     (camera settings set)
//          /pad         (touch button state)
//          /health      (device status JSON)
//          /audio/start /audio/stop /audio/status
// Port 81: /stream      (MJPEG)

#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "esp32-hal-ledc.h"
#include "sdkconfig.h"
#include "app_index.h"
#include "board_config.h"
#include "audio.h"
#include "speaker.h"
#include "sdcard.h"
#include "config.h"

#include <WiFi.h>
#include <SD.h>
#include <esp_heap_caps.h>

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#endif

extern "C" {
  int aura_read_pad_d5_raw();
  int aura_read_pad_d6_raw();
}

static esp_err_t pad_status_handler(httpd_req_t *req) {
  // Pads use INPUT_PULLUP: pressed pulls to GND, so pressed == raw 0.
  int d5 = (aura_read_pad_d5_raw() == 0) ? 1 : 0;
  int d6 = (aura_read_pad_d6_raw() == 0) ? 1 : 0;

  char resp[64];
  int n = snprintf(resp, sizeof(resp), "{\"d5\":%d,\"d6\":%d}\n", d5, d6);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, resp, n);
}

static esp_err_t health_handler(httpd_req_t *req) {
  char resp[320];
  int n = snprintf(
    resp, sizeof(resp),
    "{\"ip\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"heap\":%u,\"psram\":%u,"
    "\"recording\":%s,\"continuous\":%s,\"sd\":%s,\"sd_free_mb\":%u,\"sd_total_mb\":%u,\"spk\":%s}\n",
    WiFi.localIP().toString().c_str(), WiFi.SSID().c_str(), WiFi.RSSI(),
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
    audio_clip_active() ? "true" : "false",
    audio_continuous_active() ? "true" : "false",
    sd_ok() ? "true" : "false", sd_free_mb(), sd_total_mb(),
    speaker_busy() ? "true" : "false"
  );
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, resp, n);
}

// ── Recorder + SD endpoints ──────────────────────────────────────────────────

static void json_hdrs(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

// Reject absolute-path tricks and .. traversal on user-supplied SD paths.
static bool safe_path(const char *p) {
  return p[0] == '/' && !strstr(p, "..") && strlen(p) < 60;
}

static bool query_val(httpd_req_t *req, const char *key, char *out, size_t out_len) {
  char q[128];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
    return false;
  }
  if (httpd_query_key_value(q, key, out, out_len) != ESP_OK) {
    return false;
  }
  // Decode %2F etc. (paths arrive URL-encoded)
  char *r = out, *w = out;
  while (*r) {
    if (*r == '%' && r[1] && r[2]) {
      char hex[3] = {r[1], r[2], 0};
      *w++ = (char)strtol(hex, NULL, 16);
      r += 3;
    } else {
      *w++ = (*r == '+') ? ' ' : *r;
      r++;
    }
  }
  *w = 0;
  return true;
}

static esp_err_t rec_start_handler(httpd_req_t *req) {
  json_hdrs(req);
  if (!audio_continuous_start()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "{\"error\":\"SD card required for continuous recording\"}\n");
  }
  return httpd_resp_sendstr(req, "{\"continuous\":true}\n");
}

static esp_err_t rec_stop_handler(httpd_req_t *req) {
  json_hdrs(req);
  audio_continuous_stop();
  return httpd_resp_sendstr(req, "{\"continuous\":false}\n");
}

static esp_err_t rec_status_handler(httpd_req_t *req) {
  char resp[320];
  int n = snprintf(
    resp, sizeof(resp),
    "{\"continuous\":%s,\"clip\":%s,\"sec\":%.1f,\"file\":\"%s\",\"dropped\":%u,"
    "\"sd\":%s,\"sd_free_mb\":%u,\"sd_total_mb\":%u,\"seg_sec\":%u}\n",
    audio_continuous_active() ? "true" : "false", audio_clip_active() ? "true" : "false",
    (double)audio_current_sec(), audio_current_file(), (unsigned)audio_dropped_bytes(),
    sd_ok() ? "true" : "false", sd_free_mb(), sd_total_mb(), g_cfg.seg_sec
  );
  json_hdrs(req);
  return httpd_resp_send(req, resp, n);
}

// Manual clips keep the original /audio/* API the app already uses.
static esp_err_t audio_start_handler(httpd_req_t *req) {
  json_hdrs(req);
  if (!audio_clip_start()) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "{\"error\":\"already recording a clip\"}\n");
  }
  httpd_resp_set_status(req, "202 Accepted");
  char resp[64];
  int n = snprintf(resp, sizeof(resp), "{\"recording\":true,\"max_sec\":%d}\n", AUDIO_CLIP_MAX_SECONDS);
  return httpd_resp_send(req, resp, n);
}

static esp_err_t audio_status_handler(httpd_req_t *req) {
  char resp[96];
  int n = snprintf(
    resp, sizeof(resp), "{\"recording\":%s,\"sec\":%.2f,\"max_sec\":%d}\n",
    audio_clip_active() ? "true" : "false", (double)audio_current_sec(), AUDIO_CLIP_MAX_SECONDS
  );
  json_hdrs(req);
  return httpd_resp_send(req, resp, n);
}

static void wav_header(uint8_t *h, uint32_t pcm_len) {
  uint32_t sample_rate = AUDIO_SAMPLE_RATE;
  uint32_t byte_rate = sample_rate * 2;  // mono 16-bit
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

static esp_err_t send_sd_file(httpd_req_t *req, const char *path) {
  sd_lock();
  File f = SD.open(path, FILE_READ);
  sd_unlock();
  if (!f) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  const char *ext = strrchr(path, '.');
  if (ext && !strcasecmp(ext, ".wav")) {
    httpd_resp_set_type(req, "audio/wav");
  } else if (ext && (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"))) {
    httpd_resp_set_type(req, "image/jpeg");
  } else {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  static uint8_t buf[8192];  // one handler at a time on this httpd instance
  esp_err_t res = ESP_OK;
  while (res == ESP_OK) {
    sd_lock();
    size_t n = f.read(buf, sizeof(buf));
    sd_unlock();
    if (n == 0) {
      break;
    }
    res = httpd_resp_send_chunk(req, (const char *)buf, n);
  }
  sd_lock();
  f.close();
  sd_unlock();
  if (res == ESP_OK) {
    res = httpd_resp_send_chunk(req, NULL, 0);
  }
  return res;
}

static esp_err_t audio_stop_handler(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  char path[64];
  if (!audio_clip_stop(path, sizeof(path))) {
    json_hdrs(req);
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "{\"error\":\"not recording\"}\n");
  }

  if (path[0]) {  // clip landed on the SD card
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=clip.wav");
    return send_sd_file(req, path);
  }

  // RAM fallback (no SD card)
  size_t pcm_len = audio_ram_clip_len();
  const uint8_t *pcm = audio_ram_clip();
  uint8_t header[44];
  wav_header(header, (uint32_t)pcm_len);
  httpd_resp_set_type(req, "audio/wav");
  esp_err_t res = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
  size_t sent = 0;
  while (res == ESP_OK && sent < pcm_len) {
    size_t n = pcm_len - sent;
    if (n > 8192) {
      n = 8192;
    }
    res = httpd_resp_send_chunk(req, (const char *)pcm + sent, n);
    sent += n;
  }
  if (res == ESP_OK) {
    res = httpd_resp_send_chunk(req, NULL, 0);
  }
  return res;
}

static esp_err_t sd_list_handler(httpd_req_t *req) {
  char dir[64] = "/rec";
  query_val(req, "dir", dir, sizeof(dir));
  json_hdrs(req);
  if (!sd_ok() || !safe_path(dir)) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "{\"error\":\"no SD card\"}\n");
  }
  sd_lock();
  File d = SD.open(dir);
  sd_unlock();
  if (!d || !d.isDirectory()) {
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_sendstr(req, "{\"error\":\"no such dir\"}\n");
  }
  esp_err_t res = httpd_resp_send_chunk(req, "[", 1);
  bool first = true;
  int count = 0;
  for (;;) {
    sd_lock();
    File f = d.openNextFile();
    sd_unlock();
    if (!f || count >= 500) {
      if (f) f.close();
      break;
    }
    if (!f.isDirectory()) {
      char item[128];
      int n = snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"size\":%u}", first ? "" : ",", f.name(), (unsigned)f.size());
      first = false;
      count++;
      if (res == ESP_OK) {
        res = httpd_resp_send_chunk(req, item, n);
      }
    }
    f.close();
  }
  d.close();
  if (res == ESP_OK) {
    res = httpd_resp_send_chunk(req, "]", 1);
  }
  if (res == ESP_OK) {
    res = httpd_resp_send_chunk(req, NULL, 0);
  }
  return res;
}

static esp_err_t sd_file_handler(httpd_req_t *req) {
  char path[64];
  if (!query_val(req, "path", path, sizeof(path)) || !safe_path(path) || !sd_ok()) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  return send_sd_file(req, path);
}

static esp_err_t sd_delete_handler(httpd_req_t *req) {
  char path[64];
  json_hdrs(req);
  if (!query_val(req, "path", path, sizeof(path)) || !safe_path(path) || !sd_ok()) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "{\"error\":\"bad path\"}\n");
  }
  sd_lock();
  bool ok = SD.remove(path);
  sd_unlock();
  if (!ok) {
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_sendstr(req, "{\"error\":\"remove failed\"}\n");
  }
  return httpd_resp_sendstr(req, "{\"deleted\":true}\n");
}

// POST /sd/write?path=/logs/x.md&mode=a|w  (body = file content)
static esp_err_t sd_write_handler(httpd_req_t *req) {
  char path[64], mode[4] = "a";
  json_hdrs(req);
  if (!query_val(req, "path", path, sizeof(path)) || !safe_path(path) || !sd_ok()) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "{\"error\":\"bad path\"}\n");
  }
  query_val(req, "mode", mode, sizeof(mode));
  sd_lock();
  File f = SD.open(path, mode[0] == 'w' ? FILE_WRITE : FILE_APPEND);
  sd_unlock();
  if (!f) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_sendstr(req, "{\"error\":\"open failed\"}\n");
  }
  static char body[4096];
  size_t total = 0;
  int remaining = req->content_len;
  while (remaining > 0) {
    int n = httpd_req_recv(req, body, min(remaining, (int)sizeof(body)));
    if (n <= 0) {
      break;
    }
    sd_lock();
    f.write((uint8_t *)body, n);
    sd_unlock();
    total += n;
    remaining -= n;
  }
  sd_lock();
  f.close();
  sd_unlock();
  char resp[48];
  int n = snprintf(resp, sizeof(resp), "{\"written\":%u}\n", (unsigned)total);
  return httpd_resp_send(req, resp, n);
}

// ── Speaker endpoints ────────────────────────────────────────────────────────

// GET /tone?freq=440&ms=500&vol=80 — hardware test / notification beep
static esp_err_t tone_handler(httpd_req_t *req) {
  char v[12];
  int freq = query_val(req, "freq", v, sizeof(v)) ? atoi(v) : 440;
  int ms = query_val(req, "ms", v, sizeof(v)) ? atoi(v) : 500;
  int vol = query_val(req, "vol", v, sizeof(v)) ? atoi(v) : 80;
  json_hdrs(req);
  if (!speaker_tone(freq, ms, vol)) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "{\"error\":\"speaker busy or not initialized\"}\n");
  }
  return httpd_resp_sendstr(req, "{\"played\":true}\n");
}

// POST /speak?vol=80 — body is a 16-bit PCM WAV (mono or stereo), streamed
// straight to the amp as it arrives.
static esp_err_t speak_handler(httpd_req_t *req) {
  char v[12];
  int vol = query_val(req, "vol", v, sizeof(v)) ? atoi(v) : 80;
  json_hdrs(req);

  static uint8_t buf[4096];
  int remaining = req->content_len;
  int got = 0;
  while (got < 64 && got < remaining) {  // enough for header parsing
    int n = httpd_req_recv(req, (char *)buf + got, min(remaining - got, 64 - got));
    if (n <= 0) {
      httpd_resp_set_status(req, "400 Bad Request");
      return httpd_resp_sendstr(req, "{\"error\":\"body read failed\"}\n");
    }
    got += n;
  }
  if (got < 44 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "{\"error\":\"not a WAV file\"}\n");
  }
  uint16_t channels, bits;
  uint32_t rate;
  memcpy(&channels, buf + 22, 2);
  memcpy(&rate, buf + 24, 4);
  memcpy(&bits, buf + 34, 2);
  if (bits != 16 || channels < 1 || channels > 2 || rate < 8000 || rate > 48000) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "{\"error\":\"need 16-bit mono/stereo PCM 8-48kHz\"}\n");
  }
  // Locate the data chunk (standard files: offset 44)
  int data_off = 44;
  for (int i = 36; i < got - 4; i++) {
    if (!memcmp(buf + i, "data", 4)) {
      data_off = i + 8;
      break;
    }
  }

  if (!speaker_begin_stream(rate, vol)) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, "{\"error\":\"speaker busy\"}\n");
  }

  size_t played = 0;
  int fill = got;
  uint8_t carry[4];
  int carry_len = 0;
  int pos = data_off;
  remaining -= got;
  for (;;) {
    if (pos < fill) {
      int n = fill - pos;
      if (channels == 2) {
        // Downmix stereo pairs in place: out[i] = (L+R)/2
        int16_t *s = (int16_t *)(buf + pos);
        int frames = n / 4;
        static int16_t mono[1024];
        int off = 0;
        while (off < frames) {
          int m = min(frames - off, 1024);
          for (int i = 0; i < m; i++) {
            mono[i] = (int16_t)(((int32_t)s[(off + i) * 2] + s[(off + i) * 2 + 1]) / 2);
          }
          speaker_write((uint8_t *)mono, m * 2);
          off += m;
        }
        played += frames * 4;
        // keep any incomplete frame for the next read
        carry_len = n - frames * 4;
        memcpy(carry, buf + pos + frames * 4, carry_len);
      } else {
        int even = n & ~1;
        speaker_write(buf + pos, even);
        played += even;
        carry_len = n - even;
        memcpy(carry, buf + pos + even, carry_len);
      }
    }
    if (remaining <= 0) {
      break;
    }
    memcpy(buf, carry, carry_len);
    int n = httpd_req_recv(req, (char *)buf + carry_len, min(remaining, (int)sizeof(buf) - carry_len));
    if (n <= 0) {
      break;
    }
    remaining -= n;
    fill = carry_len + n;
    pos = 0;
    carry_len = 0;
  }
  speaker_end_stream();

  char resp[64];
  int n = snprintf(resp, sizeof(resp), "{\"played_bytes\":%u,\"rate\":%u}\n", (unsigned)played, (unsigned)rate);
  return httpd_resp_send(req, resp, n);
}

// GET /config → current config; any of the query params below set + persist.
static esp_err_t config_handler(httpd_req_t *req) {
  char v[16];
  bool changed = false;
  if (query_val(req, "cap_interval", v, sizeof(v))) {
    g_cfg.cap_interval = constrain(atoi(v), 0, 3600);
    changed = true;
  }
  if (query_val(req, "framesize", v, sizeof(v))) {
    g_cfg.framesize = constrain(atoi(v), (int)FRAMESIZE_QVGA, (int)FRAMESIZE_UXGA);
    changed = true;
  }
  if (query_val(req, "quality", v, sizeof(v))) {
    g_cfg.quality = constrain(atoi(v), 5, 40);
    changed = true;
  }
  if (query_val(req, "seg_sec", v, sizeof(v))) {
    g_cfg.seg_sec = constrain(atoi(v), 15, 300);
    changed = true;
  }
  if (query_val(req, "rec_auto", v, sizeof(v))) {
    g_cfg.rec_auto = atoi(v) != 0;
    changed = true;
  }
  if (query_val(req, "gmt_off", v, sizeof(v))) {
    g_cfg.gmt_off_min = constrain(atoi(v), -720, 840);
    changed = true;
  }
  if (changed) {
    config_save();
    config_apply_camera();
  }
  // "sizes" carries the enum values of this core build — the app builds its
  // resolution picker from it instead of hardcoding numbers that drift.
  char resp[448];
  int n = snprintf(
    resp, sizeof(resp),
    "{\"cap_interval\":%u,\"framesize\":%u,\"quality\":%u,\"seg_sec\":%u,\"rec_auto\":%s,\"gmt_off\":%d,"
    "\"sizes\":[[\"UXGA 1600\\u00d71200 (max)\",%d],[\"SXGA 1280\\u00d71024\",%d],[\"HD 1280\\u00d7720\",%d],"
    "[\"XGA 1024\\u00d7768\",%d],[\"SVGA 800\\u00d7600\",%d],[\"VGA 640\\u00d7480\",%d]]}\n",
    g_cfg.cap_interval, g_cfg.framesize, g_cfg.quality, g_cfg.seg_sec,
    g_cfg.rec_auto ? "true" : "false", g_cfg.gmt_off_min,
    (int)FRAMESIZE_UXGA, (int)FRAMESIZE_SXGA, (int)FRAMESIZE_HD,
    (int)FRAMESIZE_XGA, (int)FRAMESIZE_SVGA, (int)FRAMESIZE_VGA
  );
  json_hdrs(req);
  return httpd_resp_send(req, resp, n);
}

// ── Camera handlers (from CameraWebServer) ───────────────────────────────────

// LED FLASH setup
#if defined(LED_GPIO_NUM)
#define CONFIG_LED_MAX_INTENSITY 255

int led_duty = 0;
bool isStreaming = false;

#endif

typedef struct {
  httpd_req_t *req;
  size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

#if defined(LED_GPIO_NUM)
void enable_led(bool en) {  // Turn LED On or Off
  int duty = en ? led_duty : 0;
  if (en && isStreaming && (led_duty > CONFIG_LED_MAX_INTENSITY)) {
    duty = CONFIG_LED_MAX_INTENSITY;
  }
  ledcWrite(LED_GPIO_NUM, duty);
  log_i("Set LED intensity to %d", duty);
}
#endif

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len) {
  jpg_chunking_t *j = (jpg_chunking_t *)arg;
  if (!index) {
    j->len = 0;
  }
  if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK) {
    return 0;
  }
  j->len += len;
  return len;
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;

#if defined(LED_GPIO_NUM)
  enable_led(true);
  vTaskDelay(150 / portTICK_PERIOD_MS);  // The LED needs to be turned on ~150ms before the call to esp_camera_fb_get()
  fb = esp_camera_fb_get();              // or it won't be visible in the frame. A better way to do this is needed.
  enable_led(false);
#else
  fb = esp_camera_fb_get();
#endif

  if (!fb) {
    log_e("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char ts[32];
  snprintf(ts, 32, "%lld.%06ld", fb->timestamp.tv_sec, fb->timestamp.tv_usec);
  httpd_resp_set_hdr(req, "X-Timestamp", (const char *)ts);

  if (fb->format == PIXFORMAT_JPEG) {
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  } else {
    jpg_chunking_t jchunk = {req, 0};
    res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk) ? ESP_OK : ESP_FAIL;
    httpd_resp_send_chunk(req, NULL, 0);
  }
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  struct timeval _timestamp;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char *part_buf[128];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Framerate", "60");

#if defined(LED_GPIO_NUM)
  isStreaming = true;
  enable_led(true);
#endif

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      log_e("Camera capture failed");
      res = ESP_FAIL;
    } else {
      _timestamp.tv_sec = fb->timestamp.tv_sec;
      _timestamp.tv_usec = fb->timestamp.tv_usec;
      if (fb->format != PIXFORMAT_JPEG) {
        bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!jpeg_converted) {
          log_e("JPEG compression failed");
          res = ESP_FAIL;
        }
      } else {
        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;
      }
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if (res == ESP_OK) {
      size_t hlen = snprintf((char *)part_buf, 128, _STREAM_PART, _jpg_buf_len, _timestamp.tv_sec, _timestamp.tv_usec);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if (res != ESP_OK) {
      log_e("Send frame failed");
      break;
    }
  }

#if defined(LED_GPIO_NUM)
  isStreaming = false;
  enable_led(false);
#endif

  return res;
}

static esp_err_t parse_get(httpd_req_t *req, char **obuf) {
  char *buf = NULL;
  size_t buf_len = 0;

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char *)malloc(buf_len);
    if (!buf) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      *obuf = buf;
      return ESP_OK;
    }
    free(buf);
  }
  httpd_resp_send_404(req);
  return ESP_FAIL;
}

static esp_err_t cmd_handler(httpd_req_t *req) {
  char *buf = NULL;
  char variable[32];
  char value[32];

  if (parse_get(req, &buf) != ESP_OK) {
    return ESP_FAIL;
  }
  if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) != ESP_OK || httpd_query_key_value(buf, "val", value, sizeof(value)) != ESP_OK) {
    free(buf);
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  free(buf);

  int val = atoi(value);
  log_i("%s = %d", variable, val);
  sensor_t *s = esp_camera_sensor_get();
  int res = 0;

  if (!strcmp(variable, "framesize")) {
    if (s->pixformat == PIXFORMAT_JPEG) {
      res = s->set_framesize(s, (framesize_t)val);
    }
  } else if (!strcmp(variable, "quality")) {
    res = s->set_quality(s, val);
  } else if (!strcmp(variable, "contrast")) {
    res = s->set_contrast(s, val);
  } else if (!strcmp(variable, "brightness")) {
    res = s->set_brightness(s, val);
  } else if (!strcmp(variable, "saturation")) {
    res = s->set_saturation(s, val);
  } else if (!strcmp(variable, "gainceiling")) {
    res = s->set_gainceiling(s, (gainceiling_t)val);
  } else if (!strcmp(variable, "awb")) {
    res = s->set_whitebal(s, val);
  } else if (!strcmp(variable, "agc")) {
    res = s->set_gain_ctrl(s, val);
  } else if (!strcmp(variable, "aec")) {
    res = s->set_exposure_ctrl(s, val);
  } else if (!strcmp(variable, "hmirror")) {
    res = s->set_hmirror(s, val);
  } else if (!strcmp(variable, "vflip")) {
    res = s->set_vflip(s, val);
  } else if (!strcmp(variable, "ae_level")) {
    res = s->set_ae_level(s, val);
  }
#if defined(LED_GPIO_NUM)
  else if (!strcmp(variable, "led_intensity")) {
    led_duty = val;
    if (isStreaming) {
      enable_led(true);
    }
  }
#endif
  else {
    log_i("Unknown command: %s", variable);
    res = -1;
  }

  if (res < 0) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req) {
  static char json_response[512];

  sensor_t *s = esp_camera_sensor_get();
  char *p = json_response;
  *p++ = '{';
  p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
  p += sprintf(p, "\"quality\":%u,", s->status.quality);
  p += sprintf(p, "\"brightness\":%d,", s->status.brightness);
  p += sprintf(p, "\"contrast\":%d,", s->status.contrast);
  p += sprintf(p, "\"saturation\":%d,", s->status.saturation);
  p += sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
  p += sprintf(p, "\"vflip\":%u", s->status.vflip);
#if defined(LED_GPIO_NUM)
  p += sprintf(p, ",\"led_intensity\":%u", led_duty);
#else
  p += sprintf(p, ",\"led_intensity\":%d", -1);
#endif
  *p++ = '}';
  *p++ = 0;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json_response, strlen(json_response));
}

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  return httpd_resp_send(req, (const char *)app_index_html_gz, app_index_html_gz_len);
}

static esp_err_t icon_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "image/png");
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
  return httpd_resp_send(req, (const char *)aura_icon_png, aura_icon_png_len);
}

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 24;
  config.stack_size = 8192;       // JSON + WAV handlers need headroom
  config.lru_purge_enable = true; // iOS Safari opens several sockets

  httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL};
  httpd_uri_t icon_uri = {.uri = "/icon.png", .method = HTTP_GET, .handler = icon_handler, .user_ctx = NULL};
  httpd_uri_t status_uri = {.uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL};
  httpd_uri_t cmd_uri = {.uri = "/control", .method = HTTP_GET, .handler = cmd_handler, .user_ctx = NULL};
  httpd_uri_t capture_uri = {.uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL};
  httpd_uri_t pad_uri = {.uri = "/pad", .method = HTTP_GET, .handler = pad_status_handler, .user_ctx = NULL};
  httpd_uri_t health_uri = {.uri = "/health", .method = HTTP_GET, .handler = health_handler, .user_ctx = NULL};
  httpd_uri_t audio_start_uri = {.uri = "/audio/start", .method = HTTP_GET, .handler = audio_start_handler, .user_ctx = NULL};
  httpd_uri_t audio_stop_uri = {.uri = "/audio/stop", .method = HTTP_GET, .handler = audio_stop_handler, .user_ctx = NULL};
  httpd_uri_t audio_status_uri = {.uri = "/audio/status", .method = HTTP_GET, .handler = audio_status_handler, .user_ctx = NULL};
  httpd_uri_t rec_start_uri = {.uri = "/rec/start", .method = HTTP_GET, .handler = rec_start_handler, .user_ctx = NULL};
  httpd_uri_t rec_stop_uri = {.uri = "/rec/stop", .method = HTTP_GET, .handler = rec_stop_handler, .user_ctx = NULL};
  httpd_uri_t rec_status_uri = {.uri = "/rec/status", .method = HTTP_GET, .handler = rec_status_handler, .user_ctx = NULL};
  httpd_uri_t sd_list_uri = {.uri = "/sd/list", .method = HTTP_GET, .handler = sd_list_handler, .user_ctx = NULL};
  httpd_uri_t sd_file_uri = {.uri = "/sd/file", .method = HTTP_GET, .handler = sd_file_handler, .user_ctx = NULL};
  httpd_uri_t sd_delete_uri = {.uri = "/sd/delete", .method = HTTP_GET, .handler = sd_delete_handler, .user_ctx = NULL};
  httpd_uri_t sd_write_uri = {.uri = "/sd/write", .method = HTTP_POST, .handler = sd_write_handler, .user_ctx = NULL};
  httpd_uri_t config_uri = {.uri = "/config", .method = HTTP_GET, .handler = config_handler, .user_ctx = NULL};
  httpd_uri_t tone_uri = {.uri = "/tone", .method = HTTP_GET, .handler = tone_handler, .user_ctx = NULL};
  httpd_uri_t speak_uri = {.uri = "/speak", .method = HTTP_POST, .handler = speak_handler, .user_ctx = NULL};
  httpd_uri_t stream_uri = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL};

  log_i("Starting web server on port: '%d'", config.server_port);
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &icon_uri);
    httpd_register_uri_handler(camera_httpd, &cmd_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &pad_uri);
    httpd_register_uri_handler(camera_httpd, &health_uri);
    httpd_register_uri_handler(camera_httpd, &audio_start_uri);
    httpd_register_uri_handler(camera_httpd, &audio_stop_uri);
    httpd_register_uri_handler(camera_httpd, &audio_status_uri);
    httpd_register_uri_handler(camera_httpd, &rec_start_uri);
    httpd_register_uri_handler(camera_httpd, &rec_stop_uri);
    httpd_register_uri_handler(camera_httpd, &rec_status_uri);
    httpd_register_uri_handler(camera_httpd, &sd_list_uri);
    httpd_register_uri_handler(camera_httpd, &sd_file_uri);
    httpd_register_uri_handler(camera_httpd, &sd_delete_uri);
    httpd_register_uri_handler(camera_httpd, &sd_write_uri);
    httpd_register_uri_handler(camera_httpd, &config_uri);
    httpd_register_uri_handler(camera_httpd, &tone_uri);
    httpd_register_uri_handler(camera_httpd, &speak_uri);
  }

  config.server_port += 1;
  config.ctrl_port += 1;
  log_i("Starting stream server on port: '%d'", config.server_port);
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

void setupLedFlash() {
#if defined(LED_GPIO_NUM)
  ledcAttach(LED_GPIO_NUM, 5000, 8);
#else
  log_i("LED flash is disabled -> LED_GPIO_NUM undefined");
#endif
}

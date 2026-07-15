// Serialized action worker + event ring for the AURA glasses.
//
// Actions arrive on a queue from the pad task and HTTP handlers; a single
// worker task executes them one at a time and records the outcome in a
// small ring buffer that GET /events serves to the app.

#include "actions.h"
#include "config.h"
#include "audio.h"
#include "speaker.h"
#include "sdcard.h"
#include "led.h"

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

// ── Event ring ───────────────────────────────────────────────────────────────
#define EV_COUNT 8
#define EV_DETAIL 200

struct AuraEvent {
  uint32_t id;        // monotonic, 0 = empty slot
  uint32_t ts;        // epoch seconds (0 if clock not set)
  char type[16];
  bool ok;
  char detail[EV_DETAIL];
};

static AuraEvent s_ev[EV_COUNT];
static uint32_t s_ev_next_id = 1;
static int s_ev_head = 0;  // next slot to write
static portMUX_TYPE s_ev_mux = portMUX_INITIALIZER_UNLOCKED;

void event_push(const char *type, bool ok, const char *detail) {
  time_t now = time(NULL);
  portENTER_CRITICAL(&s_ev_mux);
  AuraEvent &e = s_ev[s_ev_head];
  e.id = s_ev_next_id++;
  e.ts = (now > 1577836800) ? (uint32_t)now : 0;
  strlcpy(e.type, type, sizeof(e.type));
  e.ok = ok;
  strlcpy(e.detail, detail ? detail : "", sizeof(e.detail));
  s_ev_head = (s_ev_head + 1) % EV_COUNT;
  portEXIT_CRITICAL(&s_ev_mux);
  log_i("event: %s ok=%d %s", type, ok, detail ? detail : "");
}

// Escape a string into JSON (quotes, backslash, control chars).
static int json_escape(const char *src, char *dst, int dst_len) {
  int w = 0;
  for (const char *r = src; *r && w < dst_len - 7; r++) {
    unsigned char c = (unsigned char)*r;
    if (c == '"' || c == '\\') {
      dst[w++] = '\\';
      dst[w++] = c;
    } else if (c == '\n') {
      dst[w++] = '\\';
      dst[w++] = 'n';
    } else if (c < 0x20) {
      w += snprintf(dst + w, dst_len - w, "\\u%04x", c);
    } else {
      dst[w++] = c;
    }
  }
  dst[w] = 0;
  return w;
}

int events_json(char *buf, size_t len) {
  AuraEvent snap[EV_COUNT];
  int head;
  portENTER_CRITICAL(&s_ev_mux);
  memcpy(snap, s_ev, sizeof(snap));
  head = s_ev_head;
  portEXIT_CRITICAL(&s_ev_mux);

  int w = snprintf(buf, len, "[");
  bool first = true;
  for (int i = 0; i < EV_COUNT; i++) {  // newest first
    AuraEvent &e = snap[(head - 1 - i + 2 * EV_COUNT) % EV_COUNT];
    if (!e.id) {
      continue;
    }
    char esc[EV_DETAIL * 2];
    json_escape(e.detail, esc, sizeof(esc));
    w += snprintf(buf + w, len - w, "%s{\"id\":%u,\"ts\":%u,\"type\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}",
                  first ? "" : ",", e.id, e.ts, e.type, e.ok ? "true" : "false", esc);
    first = false;
    if ((size_t)w >= len - 8) {
      break;
    }
  }
  w += snprintf(buf + w, len - w, "]\n");
  return w;
}

// ── Feedback tones ───────────────────────────────────────────────────────────
// speaker_tone() fails if the speaker is claimed; retry briefly so a chirp
// still lands right after another one finished.
static bool tone_retry(int freq, int ms, uint32_t patience_ms = 400) {
  uint32_t start = millis();
  do {
    if (speaker_tone(freq, ms, g_cfg.assist_vol)) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
  } while (millis() - start < patience_ms);
  return false;
}

void tone_ack_tap()  { tone_retry(880, 60); }
void tone_ack_hold() { tone_retry(1320, 60); vTaskDelay(pdMS_TO_TICKS(30)); tone_retry(1320, 60); }
void tone_success()  { tone_retry(988, 80);  vTaskDelay(pdMS_TO_TICKS(20)); tone_retry(1319, 80); }
void tone_photo()    { tone_retry(1568, 120); }
void tone_error()    { tone_retry(220, 400); }
void tone_no_wifi()  { tone_retry(400, 120); tone_retry(300, 120); tone_retry(200, 120); }

// ── Worker ───────────────────────────────────────────────────────────────────
static QueueHandle_t s_q = NULL;
static volatile bool s_busy = false;

struct ActionMsg {
  uint8_t act;
  uint8_t src;
};

bool action_enqueue(uint8_t act, uint8_t src) {
  if (!s_q || act == ACT_NONE || act >= ACT__COUNT) {
    return act == ACT_NONE;  // NONE is a no-op success
  }
  ActionMsg m = {act, src};
  return xQueueSend(s_q, &m, 0) == pdTRUE;
}

bool action_busy() {
  return s_busy;
}

static void do_capture() {
  char path[48] = "";
  if (capture_photo_to_sd(path, sizeof(path))) {
    led_flash_photo();
    tone_photo();
    event_push("capture", true, path);
  } else {
    tone_error();
    event_push("capture", false, sd_ok() ? "camera capture failed" : "no SD card");
  }
}

static void do_toggle_clip() {
  if (audio_clip_active()) {
    char path[64] = "";
    bool ok = audio_clip_stop(path, sizeof(path));
    tone_retry(660, 80);  // single low-ish beep = stopped
    event_push("clip_stop", ok, ok ? (path[0] ? path : "(RAM clip)") : "stop failed");
  } else {
    bool ok = audio_clip_start();
    if (ok) {
      tone_retry(660, 80);
      vTaskDelay(pdMS_TO_TICKS(30));
      tone_retry(880, 80);  // rising pair = recording
      event_push("clip_start", true, "recording");
    } else {
      tone_error();
      event_push("clip_start", false, "already recording");
    }
  }
}

static void do_save_audio_window() {
  if (!audio_window_ready()) {
    tone_error();
    event_push("audio_win", false, "window disabled (win_sec=0 or no PSRAM)");
    return;
  }
  char path[64] = "";
  if (audio_window_save(path, sizeof(path))) {
    tone_success();
    char detail[96];
    snprintf(detail, sizeof(detail), "%s (%.0fs)", path, (double)audio_window_sec());
    event_push("audio_win", true, detail);
  } else {
    tone_error();
    event_push("audio_win", false, sd_ok() ? "save failed" : "no SD card");
  }
}

// Placeholders — replaced by the Gemini (Phase 4) and video/window
// (Phases 2-3) implementations.
static void do_stub(const char *name) {
  tone_error();
  event_push(name, false, "feature not built into this firmware yet");
}

static void worker_task(void *arg) {
  ActionMsg m;
  for (;;) {
    if (xQueueReceive(s_q, &m, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    s_busy = true;
    switch (m.act) {
      case ACT_CAPTURE:        do_capture(); break;
      case ACT_TOGGLE_CLIP:    do_toggle_clip(); break;
      case ACT_DESCRIBE:       do_stub("describe"); break;
      case ACT_READ_TEXT:      do_stub("read_text"); break;
      case ACT_SPEAK_STATUS:   do_stub("status"); break;
      case ACT_SAVE_VIDEO:     do_stub("video_save"); break;
      case ACT_SAVE_AUDIO_WIN: do_save_audio_window(); break;
      default: break;
    }
    s_busy = false;
  }
}

void actions_init() {
  s_q = xQueueCreate(4, sizeof(ActionMsg));
  xTaskCreatePinnedToCore(worker_task, "aura_work", 16384, NULL, tskIDLE_PRIORITY + 1, NULL, 0);
}

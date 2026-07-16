// Device-side Gemini calls for the AURA glasses.
//
// Vision: SVGA JPEG → base64 → generateContent (streamed request body,
// small JSON response parsed with cJSON).
// TTS: generateContent with responseModalities:["AUDIO"] — the ~1 MB JSON
// response is NOT parsed; an incremental scanner finds the inlineData
// "data" field and streams base64 → PCM into PSRAM, then plays it.
//
// TLS uses the ESP x509 certificate bundle (no setInsecure): the API key
// travels in a header, so the connection must be verified.

#include "gemini.h"
#include "config.h"
#include "camctl.h"
#include "speaker.h"

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <mbedtls/base64.h>
#include <cJSON.h>

#define GEM_HOST "https://generativelanguage.googleapis.com"
#define GEM_TIMEOUT_MS 30000
#define GEM_MIN_HEAP (80 * 1024)      // internal heap needed for a TLS session
#define TTS_PCM_CAP (1024 * 1024)     // ~21 s of 24 kHz speech
#define VISION_RESP_CAP (32 * 1024)

static Preferences s_prefs;
static char s_key[80] = "";

void gemini_init() {
  s_prefs.begin("aura-sec", false);
  s_prefs.getString("gkey", s_key, sizeof(s_key));
}

bool gemini_key_set() {
  return s_key[0] != 0;
}

void gemini_set_key(const char *key) {
  strlcpy(s_key, key ? key : "", sizeof(s_key));
  s_prefs.putString("gkey", s_key);
}

// ── Small helpers ────────────────────────────────────────────────────────────
static void *psram_alloc(size_t n) {
  return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

static bool preflight(char *err, size_t elen) {
  if (WiFi.status() != WL_CONNECTED) {
    strlcpy(err, "no wifi", elen);
    return false;
  }
  if (!gemini_key_set()) {
    strlcpy(err, "no API key on device — send it from the app settings", elen);
    return false;
  }
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < GEM_MIN_HEAP) {
    strlcpy(err, "low memory for TLS", elen);
    return false;
  }
  return true;
}

// JSON-escape src into dst (for embedding text in a request body).
static void json_escape_to(const char *src, char *dst, size_t dlen) {
  size_t w = 0;
  for (const char *r = src; *r && w < dlen - 7; r++) {
    unsigned char c = (unsigned char)*r;
    if (c == '"' || c == '\\') {
      dst[w++] = '\\';
      dst[w++] = c;
    } else if (c == '\n' || c == '\r') {
      dst[w++] = ' ';
    } else if (c < 0x20) {
      w += snprintf(dst + w, dlen - w, "\\u%04x", c);
    } else {
      dst[w++] = c;
    }
  }
  dst[w] = 0;
}

// Language names come from the app via /config — keep only safe chars.
static void safe_lang(char *out, size_t olen) {
  size_t w = 0;
  for (const char *r = g_cfg.lang; *r && w < olen - 1; r++) {
    if (isalnum((unsigned char)*r) || *r == ' ' || *r == '-') {
      out[w++] = *r;
    }
  }
  out[w] = 0;
  if (!w) {
    strlcpy(out, "English", olen);
  }
}

static esp_http_client_handle_t make_client(const char *model) {
  char url[160];
  snprintf(url, sizeof(url), GEM_HOST "/v1beta/models/%s:generateContent", model);
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = GEM_TIMEOUT_MS;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 1024;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c) {
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "x-goog-api-key", s_key);
  }
  return c;
}

// On non-200, pull a short error snippet out of the response body.
static void read_error_snippet(esp_http_client_handle_t c, int status, char *err, size_t elen) {
  char body[280] = "";
  int got = 0, n;
  while (got < (int)sizeof(body) - 1 && (n = esp_http_client_read(c, body + got, sizeof(body) - 1 - got)) > 0) {
    got += n;
  }
  body[got] = 0;
  // Try to isolate the "message" field; fall back to the raw snippet.
  const char *msg = strstr(body, "\"message\"");
  if (msg) {
    msg = strchr(msg + 9, '"');
    if (msg) {
      msg++;
      const char *end = strchr(msg, '"');
      snprintf(err, elen, "HTTP %d: %.*s", status, end ? (int)(end - msg) : 120, msg);
      return;
    }
  }
  snprintf(err, elen, "HTTP %d %.*s", status, 100, body);
}

// ── Vision (describe / read text) ────────────────────────────────────────────
#define ASSIST_FRAMESIZE FRAMESIZE_SVGA
#define ASSIST_QUALITY 12

static bool capture_jpeg(uint8_t **out, size_t *out_len, char *err, size_t elen) {
  camctl_photo_begin(ASSIST_FRAMESIZE, ASSIST_QUALITY);
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    camctl_photo_end();
    strlcpy(err, "camera capture failed", elen);
    return false;
  }
  uint8_t *copy = (uint8_t *)psram_alloc(fb->len);
  if (copy) {
    memcpy(copy, fb->buf, fb->len);
    *out = copy;
    *out_len = fb->len;
  }
  esp_camera_fb_return(fb);
  camctl_photo_end();  // release the camera before the network round-trip
  if (!copy) {
    strlcpy(err, "PSRAM alloc failed", elen);
    return false;
  }
  return true;
}

static bool vision_request(uint8_t mode, char *result, size_t rlen) {
  char err[160];
  if (!preflight(err, sizeof(err))) {
    strlcpy(result, err, rlen);
    return false;
  }

  uint8_t *jpg = NULL;
  size_t jpg_len = 0;
  if (!capture_jpeg(&jpg, &jpg_len, err, sizeof(err))) {
    strlcpy(result, err, rlen);
    return false;
  }

  size_t b64_cap = 4 * ((jpg_len + 2) / 3) + 8;
  uint8_t *b64 = (uint8_t *)psram_alloc(b64_cap);
  size_t b64_len = 0;
  if (!b64 || mbedtls_base64_encode(b64, b64_cap, &b64_len, jpg, jpg_len) != 0) {
    free(jpg);
    if (b64) free(b64);
    strlcpy(result, "base64 failed", rlen);
    return false;
  }
  free(jpg);

  char lang[16];
  safe_lang(lang, sizeof(lang));
  char prompt[400];
  if (mode == ASSIST_READ_TEXT) {
    snprintf(prompt, sizeof(prompt),
             "You are smart glasses worn by a blind user. Read aloud ALL text visible in this photo, "
             "verbatim, in natural reading order, in %s. Do not describe the scene. "
             "If there is no readable text, say only: I don't see any text.", lang);
  } else {
    snprintf(prompt, sizeof(prompt),
             "You are smart glasses worn by a blind user who just asked what is in front of them. "
             "In %s, describe the scene in 2-3 short, concrete sentences: the main objects/people, "
             "where they are (left, center, right, near, far), and anything relevant for safe navigation. "
             "No preamble, no markdown.", lang);
  }
  static const char *HEAD =
      "{\"contents\":[{\"parts\":[{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"";
  char tail[560];
  snprintf(tail, sizeof(tail),
           "\"}},{\"text\":\"%s\"}]}],\"generationConfig\":{\"maxOutputTokens\":512}}", prompt);
  size_t total = strlen(HEAD) + b64_len + strlen(tail);

  esp_http_client_handle_t c = make_client(g_cfg.gem_model);
  bool ok = false;
  strlcpy(result, "request failed", rlen);  // default for early breaks
  do {
    if (!c || esp_http_client_open(c, total) != ESP_OK) {
      strlcpy(result, "connection failed", rlen);
      break;
    }
    if (esp_http_client_write(c, HEAD, strlen(HEAD)) < 0) break;
    size_t sent = 0;
    bool werr = false;
    while (sent < b64_len) {
      int n = esp_http_client_write(c, (const char *)b64 + sent, min(b64_len - sent, (size_t)4096));
      if (n < 0) { werr = true; break; }
      sent += n;
    }
    if (werr || esp_http_client_write(c, tail, strlen(tail)) < 0) {
      strlcpy(result, "upload failed", rlen);
      break;
    }
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
      read_error_snippet(c, status, result, rlen);
      break;
    }
    char *resp = (char *)psram_alloc(VISION_RESP_CAP);
    if (!resp) {
      strlcpy(result, "PSRAM alloc failed", rlen);
      break;
    }
    int got = 0, n;
    while (got < VISION_RESP_CAP - 1 && (n = esp_http_client_read(c, resp + got, VISION_RESP_CAP - 1 - got)) > 0) {
      got += n;
    }
    resp[got] = 0;

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
      strlcpy(result, "bad JSON from Gemini", rlen);
      break;
    }
    result[0] = 0;
    cJSON *cands = cJSON_GetObjectItem(root, "candidates");
    cJSON *cand = cands ? cJSON_GetArrayItem(cands, 0) : NULL;
    cJSON *content = cand ? cJSON_GetObjectItem(cand, "content") : NULL;
    cJSON *parts = content ? cJSON_GetObjectItem(content, "parts") : NULL;
    int np = parts ? cJSON_GetArraySize(parts) : 0;
    size_t w = 0;
    for (int i = 0; i < np; i++) {
      cJSON *t = cJSON_GetObjectItem(cJSON_GetArrayItem(parts, i), "text");
      if (t && cJSON_IsString(t)) {
        w += snprintf(result + w, rlen - w, "%s", t->valuestring);
        if (w >= rlen - 1) break;
      }
    }
    cJSON_Delete(root);
    if (!result[0]) {
      strlcpy(result, "empty response (model may be unavailable)", rlen);
      break;
    }
    ok = true;
  } while (0);

  free(b64);
  if (c) {
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
  }
  return ok;
}

// ── TTS ──────────────────────────────────────────────────────────────────────
// Incremental token matcher for the streamed response scan.
struct Matcher {
  const char *tok;
  int pos = 0;
};
static bool feed(Matcher &m, char ch) {
  if (ch == m.tok[m.pos]) {
    if (!m.tok[++m.pos]) {
      m.pos = 0;
      return true;
    }
  } else {
    m.pos = (ch == m.tok[0]) ? 1 : 0;
  }
  return false;
}

bool gemini_tts_speak(const char *text) {
  char err[160];
  if (!preflight(err, sizeof(err))) {
    log_e("tts: %s", err);
    return false;
  }
  if (!g_cfg.tts_model[0]) {
    log_e("tts: no TTS model configured (send one from the app)");
    return false;
  }

  // Build the request with the text JSON-escaped.
  size_t tlen = strlen(text);
  if (tlen > 1200) tlen = 1200;  // keep responses under the PCM cap
  char *esc = (char *)psram_alloc(tlen * 6 + 16);
  if (!esc) return false;
  char bounded[1201];
  strlcpy(bounded, text, min(sizeof(bounded), tlen + 1));
  json_escape_to(bounded, esc, tlen * 6 + 16);
  size_t body_cap = strlen(esc) + 256;
  char *body = (char *)psram_alloc(body_cap);
  if (!body) {
    free(esc);
    return false;
  }
  int body_len = snprintf(body, body_cap,
                          "{\"contents\":[{\"parts\":[{\"text\":\"Say in a natural, clear voice: %s\"}]}],"
                          "\"generationConfig\":{\"responseModalities\":[\"AUDIO\"]}}", esc);
  free(esc);

  uint8_t *pcm = (uint8_t *)psram_alloc(TTS_PCM_CAP);
  if (!pcm) {
    free(body);
    return false;
  }

  esp_http_client_handle_t c = make_client(g_cfg.tts_model);
  bool ok = false;
  size_t pcm_len = 0;
  uint32_t rate = 24000;
  do {
    if (!c || esp_http_client_open(c, body_len) != ESP_OK) break;
    if (esp_http_client_write(c, body, body_len) < 0) break;
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
      char e[160];
      read_error_snippet(c, status, e, sizeof(e));
      log_e("tts: %s", e);
      break;
    }

    // Scan the stream: inlineData → (rate=NNN) → "data" value → base64.
    Matcher m_inline = {"\"inlineData\""};
    Matcher m_rate = {"rate="};
    Matcher m_data = {"\"data\""};
    int phase = 0;         // 0: find inlineData, 1: find data, 2: skip to '"', 3: base64
    int rate_digits = -1;  // >=0 while collecting rate digits
    uint32_t rate_val = 0;
    char q[4];
    int qn = 0;
    static const int8_t B64[256] = {  /* -1 invalid, -2 padding */
      #define X -1
      X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,X, X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,
      X,X,X,X,X,X,X,X,X,X,X,62,X,X,X,63, 52,53,54,55,56,57,58,59,60,61,X,X,X,-2,X,X,
      X,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14, 15,16,17,18,19,20,21,22,23,24,25,X,X,X,X,X,
      X,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, 41,42,43,44,45,46,47,48,49,50,51,X,X,X,X,X,
      X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,X, X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,
      X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,X, X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,
      X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,X, X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,
      X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,X, X,X,X,X,X,X,X,X,X,X,X,X,X,X,X,X
      #undef X
    };
    char *chunk = (char *)psram_alloc(4096);
    if (!chunk) break;
    bool done = false;
    int n;
    while (!done && (n = esp_http_client_read(c, chunk, 4096)) > 0) {
      for (int i = 0; i < n && !done; i++) {
        char ch = chunk[i];
        if (rate_digits >= 0) {  // collecting digits after "rate="
          if (ch >= '0' && ch <= '9' && rate_digits < 6) {
            rate_val = rate_val * 10 + (ch - '0');
            rate_digits++;
            continue;
          }
          if (rate_val >= 8000 && rate_val <= 48000) rate = rate_val;
          rate_digits = -1;
        }
        if (phase == 0) {
          if (feed(m_inline, ch)) phase = 1;
        } else if (phase == 1) {
          if (feed(m_rate, ch)) { rate_digits = 0; rate_val = 0; }
          if (feed(m_data, ch)) phase = 2;
        } else if (phase == 2) {
          if (ch == '"') phase = 3;
        } else {  // base64 payload
          if (ch == '"') {
            done = true;
          } else {
            int8_t v = B64[(uint8_t)ch];
            if (v == -1) continue;  // whitespace etc.
            q[qn++] = ch;
            if (qn == 4) {
              uint8_t out[3];
              size_t olen = 0;
              mbedtls_base64_decode(out, 3, &olen, (const uint8_t *)q, 4);
              qn = 0;
              size_t cpy = min(olen, (size_t)(TTS_PCM_CAP - pcm_len));
              memcpy(pcm + pcm_len, out, cpy);
              pcm_len += cpy;
              if (pcm_len >= TTS_PCM_CAP) done = true;
            }
          }
        }
      }
      vTaskDelay(1);  // feed the watchdog on long downloads
    }
    free(chunk);
    ok = pcm_len > 1000;  // anything shorter than ~20 ms isn't speech
    if (!ok) log_e("tts: no audio in response (pcm=%u)", (unsigned)pcm_len);
  } while (0);

  free(body);
  if (c) {
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
  }

  if (ok) {
    ok = speaker_begin_stream(rate, g_cfg.assist_vol);
    if (ok) {
      for (size_t off = 0; off < pcm_len; off += 4096) {
        speaker_write(pcm + off, min((size_t)4096, pcm_len - off));
      }
      speaker_end_stream();
      log_i("tts: spoke %u B PCM at %u Hz", (unsigned)pcm_len, (unsigned)rate);
    }
  }
  free(pcm);
  return ok;
}

// ── Full assist pipeline ─────────────────────────────────────────────────────
int gemini_assist(uint8_t mode, char *result, size_t rlen) {
  if (!vision_request(mode, result, rlen)) {
    return 0;
  }
  return gemini_tts_speak(result) ? 1 : 2;
}

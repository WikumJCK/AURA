// AURA smart glasses firmware — XIAO ESP32-S3 Sense
// Camera + PDM mic + microSD logger served over HTTP, web app embedded at /.
//
// - Continuous audio logging to SD in rotating WAV segments (/rec)
// - Periodic max-quality photos to SD (/img)
// - Manual clips, live MJPEG stream on request
// - Joins whichever known network is available (iPhone hotspot or home WiFi),
//   announces itself as http://aura.local, syncs time via NTP for filenames.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SD.h>
#include <esp_camera.h>
#include <time.h>
#include "board_config.h"  // selects CAMERA_MODEL_XIAO_ESP32S3 and pulls in camera_pins.h
#include "config.h"
#include "sdcard.h"
#include "audio.h"
#include "speaker.h"
#include "actions.h"
#include "pads.h"
#include "led.h"

// Known networks, tried in order of availability/signal.
struct KnownAp {
  const char *ssid;
  const char *pass;
};
static const KnownAp KNOWN_APS[] = {
  {"WikumIphone", "wikum1234"},  // iPhone Personal Hotspot (phone-only use)
  {"JCK-WiFi", "12i405b3"},      // home network (development from PC)
};

static const char *MDNS_NAME = "aura";  // → http://aura.local

void startCameraServer();

static WiFiMulti wifiMulti;
static Preferences prefs;

// ── Config (persisted to NVS) ────────────────────────────────────────────────
AuraConfig g_cfg;

static void config_load() {
  prefs.begin("aura", false);
  g_cfg.cap_interval = prefs.getUShort("cap_int", 60);
  // Key is "fsize2": the old "fsize" stored a value from the pre-shift enum.
  g_cfg.framesize = prefs.getUChar("fsize2", FRAMESIZE_UXGA);
  g_cfg.quality = prefs.getUChar("jpegq", 10);
  g_cfg.seg_sec = prefs.getUShort("seg_sec", 60);
  g_cfg.rec_auto = prefs.getBool("rec_auto", true);
  g_cfg.gmt_off_min = prefs.getShort("gmt_off", 330);  // Asia/Colombo

  g_cfg.pad_act[SLOT_D5_TAP] = prefs.getUChar("pd5t", ACT_DESCRIBE);
  g_cfg.pad_act[SLOT_D5_HOLD] = prefs.getUChar("pd5h", ACT_READ_TEXT);
  g_cfg.pad_act[SLOT_D6_TAP] = prefs.getUChar("pd6t", ACT_CAPTURE);
  g_cfg.pad_act[SLOT_D6_HOLD] = prefs.getUChar("pd6h", ACT_SAVE_VIDEO);
  g_cfg.hold_ms = prefs.getUShort("hold_ms", 600);
  g_cfg.vid_en = prefs.getBool("vid_en", false);
  g_cfg.win_sec = prefs.getUShort("win_sec", 60);
  g_cfg.assist_vol = prefs.getUChar("avol", 85);
  prefs.getString("lang", g_cfg.lang, sizeof(g_cfg.lang));
  if (!g_cfg.lang[0]) strlcpy(g_cfg.lang, "English", sizeof(g_cfg.lang));
  prefs.getString("gmodel", g_cfg.gem_model, sizeof(g_cfg.gem_model));
  if (!g_cfg.gem_model[0]) strlcpy(g_cfg.gem_model, "gemini-3.5-flash", sizeof(g_cfg.gem_model));
  prefs.getString("tmodel", g_cfg.tts_model, sizeof(g_cfg.tts_model));

  // PSRAM guard: the video ring and a 3-min window don't fit together.
  if (g_cfg.vid_en && g_cfg.win_sec > 120) {
    g_cfg.win_sec = 120;
  }
}

void config_save() {
  prefs.putUShort("cap_int", g_cfg.cap_interval);
  prefs.putUChar("fsize2", g_cfg.framesize);
  prefs.putUChar("jpegq", g_cfg.quality);
  prefs.putUShort("seg_sec", g_cfg.seg_sec);
  prefs.putBool("rec_auto", g_cfg.rec_auto);
  prefs.putShort("gmt_off", g_cfg.gmt_off_min);

  prefs.putUChar("pd5t", g_cfg.pad_act[SLOT_D5_TAP]);
  prefs.putUChar("pd5h", g_cfg.pad_act[SLOT_D5_HOLD]);
  prefs.putUChar("pd6t", g_cfg.pad_act[SLOT_D6_TAP]);
  prefs.putUChar("pd6h", g_cfg.pad_act[SLOT_D6_HOLD]);
  prefs.putUShort("hold_ms", g_cfg.hold_ms);
  prefs.putBool("vid_en", g_cfg.vid_en);
  prefs.putUShort("win_sec", g_cfg.win_sec);
  prefs.putUChar("avol", g_cfg.assist_vol);
  prefs.putString("lang", g_cfg.lang);
  prefs.putString("gmodel", g_cfg.gem_model);
  prefs.putString("tmodel", g_cfg.tts_model);
}

void config_apply_camera() {
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, (framesize_t)g_cfg.framesize);
    s->set_quality(s, g_cfg.quality);
  }
}

// ── Camera ───────────────────────────────────────────────────────────────────
static bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = (framesize_t)g_cfg.framesize;
    config.jpeg_quality = g_cfg.quality;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;  // periodic shots must be current, not stale
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 0);
    s->set_saturation(s, 0);
  }
  return true;
}

// ── Photo capture to SD (periodic logging + ACT_CAPTURE pad action) ─────────
bool capture_photo_to_sd(char *out_path, size_t out_len) {
  if (!sd_ok()) {
    return false;
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[IMG] capture failed");
    return false;
  }
  char path[48];
  time_t now = time(NULL);
  if (now > 1577836800) {
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(path, sizeof(path), "/img/img_%04d%02d%02d_%02d%02d%02d.jpg", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  } else {
    snprintf(path, sizeof(path), "/img/img_boot_%lu.jpg", (unsigned long)millis());
  }
  sd_lock();
  File f = SD.open(path, FILE_WRITE);
  bool ok = false;
  if (f) {
    ok = f.write(fb->buf, fb->len) == fb->len;
    f.close();
  }
  sd_unlock();
  size_t len = fb->len;
  esp_camera_fb_return(fb);
  Serial.printf("[IMG] %s (%u B)%s\n", path, (unsigned)len, ok ? "" : " WRITE FAILED");
  if (ok && out_path && out_len) {
    strlcpy(out_path, path, out_len);
  }
  return ok;
}

// ── WiFi / time ──────────────────────────────────────────────────────────────
static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // keep the stream latency low
  for (auto &ap : KNOWN_APS) {
    wifiMulti.addAP(ap.ssid, ap.pass);
  }
  Serial.print("[WIFI] Connecting");
  while (wifiMulti.run(15000) != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.printf("\n[WIFI] Connected to %s, IP: %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("AURA Glasses");

  config_load();

  if (!initCamera()) {
    Serial.println("[CAM] init failed — halt");
    while (true) delay(1000);
  }

  if (sd_init()) {
    Serial.printf("[SD] ready, %u/%u MB free\n", sd_free_mb(), sd_total_mb());
  } else {
    Serial.println("[SD] not available — logging disabled, RAM clips only");
  }

  led_init();  // needs the SD mutex from sd_init()

  if (!audio_init()) {
    Serial.println("[MIC] init failed — audio endpoints will return errors");
  } else {
    Serial.println("[MIC] PDM mic ready");
  }

  // Must come after audio_init: the mic claims I2S0, the speaker gets I2S1.
  if (speaker_init()) {
    Serial.println("[SPK] MAX98357A ready (I2S1: DIN=1 BCLK=2 LRC=3)");
  } else {
    Serial.println("[SPK] init failed — /tone and /speak disabled");
  }

  actions_init();
  pads_init();
  Serial.printf("[PAD] actions d5=[%u,%u] d6=[%u,%u] hold=%ums\n",
                g_cfg.pad_act[0], g_cfg.pad_act[1], g_cfg.pad_act[2], g_cfg.pad_act[3], g_cfg.hold_ms);

  connectWiFi();

  configTime(g_cfg.gmt_off_min * 60L, 0, "pool.ntp.org", "time.google.com");

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[MDNS] http://%s.local\n", MDNS_NAME);
  } else {
    Serial.println("[MDNS] start failed");
  }

  startCameraServer();

  if (g_cfg.rec_auto && sd_ok()) {
    // Give NTP a moment so the first segment gets a real timestamp.
    for (int i = 0; i < 20 && time(NULL) < 1577836800; i++) {
      delay(250);
    }
    audio_continuous_start();
    Serial.println("[REC] continuous logging started");
  }

  Serial.println("\n===== URLs =====");
  Serial.printf("App     : http://%s/  (http://%s.local/)\n", WiFi.localIP().toString().c_str(), MDNS_NAME);
  Serial.printf("Stream  : http://%s:81/stream\n", WiFi.localIP().toString().c_str());
  Serial.printf("Health  : http://%s/health\n", WiFi.localIP().toString().c_str());
}

void loop() {
  // Reconnect if the hotspot/network drops.
  static uint32_t lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 5000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Lost connection — reconnecting");
      wifiMulti.run(15000);
    }
  }

  // Periodic photo logging to SD.
  static uint32_t lastShot = 0;
  if (g_cfg.cap_interval && sd_ok() && millis() - lastShot > g_cfg.cap_interval * 1000UL) {
    lastShot = millis();
    capture_photo_to_sd(NULL, 0);
  }

  delay(100);
}

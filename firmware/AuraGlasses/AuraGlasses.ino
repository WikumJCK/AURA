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

// Touch pads as digital inputs (INPUT_PULLUP: pressed pulls to GND)
// XIAO ESP32-S3 D5 = GPIO5, D6 = GPIO6
#define PAD_D5 5
#define PAD_D6 6

// Read by app_httpd.cpp for the /pad endpoint
extern "C" int aura_read_pad_d5_raw() { return digitalRead(PAD_D5); }
extern "C" int aura_read_pad_d6_raw() { return digitalRead(PAD_D6); }

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
}

void config_save() {
  prefs.putUShort("cap_int", g_cfg.cap_interval);
  prefs.putUChar("fsize2", g_cfg.framesize);
  prefs.putUChar("jpegq", g_cfg.quality);
  prefs.putUShort("seg_sec", g_cfg.seg_sec);
  prefs.putBool("rec_auto", g_cfg.rec_auto);
  prefs.putShort("gmt_off", g_cfg.gmt_off_min);
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

// ── Periodic photo logging ───────────────────────────────────────────────────
static void save_periodic_photo() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[IMG] capture failed");
    return;
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
  if (f) {
    f.write(fb->buf, fb->len);
    f.close();
  }
  sd_unlock();
  esp_camera_fb_return(fb);
  Serial.printf("[IMG] %s (%u B)\n", path, fb->len);
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

  pinMode(PAD_D5, INPUT_PULLUP);
  pinMode(PAD_D6, INPUT_PULLUP);

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
    save_periodic_photo();
  }

  delay(100);
}

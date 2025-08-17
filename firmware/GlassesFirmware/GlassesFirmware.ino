#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include "board_config.h"   // selects CAMERA_MODEL_XIAO_ESP32S3 and pulls in camera_pins.h

// ─────────────────────────────────────────────────────────────────────────────
// Wi‑Fi: set your network and the STATIC IP you want this device to use
// ─────────────────────────────────────────────────────────────────────────────
const char* WIFI_SSID     = "JCK-WiFi";     // ← change
const char* WIFI_PASSWORD = "12i405b3";    // ← change

IPAddress LOCAL_IP(192,168,8,80);            // ← change to the exact local IP you want
IPAddress GATEWAY (192,168,8,1);             // ← change to your router IP
IPAddress SUBNET  (255,255,255,0);
IPAddress DNS1    (8,8,8,8);                 // optional
IPAddress DNS2    (1,1,1,1);                 // optional

// Provided by app_httpd.cpp in this sketch folder
void startCameraServer();

static bool initCamera()
{
  // camera_pins.h supplies the correct pin numbers for CAMERA_MODEL_XIAO_ESP32S3
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;   // data lines
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk   = XCLK_GPIO_NUM;
  config.pin_pclk   = PCLK_GPIO_NUM;
  config.pin_vsync  = VSYNC_GPIO_NUM;
  config.pin_href   = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn  = PWDN_GPIO_NUM;   // some boards use -1
  config.pin_reset = RESET_GPIO_NUM;  // some boards use -1

  config.xclk_freq_hz = 20000000;     // 20 MHz
  config.pixel_format = PIXFORMAT_JPEG;

  // If PSRAM exists, use two frame buffers in PSRAM
  if (psramFound()) {
    config.frame_size   = FRAMESIZE_SVGA;     // 640x480 — change as you like
    config.jpeg_quality = 10;                // lower = better quality
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  } else {
    config.frame_size   = FRAMESIZE_SVGA;    // 320x240 for no-PSRAM
    config.jpeg_quality = 12;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] init failed: 0x%x\n", err);
    return false;
  }

  // Optional: tweak sensor defaults
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 0);
    s->set_saturation(s, 0);
  }
  return true;
}

static void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  // set static IP (comment this if you want DHCP)
  if (!WiFi.config(LOCAL_IP, GATEWAY, SUBNET, DNS1, DNS2)) {
    Serial.println("[WIFI] Failed to apply static IP config");
  }
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WIFI] Connecting to %s", WIFI_SSID);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - start > 20000) {
      Serial.println("\n[WIFI] Timeout — rebooting");
      ESP.restart();
    }
  }
  Serial.printf("\n[WIFI] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("AURA CamServer — static IP");

  if (!initCamera()) {
    Serial.println("Camera init failed — halt");
    while (true) delay(1000);
  }

  connectWiFi();

  startCameraServer();

  Serial.println("\n===== URLs =====");
  Serial.printf("UI      : http://%s/\n", WiFi.localIP().toString().c_str());
  Serial.printf("Stream  : http://%s/stream\n", WiFi.localIP().toString().c_str());
  Serial.printf("Capture : http://%s/capture\n", WiFi.localIP().toString().c_str());
}

void loop()
{
  delay(1000);
}

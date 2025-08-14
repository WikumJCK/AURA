#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <driver/i2s.h>
#include "Audio.h"

// Wi-Fi credentials
const char *ssid = "YOUR_WIFI_SSID";       // TODO: set your network SSID
const char *password = "YOUR_WIFI_PASSWORD"; // TODO: set your network password

// I2S pin configuration for MAX98357A
#define I2S_DOUT 1   // TODO: set to the pin used for data out
#define I2S_BCLK 2   // TODO: set to bit clock pin
#define I2S_LRCK 0   // TODO: set to left/right clock pin

Audio audio;

void connectWiFi() {
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
}

void setupCamera() {
  camera_config_t config; // TODO: populate with XIAO ESP32-S3 camera pins
  // initialize camera using esp_camera_init(&config);
}

void setupMicrophone() {
  // Configure I2S for PDM microphone input
  i2s_config_t cfg = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = 16000,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 64,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0
  };
  // TODO: configure microphone pins and install driver
}

void setupSpeaker() {
  audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT);
  audio.setVolume(100);
}

void setup() {
  Serial.begin(115200);
  connectWiFi();
  setupCamera();
  setupMicrophone();
  setupSpeaker();
}

void loop() {
  // TODO: capture audio, capture image, send to server, receive audio response
  // Example placeholder: play test tone once connected
  static bool played = false;
  if (!played && WiFi.status() == WL_CONNECTED) {
    audio.connecttospeech("Hello from AURA", "en"); // Uses Google translate TTS
    while (audio.isrunning()) {
      audio.loop();
    }
    played = true;
  }
  delay(100);
}

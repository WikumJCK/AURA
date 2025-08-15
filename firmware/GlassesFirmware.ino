#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <driver/i2s.h>

// Wi-Fi credentials
const char *ssid = "YOUR_WIFI_SSID";       // TODO: set your network SSID
const char *password = "YOUR_WIFI_PASSWORD"; // TODO: set your network password

// I2S pin configuration for MAX98357A
#define I2S_DOUT 1   // TODO: set to the pin used for data out
#define I2S_BCLK 2   // TODO: set to bit clock pin
#define I2S_LRCK 0   // TODO: set to left/right clock pin

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
  // Configure I2S for speaker output
  i2s_config_t cfg = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = 16000,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 64,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0
  };

  i2s_pin_config_t pins = {
      .bck_io_num = I2S_BCLK,
      .ws_io_num = I2S_LRCK,
      .data_out_num = I2S_DOUT,
      .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
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
    playTestTone();
    played = true;
  }
  delay(100);
}

void playTestTone() {
  const int sampleRate = 16000;
  const int frequency = 440; // A4
  const int durationMs = 500;
  const int samples = sampleRate * durationMs / 1000;
  for (int i = 0; i < samples; ++i) {
    float theta = 2.0f * PI * frequency * i / sampleRate;
    int16_t sample = (int16_t)(sin(theta) * 32767);
    size_t written;
    i2s_write(I2S_NUM_0, &sample, sizeof(sample), &written, portMAX_DELAY);
  }
}

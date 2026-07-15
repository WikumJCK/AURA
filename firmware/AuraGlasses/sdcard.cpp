#include "sdcard.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#define SD_CS 21  // Sense expansion board; also the user LED pin — LED is given up

static bool s_ok = false;
static SemaphoreHandle_t s_mtx = NULL;

bool sd_init() {
  s_mtx = xSemaphoreCreateMutex();
  // XIAO ESP32-S3 default SPI pins: SCK=7, MISO=8, MOSI=9
  if (!SD.begin(SD_CS)) {
    log_e("sd: begin failed (no card / wiring)");
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    log_e("sd: no card detected");
    return false;
  }
  SD.mkdir("/rec");
  SD.mkdir("/img");
  SD.mkdir("/logs");
  s_ok = true;
  log_i("sd: %llu MB total", SD.totalBytes() / (1024ULL * 1024ULL));
  return true;
}

bool sd_ok() {
  return s_ok;
}

void sd_lock() {
  xSemaphoreTake(s_mtx, portMAX_DELAY);
}

void sd_unlock() {
  xSemaphoreGive(s_mtx);
}

uint32_t sd_total_mb() {
  return s_ok ? (uint32_t)(SD.totalBytes() / (1024ULL * 1024ULL)) : 0;
}

uint32_t sd_free_mb() {
  return s_ok ? (uint32_t)((SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL)) : 0;
}

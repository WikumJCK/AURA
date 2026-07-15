#include "led.h"
#include "audio.h"
#include "sdcard.h"

#include <Arduino.h>

#define LED_PIN 21  // shared with SD CS; active LOW

static volatile bool s_assist = false;
static volatile uint8_t s_flash_req = 0;  // pending one-shot flashes

void led_set_assist(bool on) {
  s_assist = on;
}

void led_flash_photo() {
  s_flash_req = 2;
}

// One short pulse. Holding sd_lock guarantees no SD transaction sees the
// CS line move; with SCK idle the card ignores a bare CS assertion.
static void pulse(uint32_t on_ms) {
  sd_lock();
  digitalWrite(LED_PIN, LOW);   // LED on (CS asserted, bus idle)
  vTaskDelay(pdMS_TO_TICKS(on_ms));
  digitalWrite(LED_PIN, HIGH);  // LED off / CS deselected
  sd_unlock();
}

static void led_task(void *arg) {
  uint32_t last_blink = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (s_flash_req) {
      s_flash_req--;
      pulse(40);
      vTaskDelay(pdMS_TO_TICKS(80));
      continue;
    }
    uint32_t period = 0;
    if (s_assist) {
      period = 250;
    } else if (audio_clip_active() || audio_continuous_active()) {
      period = 1000;
    }
    if (period && millis() - last_blink >= period) {
      last_blink = millis();
      pulse(25);
    }
  }
}

void led_init() {
  // SD.begin already configured the pin when a card is present; make sure
  // it's an output (idle HIGH) even when there is no card.
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  xTaskCreatePinnedToCore(led_task, "aura_led", 2048, NULL, tskIDLE_PRIORITY + 1, NULL, 0);
}

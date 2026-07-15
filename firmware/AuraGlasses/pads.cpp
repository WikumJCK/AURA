#include "pads.h"
#include "actions.h"
#include "config.h"

#include <Arduino.h>

#define PAD_D5 5
#define PAD_D6 6
#define SCAN_MS 20
#define CONFIRM 2  // consecutive scans to accept a level change (~40 ms)

// Raw reads for the /pad endpoint (active HIGH).
extern "C" int aura_read_pad_d5_raw() { return digitalRead(PAD_D5); }
extern "C" int aura_read_pad_d6_raw() { return digitalRead(PAD_D6); }

struct PadFsm {
  uint8_t pin;
  uint8_t slot_tap, slot_hold;
  uint8_t cnt = 0;        // debounce confirm counter
  bool level = false;     // debounced level
  bool hold_fired = false;
  uint32_t t_down = 0;
};

static PadFsm s_pads[2] = {
  {PAD_D5, SLOT_D5_TAP, SLOT_D5_HOLD},
  {PAD_D6, SLOT_D6_TAP, SLOT_D6_HOLD},
};

static void fire(uint8_t slot, bool is_hold) {
  uint8_t act = g_cfg.pad_act[slot];
  if (act == ACT_NONE) {
    return;
  }
  if (action_enqueue(act, SRC_PAD)) {
    if (is_hold) {
      tone_ack_hold();
    } else {
      tone_ack_tap();
    }
  } else {
    tone_error();  // queue full — an action is already running
  }
}

static void pads_task(void *arg) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(SCAN_MS));
    for (auto &p : s_pads) {
      bool raw = digitalRead(p.pin) == HIGH;
      if (raw != p.level) {
        if (++p.cnt >= CONFIRM) {
          p.cnt = 0;
          p.level = raw;
          if (raw) {  // touch started
            p.t_down = millis();
            p.hold_fired = false;
          } else if (!p.hold_fired) {  // released before the hold threshold → tap
            fire(p.slot_tap, false);
          }
        }
      } else {
        p.cnt = 0;
      }
      // Hold fires at the threshold, while the finger is still down.
      if (p.level && !p.hold_fired && millis() - p.t_down >= g_cfg.hold_ms) {
        p.hold_fired = true;
        fire(p.slot_hold, true);
      }
    }
  }
}

void pads_init() {
  // TTP223 outputs push-pull active-high; pulldown keeps the pin defined
  // if a sensor is unplugged.
  pinMode(PAD_D5, INPUT_PULLDOWN);
  pinMode(PAD_D6, INPUT_PULLDOWN);
  xTaskCreatePinnedToCore(pads_task, "aura_pads", 3072, NULL, tskIDLE_PRIORITY + 2, NULL, 0);
}

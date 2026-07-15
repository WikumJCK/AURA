#ifndef AURA_LED_H
#define AURA_LED_H

#include <stdbool.h>

// Status blinking on the XIAO's onboard user LED. The LED shares GPIO21
// with the SD card's chip-select, so every pulse is done while holding the
// SD mutex (no SPI transaction can be in flight) and the pin is always left
// HIGH (CS deselected / LED off) afterwards.
//
// Patterns (derived automatically each tick):
//   recording (audio clip/continuous, later video) → slow blink (1 s)
//   assist request in flight (Gemini)              → rapid blink (250 ms)
//   photo saved                                    → one-shot double flash

void led_init();                  // call after sd_init() (needs the SD mutex)
void led_set_assist(bool on);     // worker sets around AI calls
void led_flash_photo();           // request a one-shot double flash

#endif  // AURA_LED_H

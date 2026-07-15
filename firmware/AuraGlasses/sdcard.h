#ifndef AURA_SDCARD_H
#define AURA_SDCARD_H

#include <stdint.h>
#include <stdbool.h>

// microSD on the XIAO ESP32-S3 Sense expansion board (SPI, CS=GPIO21).
// All SD access must happen between sd_lock()/sd_unlock() — the card is
// shared by the audio writer task, the periodic photo capture, and HTTP
// handlers.
bool sd_init();
bool sd_ok();
void sd_lock();
void sd_unlock();

uint32_t sd_total_mb();
uint32_t sd_free_mb();

#endif  // AURA_SDCARD_H

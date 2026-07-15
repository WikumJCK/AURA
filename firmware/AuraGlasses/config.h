#ifndef AURA_CONFIG_H
#define AURA_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

// Runtime configuration, persisted to NVS by the sketch (config_save()).
struct AuraConfig {
  uint16_t cap_interval;  // periodic photo interval in seconds, 0 = off
  uint8_t framesize;      // framesize_t value — ALWAYS use FRAMESIZE_* macros;
                          // the numeric values shift between core versions
  uint8_t quality;        // JPEG quality, lower = better (10 = max sane at UXGA)
  uint16_t seg_sec;       // continuous-recording segment length in seconds
  bool rec_auto;          // start continuous recording on boot
  int16_t gmt_off_min;    // local time offset for filenames/logs, minutes
};

extern AuraConfig g_cfg;

void config_save();          // persist g_cfg (implemented in the .ino)
void config_apply_camera();  // push framesize/quality to the sensor

#endif  // AURA_CONFIG_H

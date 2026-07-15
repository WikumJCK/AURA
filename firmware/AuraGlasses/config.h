#ifndef AURA_CONFIG_H
#define AURA_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

// Actions assignable to the touch pads (tap / hold slots). The numeric
// values are part of the /config API — append only, never renumber.
enum AuraAction : uint8_t {
  ACT_NONE = 0,
  ACT_DESCRIBE = 1,        // capture → Gemini vision → speak description
  ACT_READ_TEXT = 2,       // capture → Gemini OCR → speak text
  ACT_CAPTURE = 3,         // photo to SD /img + chime
  ACT_SAVE_VIDEO = 4,      // dump rolling video buffer to SD /video
  ACT_SAVE_AUDIO_WIN = 5,  // dump rolling audio window to SD /rec
  ACT_TOGGLE_CLIP = 6,     // start/stop manual audio clip
  ACT_SPEAK_STATUS = 7,    // speak time / WiFi / SD status
  ACT__COUNT
};

// Pad action slots (indexes into AuraConfig::pad_act)
enum AuraPadSlot : uint8_t {
  SLOT_D5_TAP = 0,
  SLOT_D5_HOLD = 1,
  SLOT_D6_TAP = 2,
  SLOT_D6_HOLD = 3,
};

// Runtime configuration, persisted to NVS by the sketch (config_save()).
struct AuraConfig {
  uint16_t cap_interval;  // periodic photo interval in seconds, 0 = off
  uint8_t framesize;      // framesize_t value — ALWAYS use FRAMESIZE_* macros;
                          // the numeric values shift between core versions
  uint8_t quality;        // JPEG quality, lower = better (10 = max sane at UXGA)
  uint16_t seg_sec;       // continuous-recording segment length in seconds
  bool rec_auto;          // start continuous recording on boot
  int16_t gmt_off_min;    // local time offset for filenames/logs, minutes

  uint8_t pad_act[4];     // AuraAction per slot (d5_tap, d5_hold, d6_tap, d6_hold)
  uint16_t hold_ms;       // tap-vs-hold threshold, ms
  bool vid_en;            // rolling video buffer enabled
  uint16_t win_sec;       // rolling audio window length, 0 = off (applied at boot)
  uint8_t assist_vol;     // speaker volume for chirps + spoken output, 0-100
  char lang[16];          // language for spoken descriptions ("English", ...)
  char gem_model[40];     // Gemini vision model for device-side assist
  char tts_model[48];     // Gemini TTS model (app pushes the discovered name)
};

extern AuraConfig g_cfg;

void config_save();          // persist g_cfg (implemented in the .ino)
void config_apply_camera();  // push framesize/quality to the sensor

#endif  // AURA_CONFIG_H

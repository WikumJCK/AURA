#ifndef AURA_GEMINI_H
#define AURA_GEMINI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Device-side Gemini pipeline: capture → vision (describe / OCR) → TTS →
// speaker. Runs entirely on the glasses so it works with the phone locked.
// All calls are made from the single action worker task (TLS needs ~50 KB
// of internal heap and a deep stack).

#define ASSIST_DESCRIBE 0
#define ASSIST_READ_TEXT 1

void gemini_init();  // load the API key from NVS (ns "aura-sec")

bool gemini_key_set();
// Persist the key (NULL/"" clears it). Never echoed anywhere.
void gemini_set_key(const char *key);

// Full pipeline: photo → Gemini vision → speak the answer.
// Returns 0 = failed (result holds the error), 1 = described and spoken,
// 2 = described but TTS/speaker failed (result holds the description).
int gemini_assist(uint8_t mode, char *result, size_t rlen);

// Speak arbitrary text through the speaker via Gemini TTS.
bool gemini_tts_speak(const char *text);

#endif  // AURA_GEMINI_H

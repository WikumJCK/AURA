#ifndef AURA_SPEAKER_H
#define AURA_SPEAKER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// MAX98357A I2S amplifier on I2S1 (the PDM mic owns I2S0).
// DIN = GPIO1 (D0), BCLK = GPIO2 (D1), LRC = GPIO3 (D2), Vin = 5V.
bool speaker_init();
bool speaker_busy();

// Blocking sine tone (hardware test / notification beeps). vol 0-100.
bool speaker_tone(int freq_hz, int dur_ms, int vol);

// Streaming PCM playback (16-bit mono little-endian). Claims the speaker;
// speaker_write() blocks on I2S DMA, speaker_end_stream() releases.
bool speaker_begin_stream(uint32_t sample_rate, int vol);
size_t speaker_write(const uint8_t *pcm, size_t len);
void speaker_end_stream();

#endif  // AURA_SPEAKER_H

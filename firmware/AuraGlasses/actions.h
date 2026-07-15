#ifndef AURA_ACTIONS_H
#define AURA_ACTIONS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Central action dispatch: every trigger source (pad gesture, /assist,
// /pad/sim, /video/save, /audio/window/save) enqueues here and one worker
// task executes serially — camera, speaker, Gemini and SD dumps never
// overlap each other.

enum ActionSource : uint8_t {
  SRC_PAD = 0,
  SRC_HTTP = 1,
};

void actions_init();  // call after speaker_init()/sd_init(), before pads_init()

// Queue an action (AuraAction from config.h). False = queue full.
bool action_enqueue(uint8_t act, uint8_t src);
bool action_busy();  // worker currently executing an action

// Event ring — results of executed actions, served by GET /events.
void event_push(const char *type, bool ok, const char *detail);
// Serializes the ring (newest first) as a JSON array into buf.
int events_json(char *buf, size_t len);

// Feedback tone vocabulary (blocking, skipped if the speaker is busy).
void tone_ack_tap();    // 1× 880 Hz — tap registered
void tone_ack_hold();   // 2× 1320 Hz — hold registered
void tone_success();    // rising pair — action completed
void tone_photo();      // single high chime — photo saved
void tone_error();      // low buzz — action failed
void tone_no_wifi();    // descending triple — offline

// Shared photo capture, implemented in the sketch (camera + SD glue).
// Writes /img/img_<ts>.jpg; copies the path into out_path on success.
bool capture_photo_to_sd(char *out_path, size_t out_len);

#endif  // AURA_ACTIONS_H

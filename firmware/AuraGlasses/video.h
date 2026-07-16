#ifndef AURA_VIDEO_H
#define AURA_VIDEO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Rolling video buffer + manual clip recording (MJPEG AVI on SD).
//
// A dedicated task grabs VGA JPEG frames at ~10 fps while the rolling
// buffer is enabled (g_cfg.vid_en) or a manual recording is running.
// Rolling mode keeps the last ~10 s of frames in a PSRAM ring;
// video_save_ring() dumps them to /video/vid_<ts>.avi. Manual mode
// streams frames straight to /video/clip_<ts>.avi (max 60 s).

#define VID_FPS 10
#define VID_REC_MAX_SEC 60

void video_init();  // call after camera + camctl + sd init

// Rolling buffer on/off (allocs/frees the 2.5 MB PSRAM ring). Called at
// boot from g_cfg.vid_en and on /config changes.
bool video_set_enabled(bool en);
bool video_enabled();

// Dump the rolling buffer to an AVI on SD (run from the worker task —
// takes a few seconds). Returns the file path.
bool video_save_ring(char *out_path, size_t out_len);
float video_ring_sec();   // seconds currently buffered
int video_ring_frames();

// Manual clip recording.
bool video_record_start();
bool video_record_stop(char *out_path, size_t out_len);
bool video_recording();
float video_record_sec();

int video_status_json(char *buf, size_t len);

#endif  // AURA_VIDEO_H

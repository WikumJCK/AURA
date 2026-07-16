#ifndef AURA_CAMCTL_H
#define AURA_CAMCTL_H

#include <stdbool.h>
#include <esp_camera.h>

// Camera coordinator: one mutex arbitrates sensor reconfiguration between
// photo captures (UXGA/configured size), the rolling-video task (VGA), and
// the MJPEG live stream. The rolling video task parks while a photo is
// being taken or a stream is open.

void camctl_init();

// Take the camera for a photo at the given size/quality. Blocks the video
// task, switches the sensor if needed (discarding stale frames), restores
// the previous sensor state on end.
bool camctl_photo_begin(framesize_t fs, int quality);
void camctl_photo_end();

// Live stream bracketing (suspends rolling video while open).
void camctl_stream_open();
void camctl_stream_close();
bool camctl_stream_active();

// Rolling-video task gate + park acknowledgement.
bool camctl_video_may_run();
void camctl_video_set_idle(bool idle);

// Sensor reconfiguration used by the video task itself (takes the mutex).
void camctl_set_sensor(framesize_t fs, int quality, int discard_frames);

#endif  // AURA_CAMCTL_H

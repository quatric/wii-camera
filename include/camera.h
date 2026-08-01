#ifndef WIICAM_CAMERA_H
#define WIICAM_CAMERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct camera camera_t;

typedef struct {
    const uint8_t *data;
    uint32_t width;
    uint32_t height;
    size_t stride;
    uint32_t sequence;
} camera_frame_t;

camera_t *camera_create(void);
bool camera_start(camera_t *camera, uint32_t width, uint32_t height,
                  uint32_t fps, char *error, size_t error_size);
bool camera_lock_latest(camera_t *camera, camera_frame_t *frame);
void camera_unlock(camera_t *camera);
void camera_stop(camera_t *camera);
void camera_destroy(camera_t *camera);

#endif


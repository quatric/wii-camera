#include "camera.h"

#include <malloc.h>
#include <ogc/mutex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb.h>
#include <libuvc/libuvc.h>

struct camera {
    uvc_context_t *context;
    uvc_device_t *device;
    uvc_device_handle_t *handle;
    uvc_stream_ctrl_t control;
    mutex_t mutex;
    uint8_t *pixels;
    size_t capacity;
    uint32_t width;
    uint32_t height;
    uint32_t sequence;
    bool mutex_ready;
    bool streaming;
    bool frame_ready;
};

static void set_error(char *buffer, size_t size, const char *message,
                      uvc_error_t code) {
    if (buffer != NULL && size > 0) {
        snprintf(buffer, size, "%s: %s (%d)", message, uvc_strerror(code), code);
    }
}

static void frame_callback(uvc_frame_t *frame, void *user) {
    camera_t *camera = user;
    uint32_t y;
    size_t row_bytes;
    const uint8_t *source;
    uint8_t *destination;

    if (frame == NULL || frame->data == NULL ||
        frame->frame_format != UVC_FRAME_FORMAT_YUYV ||
        frame->width == 0 || frame->height == 0) {
        return;
    }

    row_bytes = (size_t)frame->width * 2;
    if (row_bytes * frame->height > camera->capacity || frame->step < row_bytes) {
        return;
    }

    LWP_MutexLock(camera->mutex);
    source = frame->data;
    destination = camera->pixels;
    for (y = 0; y < frame->height; ++y) {
        memcpy(destination, source, row_bytes);
        source += frame->step;
        destination += row_bytes;
    }
    camera->width = frame->width;
    camera->height = frame->height;
    camera->sequence++;
    camera->frame_ready = true;
    LWP_MutexUnlock(camera->mutex);
}

camera_t *camera_create(void) {
    return calloc(1, sizeof(camera_t));
}

bool camera_start(camera_t *camera, uint32_t width, uint32_t height,
                  uint32_t fps, char *error, size_t error_size) {
    uvc_error_t result;

    if (camera == NULL || width == 0 || height == 0 || fps == 0 ||
        width > SIZE_MAX / height / 2) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "Invalid camera configuration");
        }
        return false;
    }

    camera->capacity = (size_t)width * height * 2;
    camera->pixels = memalign(32, camera->capacity);
    if (camera->pixels == NULL) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "Not enough memory for camera frame");
        }
        return false;
    }

    if (LWP_MutexInit(&camera->mutex, false) != 0) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "Could not create camera mutex");
        }
        goto fail;
    }
    camera->mutex_ready = true;

    result = uvc_init(&camera->context, NULL);
    if (result < 0) {
        set_error(error, error_size, "libuvc initialization failed", result);
        goto fail;
    }

    result = uvc_find_device(camera->context, &camera->device, 0, 0, NULL);
    if (result < 0) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "No UVC camera: %s (%d)\n%s",
                     uvc_strerror(result), result, libusb_wii_last_error());
        }
        goto fail;
    }

    result = uvc_open(camera->device, &camera->handle);
    if (result < 0) {
        set_error(error, error_size, "Could not open UVC camera", result);
        goto fail;
    }

    result = uvc_get_stream_ctrl_format_size(camera->handle, &camera->control,
                                              UVC_FRAME_FORMAT_YUYV,
                                              (int)width, (int)height, (int)fps);
    if (result < 0) {
        set_error(error, error_size, "Camera lacks requested YUYV mode", result);
        goto fail;
    }

    result = uvc_start_streaming(camera->handle, &camera->control,
                                 frame_callback, camera, 0);
    if (result < 0) {
        set_error(error, error_size, "Could not start camera stream", result);
        goto fail;
    }
    camera->streaming = true;
    return true;

fail:
    camera_stop(camera);
    return false;
}

bool camera_lock_latest(camera_t *camera, camera_frame_t *frame) {
    if (camera == NULL || frame == NULL || !camera->mutex_ready) {
        return false;
    }
    LWP_MutexLock(camera->mutex);
    if (!camera->frame_ready) {
        LWP_MutexUnlock(camera->mutex);
        return false;
    }
    frame->data = camera->pixels;
    frame->width = camera->width;
    frame->height = camera->height;
    frame->stride = (size_t)camera->width * 2;
    frame->sequence = camera->sequence;
    return true;
}

void camera_unlock(camera_t *camera) {
    if (camera != NULL && camera->mutex_ready) {
        LWP_MutexUnlock(camera->mutex);
    }
}

void camera_stop(camera_t *camera) {
    if (camera == NULL) {
        return;
    }
    if (camera->streaming) {
        uvc_stop_streaming(camera->handle);
        camera->streaming = false;
    }
    if (camera->handle != NULL) {
        uvc_close(camera->handle);
        camera->handle = NULL;
    }
    if (camera->device != NULL) {
        uvc_unref_device(camera->device);
        camera->device = NULL;
    }
    if (camera->context != NULL) {
        uvc_exit(camera->context);
        camera->context = NULL;
    }
    if (camera->mutex_ready) {
        LWP_MutexDestroy(camera->mutex);
        camera->mutex_ready = false;
    }
    free(camera->pixels);
    camera->pixels = NULL;
    camera->capacity = 0;
    camera->frame_ready = false;
}

void camera_destroy(camera_t *camera) {
    if (camera != NULL) {
        camera_stop(camera);
        free(camera);
    }
}

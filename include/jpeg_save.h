#ifndef WIICAM_JPEG_SAVE_H
#define WIICAM_JPEG_SAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool jpeg_save_baseline_yuyv(const char *path, const uint8_t *pixels,
                             uint32_t width, uint32_t height, size_t stride,
                             int quality, char *error, size_t error_size);

#endif


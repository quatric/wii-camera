#include "jpeg_save.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
} wiicam_jpeg_error_t;

static void jpeg_error_exit(j_common_ptr info) {
    wiicam_jpeg_error_t *error = (wiicam_jpeg_error_t *)info->err;
    (*info->err->format_message)(info, error->message);
    longjmp(error->jump, 1);
}

static uint8_t clamp_color(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static void yuyv_to_rgb_row(const uint8_t *source, uint8_t *destination,
                            uint32_t width) {
    uint32_t x;
    for (x = 0; x < width; x += 2) {
        int y0 = source[0] - 16;
        int u = source[1] - 128;
        int y1 = source[2] - 16;
        int v = source[3] - 128;
        int c0 = y0 < 0 ? 0 : 298 * y0;
        int c1 = y1 < 0 ? 0 : 298 * y1;

        destination[0] = clamp_color((c0 + 409 * v + 128) >> 8);
        destination[1] = clamp_color((c0 - 100 * u - 208 * v + 128) >> 8);
        destination[2] = clamp_color((c0 + 516 * u + 128) >> 8);
        destination[3] = clamp_color((c1 + 409 * v + 128) >> 8);
        destination[4] = clamp_color((c1 - 100 * u - 208 * v + 128) >> 8);
        destination[5] = clamp_color((c1 + 516 * u + 128) >> 8);
        source += 4;
        destination += 6;
    }
}

static bool file_has_baseline_sof(const char *path) {
    FILE *file = fopen(path, "rb");
    int first;
    int second;
    bool baseline = false;

    if (file == NULL) return false;
    first = fgetc(file);
    second = fgetc(file);
    if (first != 0xff || second != 0xd8) goto done;

    while ((first = fgetc(file)) != EOF) {
        unsigned int length;
        if (first != 0xff) continue;
        do {
            second = fgetc(file);
        } while (second == 0xff);
        if (second == EOF || second == 0xda || second == 0xd9) break;
        if (second == 0xc0) {
            baseline = true;
            break;
        }
        /* Reject every other Start Of Frame encoding. */
        if (second >= 0xc1 && second <= 0xcf &&
            second != 0xc4 && second != 0xc8 && second != 0xcc) {
            break;
        }
        if (second >= 0xd0 && second <= 0xd7) continue;
        first = fgetc(file);
        second = fgetc(file);
        if (first == EOF || second == EOF) break;
        length = ((unsigned int)first << 8) | (unsigned int)second;
        if (length < 2 || fseek(file, (long)length - 2, SEEK_CUR) != 0) break;
    }

done:
    fclose(file);
    return baseline;
}

bool jpeg_save_baseline_yuyv(const char *path, const uint8_t *pixels,
                             uint32_t width, uint32_t height, size_t stride,
                             int quality, char *error_text, size_t error_size) {
    struct jpeg_compress_struct info;
    wiicam_jpeg_error_t jpeg_error;
    FILE *file = NULL;
    uint8_t *row = NULL;
    bool compressor_created = false;

    if (path == NULL || pixels == NULL || width == 0 || height == 0 ||
        width > 65535 || height > 65535 || (width & 1) != 0 ||
        stride < (size_t)width * 2) {
        if (error_text != NULL && error_size > 0)
            snprintf(error_text, error_size, "Invalid image data");
        return false;
    }

    memset(&info, 0, sizeof(info));
    info.err = jpeg_std_error(&jpeg_error.base);
    jpeg_error.base.error_exit = jpeg_error_exit;
    jpeg_error.message[0] = '\0';

    if (setjmp(jpeg_error.jump)) {
        if (compressor_created) jpeg_destroy_compress(&info);
        free(row);
        if (file != NULL) fclose(file);
        remove(path);
        if (error_text != NULL && error_size > 0)
            snprintf(error_text, error_size, "JPEG error: %s", jpeg_error.message);
        return false;
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        if (error_text != NULL && error_size > 0)
            snprintf(error_text, error_size, "Could not create %s", path);
        return false;
    }
    row = malloc((size_t)width * 3);
    if (row == NULL) {
        fclose(file);
        remove(path);
        if (error_text != NULL && error_size > 0)
            snprintf(error_text, error_size, "Not enough memory for JPEG row");
        return false;
    }

    jpeg_create_compress(&info);
    compressor_created = true;
    jpeg_stdio_dest(&info, file);
    info.image_width = width;
    info.image_height = height;
    info.input_components = 3;
    info.in_color_space = JCS_RGB;
    jpeg_set_defaults(&info);
    /* TRUE clamps quantization tables to baseline-compatible 8-bit values. */
    jpeg_set_quality(&info, quality, TRUE);
    info.optimize_coding = TRUE;
    info.arith_code = FALSE;
    jpeg_start_compress(&info, TRUE);

    while (info.next_scanline < info.image_height) {
        JSAMPROW scanline = row;
        yuyv_to_rgb_row(pixels + (size_t)info.next_scanline * stride, row, width);
        jpeg_write_scanlines(&info, &scanline, 1);
    }
    jpeg_finish_compress(&info);
    jpeg_destroy_compress(&info);
    compressor_created = false;
    free(row);
    row = NULL;
    if (fclose(file) != 0) {
        file = NULL;
        remove(path);
        if (error_text != NULL && error_size > 0)
            snprintf(error_text, error_size, "Write failed for %s", path);
        return false;
    }
    file = NULL;

    if (!file_has_baseline_sof(path)) {
        remove(path);
        if (error_text != NULL && error_size > 0)
            snprintf(error_text, error_size, "Encoder did not produce baseline JPEG");
        return false;
    }
    return true;
}

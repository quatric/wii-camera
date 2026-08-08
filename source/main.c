#include "camera.h"
#include "jpeg_save.h"
#include "storage.h"

#include <fat.h>
#include <gccore.h>
#include <ogc/system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wiiuse/wpad.h>

#define CAPTURE_WIDTH  640
#define CAPTURE_HEIGHT 480
#define CAPTURE_FPS    15
#define JPEG_QUALITY   90

static volatile bool exit_requested;

static void request_exit(void) {
    exit_requested = true;
}

static void request_reset_exit(uint32_t irq, void *context) {
    (void)irq;
    (void)context;
    request_exit();
}

static void fill_black(volatile uint32_t *xfb, const GXRModeObj *mode) {
    size_t words = (size_t)mode->fbWidth * mode->xfbHeight / 2;
    size_t i;
    for (i = 0; i < words; ++i) xfb[i] = 0x10801080;
}

static void draw_yuyv(volatile uint32_t *xfb, const GXRModeObj *mode,
                      const camera_frame_t *frame, bool flash) {
    uint32_t draw_width = frame->width < mode->fbWidth ? frame->width : mode->fbWidth;
    uint32_t draw_height = frame->height < mode->xfbHeight ? frame->height : mode->xfbHeight;
    uint32_t x_offset = (mode->fbWidth - draw_width) / 2;
    uint32_t y_offset = (mode->xfbHeight - draw_height) / 2;
    uint32_t y;

    draw_width &= ~1u;
    fill_black(xfb, mode);
    for (y = 0; y < draw_height; ++y) {
        const uint8_t *source = frame->data + (size_t)y * frame->stride;
        volatile uint32_t *destination = xfb +
            ((size_t)(y + y_offset) * mode->fbWidth + x_offset) / 2;
        uint32_t x;
        for (x = 0; x < draw_width; x += 2) {
            uint8_t y0 = source[0];
            uint8_t u = source[1];
            uint8_t y1 = source[2];
            uint8_t v = source[3];
            if (flash) {
                y0 = (uint8_t)(y0 + ((255 - y0) >> 1));
                y1 = (uint8_t)(y1 + ((255 - y1) >> 1));
                u = (uint8_t)(u + ((128 - u) >> 1));
                v = (uint8_t)(v + ((128 - v) >> 1));
            }
            *destination++ = ((uint32_t)y0 << 24) | ((uint32_t)u << 16) |
                             ((uint32_t)y1 << 8) | v;
            source += 4;
        }
    }
}

static void wait_for_exit_button(void) {
    for (;;) {
        WPAD_ScanPads();
        PAD_ScanPads();
        if ((WPAD_ButtonsDown(0) & (WPAD_BUTTON_HOME | WPAD_BUTTON_B)) ||
            (PAD_ButtonsDown(0) & PAD_BUTTON_START) || exit_requested) break;
        VIDEO_WaitVSync();
    }
}

int main(void) {
    GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
    volatile uint32_t *framebuffers[2];
    unsigned int front = 0;
    camera_t *camera = NULL;
    char folder[64];
    char path[160];
    char error[160];
    unsigned int flash_frames = 0;
    bool storage_ready;

    VIDEO_Init();
    WPAD_Init();
    PAD_Init();
    SYS_SetPowerCallback(request_exit);
    SYS_SetResetCallback(request_reset_exit);

    framebuffers[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
    framebuffers[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
    if (framebuffers[0] == NULL || framebuffers[1] == NULL) return EXIT_FAILURE;
    fill_black(framebuffers[0], mode);
    fill_black(framebuffers[1], mode);
    VIDEO_Configure(mode);
    VIDEO_SetNextFramebuffer((void *)framebuffers[front]);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();

    console_init((void *)framebuffers[front], 20, 20, mode->fbWidth,
                 mode->xfbHeight, mode->fbWidth * VI_DISPLAY_PIX_SZ);
    printf("\x1b[2;0HWiiCam WIP 0.2.2\n\n");
    printf("A: save baseline JPEG   HOME/B: exit\n\n");

    storage_ready = storage_init(folder, sizeof(folder), error, sizeof(error));
    if (!storage_ready) {
        printf("Storage error: %s\n", error);
        wait_for_exit_button();
        goto cleanup;
    }
    printf("Photos: %s\n", folder);

    camera = camera_create();
    if (camera == NULL || !camera_start(camera, CAPTURE_WIDTH, CAPTURE_HEIGHT,
                                        CAPTURE_FPS, error, sizeof(error))) {
        printf("Camera error: %s\n", camera == NULL ? "out of memory" : error);
        printf("\nConnect a YUYV-capable UVC camera and relaunch.\n");
        wait_for_exit_button();
        goto cleanup;
    }

    while (!exit_requested) {
        camera_frame_t frame;
        uint32_t wpad_down;
        uint16_t pad_down;
        bool capture;

        WPAD_ScanPads();
        PAD_ScanPads();
        wpad_down = WPAD_ButtonsDown(0);
        pad_down = PAD_ButtonsDown(0);
        if ((wpad_down & (WPAD_BUTTON_HOME | WPAD_BUTTON_B)) ||
            (pad_down & PAD_BUTTON_START)) break;
        capture = (wpad_down & WPAD_BUTTON_A) || (pad_down & PAD_BUTTON_A);

        if (camera_lock_latest(camera, &frame)) {
            unsigned int back = front ^ 1u;
            if (capture) {
                if (storage_next_filename(folder, path, sizeof(path)) &&
                    jpeg_save_baseline_yuyv(path, frame.data, frame.width,
                                            frame.height, frame.stride,
                                            JPEG_QUALITY, error, sizeof(error))) {
                    flash_frames = 3;
                }
            }
            draw_yuyv(framebuffers[back], mode, &frame, flash_frames > 0);
            camera_unlock(camera);
            if (flash_frames > 0) --flash_frames;
            VIDEO_SetNextFramebuffer((void *)framebuffers[back]);
            VIDEO_Flush();
            VIDEO_WaitVSync();
            front = back;
        } else {
            VIDEO_WaitVSync();
        }
    }

cleanup:
    camera_destroy(camera);
    fatUnmount("sd:");
    fatUnmount("usb:");
    VIDEO_SetBlack(true);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
    return 0;
}

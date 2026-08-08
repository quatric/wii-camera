#include "camera.h"
#include "jpeg_save.h"
#include "storage.h"

#include <fat.h>
#include <gccore.h>
#include <ogc/system.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wiiuse/wpad.h>

#define CAPTURE_WIDTH  640
#define CAPTURE_HEIGHT 480
#define CAPTURE_FPS    15
#define JPEG_QUALITY   90

static volatile bool exit_requested;

static uint32_t ehci_read(uint32_t address) {
    return *(volatile uint32_t *)address;
}

static void ehci_write(uint32_t address, uint32_t value) {
    *(volatile uint32_t *)address = value;
    __asm__ volatile("eieio; sync" ::: "memory");
}

static bool ehci_wait_bits(uint32_t address, uint32_t mask, uint32_t value,
                           unsigned int milliseconds) {
    while (milliseconds-- > 0) {
        if ((ehci_read(address) & mask) == value) return true;
        usleep(1000);
    }
    return false;
}

typedef struct __attribute__((aligned(32))) {
    uint32_t next;
    uint32_t alternate;
    uint32_t token;
    uint32_t buffer[5];
} ehci_qtd_t;

typedef struct __attribute__((aligned(32))) {
    uint32_t horizontal;
    uint32_t endpoint;
    uint32_t capabilities;
    uint32_t current;
    ehci_qtd_t overlay;
} ehci_qh_t;

static uint32_t ehci_dma_word(uint32_t value) {
    return value;
}

static uint32_t ehci_physical(const void *pointer) {
    return (uint32_t)MEM_VIRTUAL_TO_PHYSICAL(pointer);
}

static void ehci_qtd_initialize(ehci_qtd_t *qtd, ehci_qtd_t *next,
                                unsigned int pid, unsigned int length,
                                bool data_toggle, void *buffer) {
    uint32_t token = 0x80u | (pid << 8) | (3u << 10) | (length << 16);
    memset(qtd, 0, sizeof(*qtd));
    qtd->next = ehci_dma_word(next == NULL ? 1u : ehci_physical(next));
    qtd->alternate = ehci_dma_word(1u);
    if (data_toggle) token |= 0x80000000u;
    qtd->token = ehci_dma_word(token);
    if (buffer != NULL)
        qtd->buffer[0] = ehci_dma_word(ehci_physical(buffer));
}

static bool ehci_get_device_descriptor(unsigned char descriptor[18],
                                       uint32_t *final_token,
                                       uint32_t *live_status,
                                       uint32_t *live_command) {
    ehci_qh_t *qh = memalign(32, 64);
    ehci_qtd_t *qtd = memalign(32, sizeof(ehci_qtd_t) * 3);
    unsigned char *setup = memalign(32, 32);
    unsigned char *data = memalign(32, 32);
    unsigned int elapsed;
    bool completed = false;

    if (qh == NULL || qtd == NULL || setup == NULL || data == NULL) {
        free(qh); free(qtd); free(setup); free(data);
        return false;
    }
    memset(qh, 0, 64);
    memset(setup, 0, 32);
    memset(data, 0, 32);
    setup[0] = 0x80;
    setup[1] = 0x06;
    setup[3] = 0x01;
    setup[6] = 18;

    ehci_qtd_initialize(&qtd[0], &qtd[1], 2, 8, false, setup);
    ehci_qtd_initialize(&qtd[1], &qtd[2], 1, 18, true, data);
    ehci_qtd_initialize(&qtd[2], NULL, 0, 0, true, NULL);
    qtd[2].token = ehci_dma_word(0x80000000u | 0x8000u | (3u << 10) | 0x80u);

    qh->horizontal = ehci_dma_word(ehci_physical(qh) | 2u);
    qh->endpoint = ehci_dma_word((2u << 12) | (1u << 14) |
                                 (1u << 15) | (64u << 16));
    qh->capabilities = ehci_dma_word(1u << 30);
    qh->overlay.next = ehci_dma_word(ehci_physical(&qtd[0]));
    qh->overlay.alternate = ehci_dma_word(1u);

    DCFlushRange(setup, 32);
    DCFlushRange(data, 32);
    DCFlushRange(qtd, sizeof(ehci_qtd_t) * 3);
    DCFlushRange(qh, 64);
    ehci_write(0xcd040014u, 0x3fu);
    ehci_write(0xcd040028u, ehci_physical(qh));
    ehci_write(0xcd040010u, 0x00080021u);

    for (elapsed = 0; elapsed < 500; ++elapsed) {
        DCInvalidateRange(&qtd[2], sizeof(qtd[2]));
        *final_token = qtd[2].token;
        if ((*final_token & 0x80u) == 0) {
            completed = (*final_token & 0x7cu) == 0;
            break;
        }
        usleep(1000);
    }
    *live_status = ehci_read(0xcd040014u);
    *live_command = ehci_read(0xcd040010u);
    ehci_write(0xcd040010u, 0);
    ehci_wait_bits(0xcd040014u, 0x1000u, 0x1000u, 100);
    DCInvalidateRange(data, 32);
    if (completed) memcpy(descriptor, data, 18);
    free(qh); free(qtd); free(setup); free(data);
    return completed;
}

static void print_ehci_probe(void) {
    uint32_t capability = ehci_read(0xcd040000u);
    uint32_t command = ehci_read(0xcd040010u);
    uint32_t status = ehci_read(0xcd040014u);
    uint32_t port = ehci_read(0xcd040054u);
    printf("EHCI raw cap:%08lx cmd:%08lx sts:%08lx port:%08lx\n",
           (unsigned long)capability, (unsigned long)command,
           (unsigned long)status, (unsigned long)port);
}

static void run_ehci_takeover_probe(void) {
    const uint32_t command_register = 0xcd040010u;
    const uint32_t status_register = 0xcd040014u;
    const uint32_t interrupt_register = 0xcd040018u;
    const uint32_t periodic_register = 0xcd040024u;
    const uint32_t async_register = 0xcd040028u;
    const uint32_t config_register = 0xcd040050u;
    const uint32_t port_register = 0xcd040054u;
    uint32_t command = ehci_read(command_register);
    uint32_t port;
    bool halted;
    bool reset_done;
    bool port_reset_done;
    unsigned char descriptor[18];
    uint32_t transfer_token = 0xffffffffu;
    uint32_t transfer_status = 0xffffffffu;
    uint32_t transfer_command = 0xffffffffu;
    bool descriptor_ok;

    ehci_write(interrupt_register, 0);
    ehci_write(command_register, command & ~1u);
    halted = ehci_wait_bits(status_register, 0x1000u, 0x1000u, 100);
    ehci_write(command_register, (command & ~1u) | 2u);
    reset_done = ehci_wait_bits(command_register, 2u, 0, 100);

    ehci_write(periodic_register, 0);
    ehci_write(async_register, 0);
    ehci_write(config_register, 1);
    port = ehci_read(port_register) & ~(0x2au | 0x100u);
    ehci_write(port_register, port | 0x1000u | 0x100u);
    usleep(50000);
    port = ehci_read(port_register) & ~(0x2au | 0x100u);
    ehci_write(port_register, port | 0x1000u);
    port_reset_done = ehci_wait_bits(port_register, 0x100u, 0, 100);
    usleep(20000);

    printf("EHCI take halt:%u reset:%u preset:%u port:%08lx\n",
           halted, reset_done, port_reset_done,
           (unsigned long)ehci_read(port_register));
    descriptor_ok = ehci_get_device_descriptor(descriptor, &transfer_token,
                                               &transfer_status,
                                               &transfer_command);
    if (descriptor_ok) {
        printf("EHCI dev %02x%02x VID:%02x%02x PID:%02x%02x mps:%u\n",
               descriptor[0], descriptor[1], descriptor[9], descriptor[8],
               descriptor[11], descriptor[10], descriptor[7]);
    } else {
        printf("EHCI GET_DESC fail tok:%08lx sts:%08lx cmd:%08lx\n",
               (unsigned long)transfer_token,
               (unsigned long)transfer_status,
               (unsigned long)transfer_command);
    }
}

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
    printf("\x1b[2;0HWiiCam WIP 0.2.8\n\n");
    print_ehci_probe();
    run_ehci_takeover_probe();
    printf("Direct EHCI takeover test complete. HOME/B exits.\n");
    wait_for_exit_button();
    goto cleanup;
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

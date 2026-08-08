#include "camera.h"
#include "jpeg_save.h"
#include "storage.h"

#include <fat.h>
#include <gccore.h>
#include <ogc/system.h>
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
    uint32_t buffer_high[5];
} ehci_qtd_t;

typedef struct __attribute__((aligned(32))) {
    uint32_t horizontal;
    uint32_t endpoint;
    uint32_t capabilities;
    uint32_t current;
    uint32_t overlay_next;
    uint32_t overlay_alternate;
    uint32_t overlay_token;
    uint32_t overlay_buffer[5];
    uint32_t overlay_buffer_high[5];
} ehci_qh_t;

_Static_assert(sizeof(ehci_qtd_t) == 64, "EHCI qTD must occupy 64 bytes");
_Static_assert(offsetof(ehci_qh_t, overlay_next) == 16,
               "EHCI QH overlay must begin at offset 0x10");
_Static_assert(sizeof(ehci_qh_t) == 96, "EHCI QH must occupy 96 bytes");

static uint32_t ehci_dma_word(uint32_t value) {
    return __builtin_bswap32(value);
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

static bool ehci_control_transfer(unsigned int device_address,
                                  const unsigned char request[8],
                                  unsigned int length, bool direction_in,
                                  unsigned char *result,
                                  uint32_t *final_token,
                                  uint32_t *live_status,
                                  uint32_t *live_command,
                                  uint32_t trace[9]) {
    /* Hollywood's EHCI can only DMA from its reserved MEM2 aperture. The
     * corresponding physical window is 0x133e0000-0x1345ffff. */
    ehci_qh_t *head = (ehci_qh_t *)0x933e0000u;
    ehci_qh_t *qh = (ehci_qh_t *)0x933e0060u;
    ehci_qtd_t *qtd = (ehci_qtd_t *)0x933e00c0u;
    unsigned char *setup = (unsigned char *)0x933e0180u;
    unsigned char *data = (unsigned char *)0x933e01a0u;
    unsigned int status_index = length > 0 ? 2u : 1u;
    unsigned int elapsed;
    bool completed = false;

    memset(head, 0, sizeof(*head));
    memset(qh, 0, sizeof(*qh));
    memset(setup, 0, 32);
    memset(data, 0, 32);
    memcpy(setup, request, 8);

    ehci_qtd_initialize(&qtd[0], &qtd[1], 2, 8, false, setup);
    if (length > 0) {
        ehci_qtd_initialize(&qtd[1], &qtd[2], direction_in ? 1u : 0u,
                            length, true, data);
    }
    ehci_qtd_initialize(&qtd[status_index], NULL,
                        direction_in ? 0u : 1u, 0, true, NULL);
    qtd[status_index].token = ehci_dma_word(0x80000000u | 0x8000u |
                                           (3u << 10) | 0x80u |
                                           ((direction_in ? 0u : 1u) << 8));

    head->horizontal = ehci_dma_word(ehci_physical(qh) | 2u);
    head->endpoint = ehci_dma_word(1u << 15);
    head->overlay_next = ehci_dma_word(1u);
    head->overlay_alternate = ehci_dma_word(1u);
    head->overlay_token = ehci_dma_word(0x40u);

    qh->horizontal = ehci_dma_word(ehci_physical(head) | 2u);
    qh->endpoint = ehci_dma_word((4u << 28) | (2u << 12) | (1u << 14) |
                                 (64u << 16) | (device_address & 0x7fu));
    qh->capabilities = ehci_dma_word(1u << 30);
    qh->overlay_next = ehci_dma_word(ehci_physical(&qtd[0]));
    qh->overlay_alternate = ehci_dma_word(1u);

    DCFlushRange(setup, 32);
    DCFlushRange(data, 32);
    DCFlushRange(qtd, sizeof(ehci_qtd_t) * 3);
    DCFlushRange(head, sizeof(*head));
    DCFlushRange(qh, sizeof(*qh));
    ehci_write(0xcd040014u, 0x3fu);
    ehci_write(0xcd040028u, ehci_physical(head));
    ehci_write(0xcd040010u, 0x00080021u);

    for (elapsed = 0; elapsed < 500; ++elapsed) {
        DCInvalidateRange(&qtd[status_index], sizeof(qtd[status_index]));
        *final_token = __builtin_bswap32(qtd[status_index].token);
        if ((*final_token & 0x80u) == 0) {
            completed = (*final_token & 0x7cu) == 0;
            break;
        }
        usleep(1000);
    }
    *live_status = ehci_read(0xcd040014u);
    *live_command = ehci_read(0xcd040010u);
    DCInvalidateRange(qh, sizeof(*qh));
    DCInvalidateRange(&qtd[0], sizeof(qtd[0]));
    trace[0] = ehci_read(0xcd040028u);
    trace[1] = __builtin_bswap32(qh->current);
    trace[2] = __builtin_bswap32(qh->overlay_next);
    trace[3] = __builtin_bswap32(qh->overlay_token);
    trace[4] = __builtin_bswap32(qtd[0].token);
    DCInvalidateRange(head, sizeof(*head));
    trace[5] = __builtin_bswap32(head->horizontal);
    trace[6] = __builtin_bswap32(qh->horizontal);
    trace[7] = __builtin_bswap32(qh->endpoint);
    trace[8] = __builtin_bswap32(qh->capabilities);
    ehci_write(0xcd040010u, 0);
    ehci_wait_bits(0xcd040014u, 0x1000u, 0x1000u, 100);
    DCInvalidateRange(data, 32);
    if (completed && direction_in && result != NULL && length > 0)
        memcpy(result, data, length);
    return completed;
}

static bool ehci_enumerate_device(unsigned char descriptor[18],
                                  unsigned char config_header[9],
                                  uint32_t *final_token,
                                  uint32_t *live_status,
                                  uint32_t *live_command,
                                  uint32_t trace[9],
                                  unsigned int *stage,
                                  unsigned int *active_address) {
    static const unsigned char get_device[8] =
        {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 18, 0x00};
    static const unsigned char set_address[8] =
        {0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const unsigned char get_config[8] =
        {0x80, 0x06, 0x00, 0x02, 0x00, 0x00, 9, 0x00};

    *stage = 1;
    if (!ehci_control_transfer(0, get_device, 18, true, descriptor,
                               final_token, live_status, live_command, trace))
        return false;
    *stage = 2;
    if (!ehci_control_transfer(0, set_address, 0, false, NULL,
                               final_token, live_status, live_command, trace))
        return false;
    usleep(5000);
    *stage = 3;
    if (ehci_control_transfer(1, get_config, 9, true, config_header,
                              final_token, live_status, live_command, trace)) {
        *active_address = 1;
        return true;
    }
    *stage = 4;
    if (!ehci_control_transfer(0, get_config, 9, true, config_header,
                               final_token, live_status, live_command, trace))
        return false;
    *active_address = 0;
    return true;
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
    unsigned char config_header[9];
    uint32_t transfer_token = 0xffffffffu;
    uint32_t transfer_status = 0xffffffffu;
    uint32_t transfer_command = 0xffffffffu;
    uint32_t trace[9] = {0};
    unsigned int enumeration_stage = 0;
    unsigned int active_address = 0xffu;
    bool descriptor_ok;

    ehci_write(interrupt_register, 0);
    ehci_write(command_register, command & ~1u);
    halted = ehci_wait_bits(status_register, 0x1000u, 0x1000u, 100);
    ehci_write(command_register, (command & ~1u) | 2u);
    reset_done = ehci_wait_bits(command_register, 2u, 0, 100);

    ehci_write(periodic_register, 0);
    ehci_write(async_register, 0);
    ehci_write(0xcd0400ccu, ehci_read(0xcd0400ccu) | (1u << 15));
    ehci_write(config_register, 1);
    /* EHCI requires CMD_RUN before root-hub reset signalling. Clear PE when
     * asserting reset so the device returns to USB address zero. */
    ehci_write(command_register, 0x00080001u);
    ehci_wait_bits(status_register, 0x1000u, 0, 100);
    port = ehci_read(port_register) & ~(0x2au | 0x100u | 0x4u);
    ehci_write(port_register, port | 0x1000u | 0x100u);
    usleep(50000);
    port = ehci_read(port_register) & ~(0x2au | 0x100u);
    ehci_write(port_register, port | 0x1000u);
    port_reset_done = ehci_wait_bits(port_register, 0x100u, 0, 100);
    usleep(20000);

    printf("EHCI take halt:%u reset:%u preset:%u port:%08lx\n",
           halted, reset_done, port_reset_done,
           (unsigned long)ehci_read(port_register));
    descriptor_ok = ehci_enumerate_device(descriptor, config_header,
                                          &transfer_token, &transfer_status,
                                          &transfer_command, trace,
                                          &enumeration_stage,
                                          &active_address);
    if (descriptor_ok) {
        printf("EHCI dev %02x%02x VID:%02x%02x PID:%02x%02x mps:%u\n",
               descriptor[0], descriptor[1], descriptor[9], descriptor[8],
               descriptor[11], descriptor[10], descriptor[7]);
        printf("EHCI addr:%u cfglen:%u interfaces:%u cfgval:%u\n",
               active_address,
               (unsigned int)config_header[2] |
                   ((unsigned int)config_header[3] << 8),
               config_header[4], config_header[5]);
    } else {
        printf("EHCI enum stage:%u fail tok:%08lx sts:%08lx cmd:%08lx\n",
               enumeration_stage,
               (unsigned long)transfer_token,
               (unsigned long)transfer_status,
               (unsigned long)transfer_command);
        printf("ASY:%08lx cur:%08lx nxt:%08lx ovt:%08lx q0:%08lx\n",
               (unsigned long)trace[0], (unsigned long)trace[1],
               (unsigned long)trace[2], (unsigned long)trace[3],
               (unsigned long)trace[4]);
        printf("head:%08lx qlink:%08lx ep:%08lx cap:%08lx\n",
               (unsigned long)trace[5], (unsigned long)trace[6],
               (unsigned long)trace[7], (unsigned long)trace[8]);
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
    printf("\x1b[2;0HWiiCam WIP 0.2.17\n\n");
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

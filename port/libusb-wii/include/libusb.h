#ifndef LIBUSB_WII_H
#define LIBUSB_WII_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIBUSB_API_VERSION 0x01000106
#define LIBUSB_CALL
#define LIBUSB_ENDPOINT_IN 0x80
#define LIBUSB_ENDPOINT_OUT 0x00
#define LIBUSB_SUCCESS 0

enum libusb_error {
    LIBUSB_ERROR_IO = -1,
    LIBUSB_ERROR_INVALID_PARAM = -2,
    LIBUSB_ERROR_ACCESS = -3,
    LIBUSB_ERROR_NO_DEVICE = -4,
    LIBUSB_ERROR_NOT_FOUND = -5,
    LIBUSB_ERROR_BUSY = -6,
    LIBUSB_ERROR_TIMEOUT = -7,
    LIBUSB_ERROR_OVERFLOW = -8,
    LIBUSB_ERROR_PIPE = -9,
    LIBUSB_ERROR_INTERRUPTED = -10,
    LIBUSB_ERROR_NO_MEM = -11,
    LIBUSB_ERROR_NOT_SUPPORTED = -12,
    LIBUSB_ERROR_OTHER = -99
};

enum libusb_transfer_type {
    LIBUSB_TRANSFER_TYPE_CONTROL = 0,
    LIBUSB_TRANSFER_TYPE_ISOCHRONOUS = 1,
    LIBUSB_TRANSFER_TYPE_BULK = 2,
    LIBUSB_TRANSFER_TYPE_INTERRUPT = 3
};

enum libusb_transfer_status {
    LIBUSB_TRANSFER_COMPLETED,
    LIBUSB_TRANSFER_ERROR,
    LIBUSB_TRANSFER_TIMED_OUT,
    LIBUSB_TRANSFER_CANCELLED,
    LIBUSB_TRANSFER_STALL,
    LIBUSB_TRANSFER_NO_DEVICE,
    LIBUSB_TRANSFER_OVERFLOW
};

typedef struct libusb_context libusb_context;
typedef struct libusb_device libusb_device;
typedef struct libusb_device_handle libusb_device_handle;

struct libusb_device_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
};

struct libusb_endpoint_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
    uint8_t bRefresh;
    uint8_t bSynchAddress;
    const unsigned char *extra;
    int extra_length;
};

struct libusb_interface_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
    const struct libusb_endpoint_descriptor *endpoint;
    const unsigned char *extra;
    int extra_length;
};

struct libusb_interface {
    const struct libusb_interface_descriptor *altsetting;
    int num_altsetting;
};

struct libusb_config_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t MaxPower;
    const struct libusb_interface *interface;
    const unsigned char *extra;
    int extra_length;
};

struct libusb_ss_endpoint_companion_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bMaxBurst;
    uint8_t bmAttributes;
    uint16_t wBytesPerInterval;
};

struct libusb_iso_packet_descriptor {
    unsigned int length;
    unsigned int actual_length;
    enum libusb_transfer_status status;
};

struct libusb_transfer;
typedef void (LIBUSB_CALL *libusb_transfer_cb_fn)(struct libusb_transfer *transfer);

struct libusb_transfer {
    libusb_device_handle *dev_handle;
    uint8_t flags;
    unsigned char endpoint;
    unsigned char type;
    unsigned int timeout;
    enum libusb_transfer_status status;
    int length;
    int actual_length;
    libusb_transfer_cb_fn callback;
    void *user_data;
    unsigned char *buffer;
    int num_iso_packets;
    struct libusb_iso_packet_descriptor iso_packet_desc[];
};

int libusb_init(libusb_context **context);
void libusb_exit(libusb_context *context);
ssize_t libusb_get_device_list(libusb_context *context, libusb_device ***list);
void libusb_free_device_list(libusb_device **list, int unref_devices);
libusb_device *libusb_ref_device(libusb_device *device);
void libusb_unref_device(libusb_device *device);
int libusb_get_device_descriptor(libusb_device *device,
                                 struct libusb_device_descriptor *descriptor);
int libusb_get_config_descriptor(libusb_device *device, uint8_t config_index,
                                 struct libusb_config_descriptor **config);
void libusb_free_config_descriptor(struct libusb_config_descriptor *config);
uint8_t libusb_get_bus_number(libusb_device *device);
uint8_t libusb_get_device_address(libusb_device *device);
int libusb_open(libusb_device *device, libusb_device_handle **handle);
void libusb_close(libusb_device_handle *handle);
libusb_device *libusb_get_device(libusb_device_handle *handle);
int libusb_get_string_descriptor_ascii(libusb_device_handle *handle,
                                       uint8_t index, unsigned char *data,
                                       int length);
int libusb_claim_interface(libusb_device_handle *handle, int interface_number);
int libusb_release_interface(libusb_device_handle *handle, int interface_number);
int libusb_detach_kernel_driver(libusb_device_handle *handle, int interface_number);
int libusb_attach_kernel_driver(libusb_device_handle *handle, int interface_number);
int libusb_set_interface_alt_setting(libusb_device_handle *handle,
                                     int interface_number, int alternate_setting);
int libusb_control_transfer(libusb_device_handle *handle, uint8_t request_type,
                            uint8_t request, uint16_t value, uint16_t index,
                            unsigned char *data, uint16_t length,
                            unsigned int timeout);
struct libusb_transfer *libusb_alloc_transfer(int iso_packets);
void libusb_free_transfer(struct libusb_transfer *transfer);
int libusb_submit_transfer(struct libusb_transfer *transfer);
int libusb_cancel_transfer(struct libusb_transfer *transfer);
int libusb_handle_events(libusb_context *context);
int libusb_handle_events_completed(libusb_context *context, int *completed);
int libusb_get_ss_endpoint_companion_descriptor(
    libusb_context *context, const struct libusb_endpoint_descriptor *endpoint,
    struct libusb_ss_endpoint_companion_descriptor **companion);
void libusb_free_ss_endpoint_companion_descriptor(
    struct libusb_ss_endpoint_companion_descriptor *companion);

static inline void libusb_fill_bulk_transfer(
    struct libusb_transfer *transfer, libusb_device_handle *handle,
    unsigned char endpoint, unsigned char *buffer, int length,
    libusb_transfer_cb_fn callback, void *user_data, unsigned int timeout) {
    transfer->dev_handle = handle;
    transfer->endpoint = endpoint;
    transfer->type = LIBUSB_TRANSFER_TYPE_BULK;
    transfer->timeout = timeout;
    transfer->buffer = buffer;
    transfer->length = length;
    transfer->user_data = user_data;
    transfer->callback = callback;
}

static inline void libusb_fill_interrupt_transfer(
    struct libusb_transfer *transfer, libusb_device_handle *handle,
    unsigned char endpoint, unsigned char *buffer, int length,
    libusb_transfer_cb_fn callback, void *user_data, unsigned int timeout) {
    libusb_fill_bulk_transfer(transfer, handle, endpoint, buffer, length,
                              callback, user_data, timeout);
    transfer->type = LIBUSB_TRANSFER_TYPE_INTERRUPT;
}

static inline void libusb_fill_iso_transfer(
    struct libusb_transfer *transfer, libusb_device_handle *handle,
    unsigned char endpoint, unsigned char *buffer, int length,
    int iso_packets, libusb_transfer_cb_fn callback, void *user_data,
    unsigned int timeout) {
    transfer->dev_handle = handle;
    transfer->endpoint = endpoint;
    transfer->type = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
    transfer->timeout = timeout;
    transfer->buffer = buffer;
    transfer->length = length;
    transfer->num_iso_packets = iso_packets;
    transfer->user_data = user_data;
    transfer->callback = callback;
}

static inline void libusb_set_iso_packet_lengths(
    struct libusb_transfer *transfer, unsigned int length) {
    int i;
    for (i = 0; i < transfer->num_iso_packets; ++i)
        transfer->iso_packet_desc[i].length = length;
}

static inline unsigned char *libusb_get_iso_packet_buffer_simple(
    struct libusb_transfer *transfer, unsigned int packet) {
    if (packet >= (unsigned int)transfer->num_iso_packets) return NULL;
    return transfer->buffer + transfer->iso_packet_desc[0].length * packet;
}

#ifdef __cplusplus
}
#endif

#endif


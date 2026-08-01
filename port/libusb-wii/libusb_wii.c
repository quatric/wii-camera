#include <libusb.h>

#include <malloc.h>
#include <ogc/mutex.h>
#include <ogc/usb.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WII_USB_MAX_DEVICES 32
#define WII_USB_VIDEO_CLASS 14

struct libusb_context {
    bool initialized;
};

struct libusb_device {
    int references;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t address;
    usb_devdesc descriptors;
    bool descriptors_ready;
};

struct libusb_device_handle {
    libusb_device *device;
    int32_t fd;
};

typedef struct {
    mutex_t mutex;
    bool mutex_ready;
    bool in_flight;
    bool cancelled;
    unsigned char *dma_buffer;
    size_t dma_capacity;
    uint16_t *packet_sizes;
    int packet_capacity;
    struct libusb_transfer public_transfer;
} wii_transfer_t;

static wii_transfer_t *private_transfer(struct libusb_transfer *transfer) {
    return (wii_transfer_t *)((unsigned char *)transfer -
                             offsetof(wii_transfer_t, public_transfer));
}

static int map_ios_error(int result) {
    if (result >= 0) return LIBUSB_SUCCESS;
    return LIBUSB_ERROR_IO;
}

static int open_legacy_device(const libusb_device *device, int32_t *fd) {
    int result = USB_OpenDevice(USB_OH0_DEVICE_ID, device->vendor_id,
                                device->product_id, fd);
    return result < 0 ? LIBUSB_ERROR_NO_DEVICE : LIBUSB_SUCCESS;
}

static int read_descriptors(libusb_device *device, usb_devdesc *descriptors) {
    int32_t fd = -1;
    int result = open_legacy_device(device, &fd);
    if (result != LIBUSB_SUCCESS) return result;
    result = USB_GetDescriptors(fd, descriptors);
    USB_CloseDevice(&fd);
    return map_ios_error(result);
}

int libusb_init(libusb_context **context) {
    libusb_context *created;
    if (context == NULL) return LIBUSB_ERROR_INVALID_PARAM;
    created = calloc(1, sizeof(*created));
    if (created == NULL) return LIBUSB_ERROR_NO_MEM;
    if (USB_Initialize() < 0) {
        free(created);
        return LIBUSB_ERROR_IO;
    }
    created->initialized = true;
    *context = created;
    return LIBUSB_SUCCESS;
}

void libusb_exit(libusb_context *context) {
    if (context == NULL) return;
    if (context->initialized) USB_Deinitialize();
    free(context);
}

ssize_t libusb_get_device_list(libusb_context *context, libusb_device ***list) {
    usb_device_entry entries[WII_USB_MAX_DEVICES] __attribute__((aligned(32)));
    libusb_device **devices;
    uint8_t count = 0;
    size_t unique_count = 0;
    int attempt;
    size_t i;

    if (context == NULL || list == NULL) return LIBUSB_ERROR_INVALID_PARAM;
    memset(entries, 0, sizeof(entries));
    /* IOS58 populates /dev/usb/ven asynchronously after initialization. */
    for (attempt = 0; attempt < 10; ++attempt) {
        if (USB_GetDeviceList(entries, WII_USB_MAX_DEVICES,
                              WII_USB_VIDEO_CLASS, &count) < 0)
            return LIBUSB_ERROR_IO;
        if (count != 0) break;
        usleep(20000);
    }

    devices = calloc((size_t)count + 1, sizeof(*devices));
    if (devices == NULL) return LIBUSB_ERROR_NO_MEM;
    for (i = 0; i < count; ++i) {
        size_t duplicate;
        libusb_device *device;
        for (duplicate = 0; duplicate < unique_count; ++duplicate) {
            if (devices[duplicate]->vendor_id == entries[i].vid &&
                devices[duplicate]->product_id == entries[i].pid)
                break;
        }
        if (duplicate != unique_count) continue;
        device = calloc(1, sizeof(*device));
        if (device == NULL) {
            libusb_free_device_list(devices, 1);
            return LIBUSB_ERROR_NO_MEM;
        }
        device->references = 1;
        device->vendor_id = entries[i].vid;
        device->product_id = entries[i].pid;
        device->address = (uint8_t)(i + 1);
        if (read_descriptors(device, &device->descriptors) != LIBUSB_SUCCESS) {
            free(device);
            continue;
        }
        device->descriptors_ready = true;
        devices[unique_count++] = device;
    }
    devices[unique_count] = NULL;
    *list = devices;
    return (ssize_t)unique_count;
}

void libusb_free_device_list(libusb_device **list, int unref_devices) {
    size_t i;
    if (list == NULL) return;
    if (unref_devices) {
        for (i = 0; list[i] != NULL; ++i) libusb_unref_device(list[i]);
    }
    free(list);
}

libusb_device *libusb_ref_device(libusb_device *device) {
    if (device != NULL) ++device->references;
    return device;
}

void libusb_unref_device(libusb_device *device) {
    if (device != NULL && --device->references == 0) {
        if (device->descriptors_ready) USB_FreeDescriptors(&device->descriptors);
        free(device);
    }
}

int libusb_get_device_descriptor(libusb_device *device,
                                 struct libusb_device_descriptor *descriptor) {
    const usb_devdesc *source;
    if (device == NULL || descriptor == NULL) return LIBUSB_ERROR_INVALID_PARAM;
    if (!device->descriptors_ready) return LIBUSB_ERROR_NO_DEVICE;
    source = &device->descriptors;
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->bLength = source->bLength;
    descriptor->bDescriptorType = source->bDescriptorType;
    descriptor->bcdUSB = source->bcdUSB;
    descriptor->bDeviceClass = source->bDeviceClass;
    descriptor->bDeviceSubClass = source->bDeviceSubClass;
    descriptor->bDeviceProtocol = source->bDeviceProtocol;
    descriptor->bMaxPacketSize0 = source->bMaxPacketSize0;
    descriptor->idVendor = source->idVendor;
    descriptor->idProduct = source->idProduct;
    descriptor->bcdDevice = source->bcdDevice;
    descriptor->iManufacturer = source->iManufacturer;
    descriptor->iProduct = source->iProduct;
    descriptor->iSerialNumber = source->iSerialNumber;
    descriptor->bNumConfigurations = source->bNumConfigurations;
    return LIBUSB_SUCCESS;
}

static void free_interface_descriptor(struct libusb_interface_descriptor *interface) {
    uint8_t endpoint;
    struct libusb_endpoint_descriptor *endpoints =
        (struct libusb_endpoint_descriptor *)interface->endpoint;
    for (endpoint = 0; endpoint < interface->bNumEndpoints; ++endpoint)
        free((void *)endpoints[endpoint].extra);
    free(endpoints);
    free((void *)interface->extra);
}

void libusb_free_config_descriptor(struct libusb_config_descriptor *config) {
    uint8_t interface_number;
    struct libusb_interface *interfaces;
    if (config == NULL) return;
    interfaces = (struct libusb_interface *)config->interface;
    for (interface_number = 0; interface_number < config->bNumInterfaces;
         ++interface_number) {
        int alternate;
        struct libusb_interface_descriptor *settings =
            (struct libusb_interface_descriptor *)interfaces[interface_number].altsetting;
        for (alternate = 0; alternate < interfaces[interface_number].num_altsetting;
             ++alternate)
            free_interface_descriptor(&settings[alternate]);
        free(settings);
    }
    free(interfaces);
    free((void *)config->extra);
    free(config);
}

static bool copy_interface_descriptor(struct libusb_interface_descriptor *destination,
                                      const usb_interfacedesc *source) {
    uint8_t endpoint;
    struct libusb_endpoint_descriptor *endpoints = NULL;
    memset(destination, 0, sizeof(*destination));
    destination->bLength = source->bLength;
    destination->bDescriptorType = source->bDescriptorType;
    destination->bInterfaceNumber = source->bInterfaceNumber;
    destination->bAlternateSetting = source->bAlternateSetting;
    destination->bNumEndpoints = source->bNumEndpoints;
    destination->bInterfaceClass = source->bInterfaceClass;
    destination->bInterfaceSubClass = source->bInterfaceSubClass;
    destination->bInterfaceProtocol = source->bInterfaceProtocol;
    destination->iInterface = source->iInterface;
    if (source->extra_size > 0) {
        unsigned char *extra = malloc(source->extra_size);
        if (extra == NULL) return false;
        memcpy(extra, source->extra, source->extra_size);
        destination->extra = extra;
        destination->extra_length = source->extra_size;
    }
    if (source->bNumEndpoints > 0) {
        endpoints = calloc(source->bNumEndpoints, sizeof(*endpoints));
        if (endpoints == NULL) {
            free((void *)destination->extra);
            destination->extra = NULL;
            return false;
        }
        destination->endpoint = endpoints;
    }
    for (endpoint = 0; endpoint < source->bNumEndpoints; ++endpoint) {
        endpoints[endpoint].bLength = source->endpoints[endpoint].bLength;
        endpoints[endpoint].bDescriptorType = source->endpoints[endpoint].bDescriptorType;
        endpoints[endpoint].bEndpointAddress = source->endpoints[endpoint].bEndpointAddress;
        endpoints[endpoint].bmAttributes = source->endpoints[endpoint].bmAttributes;
        endpoints[endpoint].wMaxPacketSize = source->endpoints[endpoint].wMaxPacketSize;
        endpoints[endpoint].bInterval = source->endpoints[endpoint].bInterval;
    }
    return true;
}

int libusb_get_config_descriptor(libusb_device *device, uint8_t config_index,
                                 struct libusb_config_descriptor **config_out) {
    const usb_devdesc *source;
    usb_configurationdesc *source_config;
    struct libusb_config_descriptor *config;
    struct libusb_interface *interfaces;
    uint8_t max_interface = 0;
    uint8_t source_index;

    if (device == NULL || config_out == NULL) return LIBUSB_ERROR_INVALID_PARAM;
    if (!device->descriptors_ready) return LIBUSB_ERROR_NO_DEVICE;
    source = &device->descriptors;
    if (config_index >= source->bNumConfigurations) return LIBUSB_ERROR_NOT_FOUND;
    source_config = &source->configurations[config_index];
    for (source_index = 0; source_index < source_config->bNumInterfaces;
         ++source_index) {
        if (source_config->interfaces[source_index].bInterfaceNumber > max_interface)
            max_interface = source_config->interfaces[source_index].bInterfaceNumber;
    }

    config = calloc(1, sizeof(*config));
    interfaces = calloc((size_t)max_interface + 1, sizeof(*interfaces));
    if (config == NULL || interfaces == NULL) {
        free(config);
        free(interfaces);
        return LIBUSB_ERROR_NO_MEM;
    }
    config->bLength = source_config->bLength;
    config->bDescriptorType = source_config->bDescriptorType;
    config->wTotalLength = source_config->wTotalLength;
    config->bNumInterfaces = max_interface + 1;
    config->bConfigurationValue = source_config->bConfigurationValue;
    config->iConfiguration = source_config->iConfiguration;
    config->bmAttributes = source_config->bmAttributes;
    config->MaxPower = source_config->bMaxPower;
    config->interface = interfaces;

    for (source_index = 0; source_index < source_config->bNumInterfaces;
         ++source_index) {
        uint8_t number = source_config->interfaces[source_index].bInterfaceNumber;
        ++interfaces[number].num_altsetting;
    }
    for (source_index = 0; source_index <= max_interface; ++source_index) {
        int count = interfaces[source_index].num_altsetting;
        if (count > 0) {
            interfaces[source_index].altsetting =
                calloc((size_t)count, sizeof(struct libusb_interface_descriptor));
            if (interfaces[source_index].altsetting == NULL) {
                libusb_free_config_descriptor(config);
                return LIBUSB_ERROR_NO_MEM;
            }
            interfaces[source_index].num_altsetting = 0;
        }
    }
    for (source_index = 0; source_index < source_config->bNumInterfaces;
         ++source_index) {
        uint8_t number = source_config->interfaces[source_index].bInterfaceNumber;
        struct libusb_interface_descriptor *settings =
            (struct libusb_interface_descriptor *)interfaces[number].altsetting;
        int alternate = interfaces[number].num_altsetting;
        if (!copy_interface_descriptor(&settings[alternate],
                                       &source_config->interfaces[source_index])) {
            libusb_free_config_descriptor(config);
            return LIBUSB_ERROR_NO_MEM;
        }
        ++interfaces[number].num_altsetting;
    }
    *config_out = config;
    return LIBUSB_SUCCESS;
}

uint8_t libusb_get_bus_number(libusb_device *device) {
    (void)device;
    return 0;
}

uint8_t libusb_get_device_address(libusb_device *device) {
    return device == NULL ? 0 : device->address;
}

int libusb_open(libusb_device *device, libusb_device_handle **handle_out) {
    libusb_device_handle *handle;
    int result;
    if (device == NULL || handle_out == NULL) return LIBUSB_ERROR_INVALID_PARAM;
    handle = calloc(1, sizeof(*handle));
    if (handle == NULL) return LIBUSB_ERROR_NO_MEM;
    result = open_legacy_device(device, &handle->fd);
    if (result != LIBUSB_SUCCESS) {
        free(handle);
        return result;
    }
    handle->device = libusb_ref_device(device);
    *handle_out = handle;
    return LIBUSB_SUCCESS;
}

void libusb_close(libusb_device_handle *handle) {
    if (handle == NULL) return;
    USB_CloseDevice(&handle->fd);
    libusb_unref_device(handle->device);
    free(handle);
}

libusb_device *libusb_get_device(libusb_device_handle *handle) {
    return handle == NULL ? NULL : handle->device;
}

int libusb_get_string_descriptor_ascii(libusb_device_handle *handle,
                                       uint8_t index, unsigned char *data,
                                       int length) {
    int result;
    if (handle == NULL || data == NULL || length <= 0)
        return LIBUSB_ERROR_INVALID_PARAM;
    result = USB_GetAsciiString(handle->fd, index, 0x0409, (uint16_t)length, data);
    return result < 0 ? LIBUSB_ERROR_IO : result;
}

int libusb_claim_interface(libusb_device_handle *handle, int interface_number) {
    (void)interface_number;
    return handle == NULL ? LIBUSB_ERROR_INVALID_PARAM : LIBUSB_SUCCESS;
}

int libusb_release_interface(libusb_device_handle *handle, int interface_number) {
    (void)interface_number;
    return handle == NULL ? LIBUSB_ERROR_INVALID_PARAM : LIBUSB_SUCCESS;
}

int libusb_detach_kernel_driver(libusb_device_handle *handle, int interface_number) {
    (void)handle;
    (void)interface_number;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int libusb_attach_kernel_driver(libusb_device_handle *handle, int interface_number) {
    (void)handle;
    (void)interface_number;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int libusb_set_interface_alt_setting(libusb_device_handle *handle,
                                     int interface_number, int alternate_setting) {
    int result;
    if (handle == NULL) return LIBUSB_ERROR_INVALID_PARAM;
    result = USB_SetAlternativeInterface(handle->fd, (uint8_t)interface_number,
                                         (uint8_t)alternate_setting);
    return map_ios_error(result);
}

int libusb_control_transfer(libusb_device_handle *handle, uint8_t request_type,
                            uint8_t request, uint16_t value, uint16_t index,
                            unsigned char *data, uint16_t length,
                            unsigned int timeout) {
    unsigned char *aligned = NULL;
    int result;
    (void)timeout;
    if (handle == NULL || (length > 0 && data == NULL))
        return LIBUSB_ERROR_INVALID_PARAM;
    if (length > 0) {
        aligned = memalign(32, (length + 31u) & ~31u);
        if (aligned == NULL) return LIBUSB_ERROR_NO_MEM;
        if ((request_type & LIBUSB_ENDPOINT_IN) == 0) memcpy(aligned, data, length);
    }
    if (request_type & LIBUSB_ENDPOINT_IN)
        result = USB_ReadCtrlMsg(handle->fd, request_type, request, value, index,
                                 length, aligned);
    else
        result = USB_WriteCtrlMsg(handle->fd, request_type, request, value, index,
                                  length, aligned);
    if (result >= 0 && length > 0 && (request_type & LIBUSB_ENDPOINT_IN))
        memcpy(data, aligned, length);
    free(aligned);
    return result < 0 ? LIBUSB_ERROR_IO : result;
}

struct libusb_transfer *libusb_alloc_transfer(int iso_packets) {
    wii_transfer_t *transfer;
    size_t size;
    if (iso_packets < 0) return NULL;
    size = sizeof(*transfer) +
           (size_t)iso_packets * sizeof(struct libusb_iso_packet_descriptor);
    transfer = calloc(1, size);
    if (transfer == NULL) return NULL;
    if (LWP_MutexInit(&transfer->mutex, false) != 0) {
        free(transfer);
        return NULL;
    }
    transfer->mutex_ready = true;
    transfer->public_transfer.num_iso_packets = iso_packets;
    return &transfer->public_transfer;
}

void libusb_free_transfer(struct libusb_transfer *public_transfer) {
    wii_transfer_t *transfer;
    if (public_transfer == NULL) return;
    transfer = private_transfer(public_transfer);
    free(transfer->dma_buffer);
    free(transfer->packet_sizes);
    if (transfer->mutex_ready) LWP_MutexDestroy(transfer->mutex);
    free(transfer);
}

static int prepare_dma(wii_transfer_t *transfer) {
    struct libusb_transfer *public_transfer = &transfer->public_transfer;
    int packet;
    if (public_transfer->length <= 0 || public_transfer->length > 65535)
        return LIBUSB_ERROR_INVALID_PARAM;
    if (transfer->dma_capacity < (size_t)public_transfer->length) {
        unsigned char *replacement =
            memalign(32, ((size_t)public_transfer->length + 31u) & ~31u);
        if (replacement == NULL) return LIBUSB_ERROR_NO_MEM;
        free(transfer->dma_buffer);
        transfer->dma_buffer = replacement;
        transfer->dma_capacity = public_transfer->length;
    }
    if (public_transfer->num_iso_packets > transfer->packet_capacity) {
        uint16_t *replacement = memalign(
            32, ((size_t)public_transfer->num_iso_packets * sizeof(uint16_t) + 31u) & ~31u);
        if (replacement == NULL) return LIBUSB_ERROR_NO_MEM;
        free(transfer->packet_sizes);
        transfer->packet_sizes = replacement;
        transfer->packet_capacity = public_transfer->num_iso_packets;
    }
    for (packet = 0; packet < public_transfer->num_iso_packets; ++packet) {
        if (public_transfer->iso_packet_desc[packet].length > UINT16_MAX)
            return LIBUSB_ERROR_INVALID_PARAM;
        transfer->packet_sizes[packet] =
            (uint16_t)public_transfer->iso_packet_desc[packet].length;
    }
    if ((public_transfer->endpoint & LIBUSB_ENDPOINT_IN) == 0)
        memcpy(transfer->dma_buffer, public_transfer->buffer, public_transfer->length);
    return LIBUSB_SUCCESS;
}

static int transfer_complete(int result, void *user_data) {
    wii_transfer_t *transfer = user_data;
    struct libusb_transfer *public_transfer = &transfer->public_transfer;
    bool cancelled;
    int packet;

    LWP_MutexLock(transfer->mutex);
    transfer->in_flight = false;
    cancelled = transfer->cancelled;
    if (cancelled) {
        public_transfer->status = LIBUSB_TRANSFER_CANCELLED;
    } else if (result < 0) {
        public_transfer->status = LIBUSB_TRANSFER_ERROR;
    } else {
        public_transfer->status = LIBUSB_TRANSFER_COMPLETED;
        if (public_transfer->endpoint & LIBUSB_ENDPOINT_IN)
            memcpy(public_transfer->buffer, transfer->dma_buffer,
                   public_transfer->length);
    }
    public_transfer->actual_length = result < 0 ? 0 : result;
    if (public_transfer->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
        public_transfer->actual_length = 0;
        for (packet = 0; packet < public_transfer->num_iso_packets; ++packet) {
            public_transfer->iso_packet_desc[packet].actual_length =
                cancelled || result < 0 ? 0 : transfer->packet_sizes[packet];
            public_transfer->iso_packet_desc[packet].status =
                public_transfer->status;
            public_transfer->actual_length +=
                (int)public_transfer->iso_packet_desc[packet].actual_length;
        }
    }
    LWP_MutexUnlock(transfer->mutex);
    if (public_transfer->callback != NULL) public_transfer->callback(public_transfer);
    return 0;
}

int libusb_submit_transfer(struct libusb_transfer *public_transfer) {
    wii_transfer_t *transfer;
    int result;
    if (public_transfer == NULL || public_transfer->dev_handle == NULL ||
        public_transfer->buffer == NULL)
        return LIBUSB_ERROR_INVALID_PARAM;
    transfer = private_transfer(public_transfer);
    LWP_MutexLock(transfer->mutex);
    if (transfer->in_flight) {
        LWP_MutexUnlock(transfer->mutex);
        return LIBUSB_ERROR_BUSY;
    }
    result = prepare_dma(transfer);
    if (result != LIBUSB_SUCCESS) {
        LWP_MutexUnlock(transfer->mutex);
        return result;
    }
    transfer->cancelled = false;
    transfer->in_flight = true;
    if (public_transfer->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
        result = USB_ReadIsoMsgAsync(public_transfer->dev_handle->fd,
                                     public_transfer->endpoint,
                                     (uint8_t)public_transfer->num_iso_packets,
                                     transfer->packet_sizes, transfer->dma_buffer,
                                     transfer_complete, transfer);
    } else if (public_transfer->type == LIBUSB_TRANSFER_TYPE_BULK) {
        result = USB_ReadBlkMsgAsync(public_transfer->dev_handle->fd,
                                     public_transfer->endpoint,
                                     (uint16_t)public_transfer->length,
                                     transfer->dma_buffer,
                                     transfer_complete, transfer);
    } else if (public_transfer->type == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
        result = USB_ReadIntrMsgAsync(public_transfer->dev_handle->fd,
                                      public_transfer->endpoint,
                                      (uint16_t)public_transfer->length,
                                      transfer->dma_buffer,
                                      transfer_complete, transfer);
    } else {
        result = LIBUSB_ERROR_NOT_SUPPORTED;
    }
    if (result < 0) transfer->in_flight = false;
    LWP_MutexUnlock(transfer->mutex);
    return result < 0 ? LIBUSB_ERROR_IO : LIBUSB_SUCCESS;
}

int libusb_cancel_transfer(struct libusb_transfer *public_transfer) {
    wii_transfer_t *transfer;
    if (public_transfer == NULL) return LIBUSB_ERROR_INVALID_PARAM;
    transfer = private_transfer(public_transfer);
    LWP_MutexLock(transfer->mutex);
    if (!transfer->in_flight) {
        LWP_MutexUnlock(transfer->mutex);
        return LIBUSB_ERROR_NOT_FOUND;
    }
    /* IOS has no portable per-request cancel. A video request completes within
     * a few milliseconds; its completion is translated to CANCELLED. */
    transfer->cancelled = true;
    LWP_MutexUnlock(transfer->mutex);
    return LIBUSB_SUCCESS;
}

int libusb_handle_events(libusb_context *context) {
    (void)context;
    usleep(1000);
    return LIBUSB_SUCCESS;
}

int libusb_handle_events_completed(libusb_context *context, int *completed) {
    (void)context;
    if (completed == NULL || !*completed) usleep(1000);
    return LIBUSB_SUCCESS;
}

int libusb_get_ss_endpoint_companion_descriptor(
    libusb_context *context, const struct libusb_endpoint_descriptor *endpoint,
    struct libusb_ss_endpoint_companion_descriptor **companion) {
    (void)context;
    (void)endpoint;
    if (companion != NULL) *companion = NULL;
    return LIBUSB_ERROR_NOT_FOUND;
}

void libusb_free_ss_endpoint_companion_descriptor(
    struct libusb_ss_endpoint_companion_descriptor *companion) {
    free(companion);
}

#include <libusb.h>

#include <malloc.h>
#include <ogc/mutex.h>
#include <ogc/usb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WII_USB_MAX_DEVICES 32
#define WII_USB_VIDEO_CLASS 14
#define WII_USB_MAX_INTERFACES 32
#define WII_USB_NO_DEVICE_ID INT32_MIN

struct libusb_context {
    bool initialized;
};

struct libusb_device {
    int references;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t address;
    int32_t device_id;
    unsigned char device_descriptor[USB_DT_DEVICE_SIZE];
    unsigned char *configuration;
    uint16_t configuration_length;
    int32_t interface_device_ids[WII_USB_MAX_INTERFACES];
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

static char wii_usb_status[160] = "USB backend has not enumerated devices";

const char *libusb_wii_last_error(void) {
    return wii_usb_status;
}

static wii_transfer_t *private_transfer(struct libusb_transfer *transfer) {
    return (wii_transfer_t *)((unsigned char *)transfer -
                             offsetof(wii_transfer_t, public_transfer));
}

static int map_ios_error(int result) {
    if (result >= 0) return LIBUSB_SUCCESS;
    return LIBUSB_ERROR_IO;
}

static uint16_t read_le16(const unsigned char *value) {
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static int open_device(const libusb_device *device, int32_t *fd) {
    int result = USB_OpenDevice(device->device_id, device->vendor_id,
                                device->product_id, fd);
    return result < 0 ? LIBUSB_ERROR_NO_DEVICE : LIBUSB_SUCCESS;
}

static void initialize_interface_ids(libusb_device *device) {
    int interface_number;
    for (interface_number = 0; interface_number < WII_USB_MAX_INTERFACES;
         ++interface_number)
        device->interface_device_ids[interface_number] = WII_USB_NO_DEVICE_ID;
}

static void map_ios58_interfaces(libusb_device *device,
                                 const usb_device_entry *entries,
                                 uint8_t entry_count) {
    uint8_t entry_index;
    for (entry_index = 0; entry_index < entry_count; ++entry_index) {
        usb_devdesc descriptor;
        int32_t fd = -1;
        uint8_t configuration_index;
        if (entries[entry_index].vid != device->vendor_id ||
            entries[entry_index].pid != device->product_id)
            continue;
        if (USB_OpenDevice(entries[entry_index].device_id,
                           entries[entry_index].vid,
                           entries[entry_index].pid, &fd) < 0)
            continue;
        if (USB_GetDescriptors(fd, &descriptor) < 0) {
            USB_CloseDevice(&fd);
            continue;
        }
        for (configuration_index = 0;
             configuration_index < descriptor.bNumConfigurations;
             ++configuration_index) {
            usb_configurationdesc *configuration =
                &descriptor.configurations[configuration_index];
            uint8_t interface_index;
            for (interface_index = 0;
                 interface_index < configuration->bNumInterfaces;
                 ++interface_index) {
                uint8_t number = configuration->interfaces[interface_index].bInterfaceNumber;
                if (number < WII_USB_MAX_INTERFACES)
                    device->interface_device_ids[number] = entries[entry_index].device_id;
            }
        }
        USB_FreeDescriptors(&descriptor);
        USB_CloseDevice(&fd);
    }
}

static int32_t device_id_for_interface(const libusb_device *device,
                                       unsigned int interface_number) {
    if (interface_number < WII_USB_MAX_INTERFACES &&
        device->interface_device_ids[interface_number] != WII_USB_NO_DEVICE_ID)
        return device->interface_device_ids[interface_number];
    return device->device_id;
}

static int32_t device_id_for_endpoint(const libusb_device *device,
                                      uint8_t endpoint_address) {
    size_t offset = 0;
    unsigned int interface_number = WII_USB_MAX_INTERFACES;
    while (offset + 2 <= device->configuration_length) {
        const unsigned char *descriptor = device->configuration + offset;
        if (descriptor[0] < 2 || offset + descriptor[0] > device->configuration_length)
            break;
        if (descriptor[1] == USB_DT_INTERFACE && descriptor[0] >= USB_DT_INTERFACE_SIZE)
            interface_number = descriptor[2];
        else if (descriptor[1] == USB_DT_ENDPOINT &&
                 descriptor[0] >= USB_DT_ENDPOINT_SIZE &&
                 descriptor[2] == endpoint_address)
            return device_id_for_interface(device, interface_number);
        offset += descriptor[0];
    }
    return device->device_id;
}

static int read_raw_descriptors(int32_t fd, libusb_device *device) {
    unsigned char *buffer = memalign(32, 32);
    unsigned char *configuration = NULL;
    uint16_t total_length;
    int result;

    if (buffer == NULL) return LIBUSB_ERROR_NO_MEM;
    memset(buffer, 0, 32);
    result = USB_GetGenericDescriptor(fd, USB_DT_DEVICE, 0, 0,
                                      buffer, USB_DT_DEVICE_SIZE);
    if (result < 0 || buffer[0] < USB_DT_DEVICE_SIZE ||
        buffer[1] != USB_DT_DEVICE) {
        free(buffer);
        return LIBUSB_ERROR_IO;
    }
    memcpy(device->device_descriptor, buffer, USB_DT_DEVICE_SIZE);

    memset(buffer, 0, 32);
    result = USB_GetGenericDescriptor(fd, USB_DT_CONFIG, 0, 0,
                                      buffer, USB_DT_CONFIG_SIZE);
    if (result < 0 || buffer[0] < USB_DT_CONFIG_SIZE ||
        buffer[1] != USB_DT_CONFIG) {
        free(buffer);
        return LIBUSB_ERROR_IO;
    }
    total_length = read_le16(buffer + 2);
    free(buffer);
    if (total_length < USB_DT_CONFIG_SIZE) return LIBUSB_ERROR_IO;

    buffer = memalign(32, ((size_t)total_length + 31u) & ~31u);
    configuration = malloc(total_length);
    if (buffer == NULL || configuration == NULL) {
        free(buffer);
        free(configuration);
        return LIBUSB_ERROR_NO_MEM;
    }
    memset(buffer, 0, total_length);
    result = USB_GetGenericDescriptor(fd, USB_DT_CONFIG, 0, 0,
                                      buffer, total_length);
    if (result < 0 || buffer[1] != USB_DT_CONFIG) {
        free(buffer);
        free(configuration);
        return LIBUSB_ERROR_IO;
    }
    memcpy(configuration, buffer, total_length);
    free(buffer);
    device->configuration = configuration;
    device->configuration_length = total_length;
    return LIBUSB_SUCCESS;
}

static unsigned int configuration_uvc_mask(const libusb_device *device) {
    size_t offset = 0;
    bool control = false;
    bool streaming = false;
    while (offset + 2 <= device->configuration_length) {
        const unsigned char *descriptor = device->configuration + offset;
        if (descriptor[0] < 2 || offset + descriptor[0] > device->configuration_length)
            break;
        if (descriptor[1] == USB_DT_INTERFACE && descriptor[0] >= USB_DT_INTERFACE_SIZE) {
            if (descriptor[5] == WII_USB_VIDEO_CLASS && descriptor[6] == 1)
                control = true;
            if (descriptor[5] == WII_USB_VIDEO_CLASS && descriptor[6] == 2)
                streaming = true;
        }
        offset += descriptor[0];
    }
    return (control ? 1u : 0u) | (streaming ? 2u : 0u);
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
    unsigned int open_failures = 0;
    unsigned int descriptor_failures = 0;
    unsigned int incomplete_uvc = 0;
    unsigned int last_uvc_mask = 0;
    int attempt;
    size_t i;

    if (context == NULL || list == NULL) return LIBUSB_ERROR_INVALID_PARAM;
    memset(entries, 0, sizeof(entries));
    /* IOS58 populates /dev/usb/ven asynchronously after initialization. */
    for (attempt = 0; attempt < 50; ++attempt) {
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
        int32_t fd = -1;
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
        device->device_id = entries[i].device_id;
        initialize_interface_ids(device);
        if (open_device(device, &fd) != LIBUSB_SUCCESS) {
            ++open_failures;
            free(device);
            continue;
        }
        if (read_raw_descriptors(fd, device) != LIBUSB_SUCCESS) {
            ++descriptor_failures;
            if (fd >= 0) USB_CloseDevice(&fd);
            free(device->configuration);
            free(device);
            continue;
        }
        last_uvc_mask = configuration_uvc_mask(device);
        if (last_uvc_mask == 0) {
            ++incomplete_uvc;
            USB_CloseDevice(&fd);
            free(device->configuration);
            free(device);
            continue;
        }
        USB_CloseDevice(&fd);
        map_ios58_interfaces(device, entries, count);
        devices[unique_count++] = device;
    }
    /* Older IOS/libogc combinations only enumerate VID/PID. Try the legacy
     * OH0 path when IOS58's interface IDs did not yield a full UVC config. */
    if (unique_count == 0) {
        for (i = 0; i < count; ++i) {
            libusb_device *device;
            int32_t fd = -1;
            size_t duplicate;
            for (duplicate = 0; duplicate < i; ++duplicate) {
                if (entries[duplicate].vid == entries[i].vid &&
                    entries[duplicate].pid == entries[i].pid)
                    break;
            }
            if (duplicate != i) continue;
            device = calloc(1, sizeof(*device));
            if (device == NULL) break;
            device->references = 1;
            device->vendor_id = entries[i].vid;
            device->product_id = entries[i].pid;
            device->address = (uint8_t)(i + 1);
            device->device_id = USB_OH0_DEVICE_ID;
            initialize_interface_ids(device);
            if (open_device(device, &fd) == LIBUSB_SUCCESS &&
                read_raw_descriptors(fd, device) == LIBUSB_SUCCESS &&
                configuration_uvc_mask(device) != 0) {
                USB_CloseDevice(&fd);
                devices[unique_count++] = device;
            } else {
                if (fd >= 0) USB_CloseDevice(&fd);
                free(device->configuration);
                free(device);
            }
        }
    }
    devices[unique_count] = NULL;
    if (unique_count > 0) {
        snprintf(wii_usb_status, sizeof(wii_usb_status),
                 "IOS USB: %u, UVC:%u %04x:%04x mask:%u",
                 count, (unsigned int)unique_count,
                 devices[0]->vendor_id, devices[0]->product_id,
                 last_uvc_mask);
    } else {
        snprintf(wii_usb_status, sizeof(wii_usb_status),
                 "IOS USB: %u; open:%u desc:%u nonuvc:%u mask:%u",
                 count, open_failures, descriptor_failures, incomplete_uvc,
                 last_uvc_mask);
    }
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
        free(device->configuration);
        free(device);
    }
}

int libusb_get_device_descriptor(libusb_device *device,
                                 struct libusb_device_descriptor *descriptor) {
    const unsigned char *source;
    if (device == NULL || descriptor == NULL) return LIBUSB_ERROR_INVALID_PARAM;
    source = device->device_descriptor;
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->bLength = source[0];
    descriptor->bDescriptorType = source[1];
    descriptor->bcdUSB = read_le16(source + 2);
    descriptor->bDeviceClass = source[4];
    descriptor->bDeviceSubClass = source[5];
    descriptor->bDeviceProtocol = source[6];
    descriptor->bMaxPacketSize0 = source[7];
    descriptor->idVendor = read_le16(source + 8);
    descriptor->idProduct = read_le16(source + 10);
    descriptor->bcdDevice = read_le16(source + 12);
    descriptor->iManufacturer = source[14];
    descriptor->iProduct = source[15];
    descriptor->iSerialNumber = source[16];
    descriptor->bNumConfigurations = source[17];
    return LIBUSB_SUCCESS;
}

static void free_interface_descriptor(struct libusb_interface_descriptor *interface) {
    uint8_t endpoint;
    struct libusb_endpoint_descriptor *endpoints =
        (struct libusb_endpoint_descriptor *)interface->endpoint;
    if (endpoints != NULL) {
        for (endpoint = 0; endpoint < interface->bNumEndpoints; ++endpoint)
            free((void *)endpoints[endpoint].extra);
    }
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

int libusb_get_config_descriptor(libusb_device *device, uint8_t config_index,
                                 struct libusb_config_descriptor **config_out) {
    const unsigned char *source;
    size_t length;
    struct libusb_config_descriptor *config;
    struct libusb_interface *interfaces;
    uint8_t max_interface = 0;
    size_t offset;

    if (device == NULL || config_out == NULL) return LIBUSB_ERROR_INVALID_PARAM;
    if (config_index != 0 || device->configuration == NULL)
        return LIBUSB_ERROR_NOT_FOUND;
    source = device->configuration;
    length = device->configuration_length;
    for (offset = source[0]; offset + 2 <= length; offset += source[offset]) {
        if (source[offset] < 2 || offset + source[offset] > length) break;
        if (source[offset + 1] == USB_DT_INTERFACE &&
            source[offset] >= USB_DT_INTERFACE_SIZE &&
            source[offset + 2] > max_interface)
            max_interface = source[offset + 2];
    }

    config = calloc(1, sizeof(*config));
    interfaces = calloc((size_t)max_interface + 1, sizeof(*interfaces));
    if (config == NULL || interfaces == NULL) {
        free(config);
        free(interfaces);
        return LIBUSB_ERROR_NO_MEM;
    }
    config->bLength = source[0];
    config->bDescriptorType = source[1];
    config->wTotalLength = read_le16(source + 2);
    config->bNumInterfaces = max_interface + 1;
    config->bConfigurationValue = source[5];
    config->iConfiguration = source[6];
    config->bmAttributes = source[7];
    config->MaxPower = source[8];
    config->interface = interfaces;

    for (offset = source[0]; offset + 2 <= length; offset += source[offset]) {
        if (source[offset] < 2 || offset + source[offset] > length) break;
        if (source[offset + 1] == USB_DT_INTERFACE &&
            source[offset] >= USB_DT_INTERFACE_SIZE)
            ++interfaces[source[offset + 2]].num_altsetting;
    }
    for (offset = 0; offset <= max_interface; ++offset) {
        int count = interfaces[offset].num_altsetting;
        if (count > 0) {
            interfaces[offset].altsetting =
                calloc((size_t)count, sizeof(struct libusb_interface_descriptor));
            if (interfaces[offset].altsetting == NULL) {
                libusb_free_config_descriptor(config);
                return LIBUSB_ERROR_NO_MEM;
            }
            interfaces[offset].num_altsetting = 0;
        }
    }

    offset = source[0];
    while (offset + USB_DT_INTERFACE_SIZE <= length) {
        const unsigned char *raw = source + offset;
        size_t end;
        size_t cursor;
        uint8_t number;
        int alternate;
        struct libusb_interface_descriptor *settings;
        struct libusb_interface_descriptor *destination;
        struct libusb_endpoint_descriptor *endpoints = NULL;
        unsigned int endpoint_index = 0;

        if (raw[0] < 2 || offset + raw[0] > length) break;
        if (raw[1] != USB_DT_INTERFACE || raw[0] < USB_DT_INTERFACE_SIZE) {
            offset += raw[0];
            continue;
        }
        end = offset + raw[0];
        while (end + 2 <= length) {
            if (source[end] < 2 || end + source[end] > length) break;
            if (source[end + 1] == USB_DT_INTERFACE) break;
            end += source[end];
        }
        number = raw[2];
        alternate = interfaces[number].num_altsetting;
        settings = (struct libusb_interface_descriptor *)interfaces[number].altsetting;
        destination = &settings[alternate];
        destination->bLength = raw[0];
        destination->bDescriptorType = raw[1];
        destination->bInterfaceNumber = number;
        destination->bAlternateSetting = raw[3];
        destination->bNumEndpoints = raw[4];
        destination->bInterfaceClass = raw[5];
        destination->bInterfaceSubClass = raw[6];
        destination->bInterfaceProtocol = raw[7];
        destination->iInterface = raw[8];

        cursor = offset + raw[0];
        {
            size_t extra_end = cursor;
            while (extra_end + 2 <= end && source[extra_end + 1] != USB_DT_ENDPOINT) {
                if (source[extra_end] < 2 || extra_end + source[extra_end] > end)
                    break;
                extra_end += source[extra_end];
            }
            if (extra_end > cursor) {
                unsigned char *extra = malloc(extra_end - cursor);
                if (extra == NULL) {
                    libusb_free_config_descriptor(config);
                    return LIBUSB_ERROR_NO_MEM;
                }
                memcpy(extra, source + cursor, extra_end - cursor);
                destination->extra = extra;
                destination->extra_length = (int)(extra_end - cursor);
            }
        }
        if (destination->bNumEndpoints > 0) {
            endpoints = calloc(destination->bNumEndpoints, sizeof(*endpoints));
            if (endpoints == NULL) {
                libusb_free_config_descriptor(config);
                return LIBUSB_ERROR_NO_MEM;
            }
            destination->endpoint = endpoints;
        }
        for (cursor = offset + raw[0]; cursor + 2 <= end; cursor += source[cursor]) {
            const unsigned char *endpoint = source + cursor;
            if (endpoint[0] < 2 || cursor + endpoint[0] > end) break;
            if (endpoint[1] != USB_DT_ENDPOINT || endpoint[0] < USB_DT_ENDPOINT_SIZE ||
                endpoint_index >= destination->bNumEndpoints)
                continue;
            endpoints[endpoint_index].bLength = endpoint[0];
            endpoints[endpoint_index].bDescriptorType = endpoint[1];
            endpoints[endpoint_index].bEndpointAddress = endpoint[2];
            endpoints[endpoint_index].bmAttributes = endpoint[3];
            endpoints[endpoint_index].wMaxPacketSize = read_le16(endpoint + 4);
            endpoints[endpoint_index].bInterval = endpoint[6];
            if (endpoint[0] >= 8) endpoints[endpoint_index].bRefresh = endpoint[7];
            if (endpoint[0] >= 9) endpoints[endpoint_index].bSynchAddress = endpoint[8];
            ++endpoint_index;
        }
        ++interfaces[number].num_altsetting;
        offset = end;
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
    result = open_device(device, &handle->fd);
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
    int32_t fd;
    if (handle == NULL) return LIBUSB_ERROR_INVALID_PARAM;
    fd = device_id_for_interface(handle->device, (unsigned int)interface_number);
    result = USB_SetAlternativeInterface(fd, (uint8_t)interface_number,
                                         (uint8_t)alternate_setting);
    return map_ios_error(result);
}

int libusb_control_transfer(libusb_device_handle *handle, uint8_t request_type,
                            uint8_t request, uint16_t value, uint16_t index,
                            unsigned char *data, uint16_t length,
                            unsigned int timeout) {
    unsigned char *aligned = NULL;
    int result;
    int32_t fd;
    (void)timeout;
    if (handle == NULL || (length > 0 && data == NULL))
        return LIBUSB_ERROR_INVALID_PARAM;
    if (length > 0) {
        aligned = memalign(32, (length + 31u) & ~31u);
        if (aligned == NULL) return LIBUSB_ERROR_NO_MEM;
        if ((request_type & LIBUSB_ENDPOINT_IN) == 0) memcpy(aligned, data, length);
    }
    fd = device_id_for_interface(handle->device, index & 0xffu);
    if (request_type & LIBUSB_ENDPOINT_IN)
        result = USB_ReadCtrlMsg(fd, request_type, request, value, index,
                                 length, aligned);
    else
        result = USB_WriteCtrlMsg(fd, request_type, request, value, index,
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
    int32_t fd;
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
    fd = device_id_for_endpoint(public_transfer->dev_handle->device,
                                public_transfer->endpoint);
    if (public_transfer->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
        result = USB_ReadIsoMsgAsync(fd,
                                     public_transfer->endpoint,
                                     (uint8_t)public_transfer->num_iso_packets,
                                     transfer->packet_sizes, transfer->dma_buffer,
                                     transfer_complete, transfer);
    } else if (public_transfer->type == LIBUSB_TRANSFER_TYPE_BULK) {
        result = USB_ReadBlkMsgAsync(fd,
                                     public_transfer->endpoint,
                                     (uint16_t)public_transfer->length,
                                     transfer->dma_buffer,
                                     transfer_complete, transfer);
    } else if (public_transfer->type == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
        result = USB_ReadIntrMsgAsync(fd,
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

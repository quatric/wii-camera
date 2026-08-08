# libuvc on Wii

This port keeps libuvc's public API intact and supplies the libusb subset it
uses through `port/libusb-wii`.

## Data path

1. `libusb_get_device_list` enumerates IOS58 `/dev/usb/ven` device IDs.
2. The backend reads each candidate's raw device and configuration descriptors
   with `GET_DESCRIPTOR`, preserving every interface alternate setting.
3. Candidates must contain both UVC VideoControl and VideoStreaming interfaces.
   IOS58 may expose a composite webcam once per interface, so matching entries
   are collected into one libusb-compatible device and mapped by interface.
4. libuvc performs its normal UVC probe and commit control requests.
5. Control and streaming requests are routed to the IOS58 device ID belonging
   to the target interface or endpoint. This is required for composite cameras
   such as the Logitech C920.
6. `libusb_submit_transfer` converts libuvc transfers to
   `USB_ReadIsoMsgAsync`, `USB_ReadBlkMsgAsync`, or `USB_ReadIntrMsgAsync`.
7. IOS writes into 32-byte-aligned bounce buffers. Completion copies data into
   libuvc's buffers and invokes the original libusb callback.

## Wii-specific libuvc changes

Two small changes are applied to the vendored libuvc source:

- `stream.c` caps an isochronous transfer at 65,535 bytes, the maximum accepted
  by libogc's `USB_ReadIsoMsgAsync` request structure.
- `device.c` skips the optional asynchronous UVC status endpoint. Streaming and
  synchronous controls do not depend on it, while IOS cannot reliably cancel a
  single pending interrupt transfer during shutdown.

## Hardware test checklist

1. Launch from Homebrew Channel under IOS58 with AHB access.
2. Confirm the camera appears and the UVC probe/commit succeeds.
3. Confirm continuous preview for at least five minutes.
4. Take repeated photos and verify every file contains an SOF0 marker.
5. Exit during streaming and confirm no shutdown hang.
6. Repeat disconnect/reconnect tests with full-speed and high-speed webcams.

If IOS58 enumeration produces no usable UVC device, the backend retains a
legacy `/dev/usb/oh0/VID/PID` fallback. The app displays the backend's scan
summary on failure, including the number of IOS USB entries and UVC devices;
capture that line when reporting hardware-test results.

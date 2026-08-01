# libuvc on Wii

This port keeps libuvc's public API intact and supplies the libusb subset it
uses through `port/libusb-wii`.

## Data path

1. `libusb_get_device_list` enumerates non-HID USB devices through libogc.
2. Each candidate is opened through IOS58's legacy `/dev/usb/oh0/VID/PID`
   device path so the complete configuration and alternate settings are
   visible to libuvc.
3. libogc descriptors are deep-copied into libusb-compatible descriptor trees.
   Alternate settings flattened by libogc are regrouped by interface number.
4. libuvc performs its normal UVC probe and commit control requests.
5. `libusb_submit_transfer` converts libuvc transfers to
   `USB_ReadIsoMsgAsync`, `USB_ReadBlkMsgAsync`, or `USB_ReadIntrMsgAsync`.
6. IOS writes into 32-byte-aligned bounce buffers. Completion copies data into
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

If enumeration fails, first verify that `/dev/usb/oh0/VID/PID` can be opened on
the active IOS. If streaming negotiation fails, log the advertised format and
alternate-setting descriptors before changing the libuvc parser.

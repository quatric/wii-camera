# WiiCam (WIP)

WiiCam is an experimental Wii homebrew camera app. It captures a standard USB
Video Class camera through **libuvc**, displays its YUYV stream directly in the
Wii's YCbCr framebuffer, and saves photos as verified baseline JPEG files in:

- `sd:/wiicam`, preferred when an SD card is mounted
- `usb:/wiicam`, used when SD is unavailable

Press **A** on a Wii Remote or GameCube controller to take a photo. Press
**HOME**, **B**, or GameCube **START** to exit.

## Wii libuvc port

The repository includes libuvc 0.0.7 and a purpose-built compatibility layer in
`port/libusb-wii`. It maps the subset of libusb used by libuvc to libogc's IOS
USB driver, including device discovery, descriptors, control requests,
alternate interfaces, and asynchronous isochronous/bulk transfers.

The port uses aligned DMA bounce buffers because IOS requires USB buffers to be
32-byte aligned. It also limits each isochronous batch to 65,535 bytes, matching
libogc's request-size field. The optional UVC status interrupt endpoint is
disabled because IOS has no dependable per-request cancellation primitive;
normal camera control and video streaming remain available.

This is a first hardware-test build. It compiles and links completely, but has
not yet been exercised with a physical Wii and webcam. It expects IOS58's
legacy `/dev/usb/oh0` interface, which exposes all alternate settings of a
composite UVC device through one handle.

## Dependencies

- devkitPro with devkitPPC, libogc, and libfat-ogc
- ppc-libjpeg-turbo
- A UVC webcam that supports the requested YUYV mode

On macOS or Linux, the packaged dependencies are normally installed with:

```sh
dkp-pacman -S wii-dev libfat-ogc ppc-libjpeg-turbo
```

Build and make a Homebrew Channel-ready directory:

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC="$DEVKITPRO/devkitPPC"
make package
```

Copy `dist/apps/wiicam` to `apps/wiicam` on the SD card. The `<ahb_access/>` and
`<no_ios_reload/>` entries are intentional: the USB backend needs IOS58 USB
access when launched from the Homebrew Channel.

## JPEG guarantee

Each snapshot is decoded from YUYV and encoded with libjpeg. Quantization tables
are clamped to baseline-compatible 8-bit values, arithmetic and progressive
coding are disabled, and the finished file is scanned for an SOF0 marker. A file
that does not verify as baseline JPEG is deleted and reported as a failed save.

## Camera compatibility

The first attached UVC device is opened. It must advertise 640x480 YUYV at 15
fps. This conservative mode is deliberate, but older or MJPEG-only webcams will
fail with a clear on-screen error. Changing `CAPTURE_WIDTH`, `CAPTURE_HEIGHT`,
and `CAPTURE_FPS` in `source/main.c` changes the requested mode.

## Port status and limitations

- Only device-to-host isochronous, bulk, and interrupt asynchronous transfers
  are implemented because those are the paths libuvc uses for capture.
- Transfer timeout values are currently advisory; IOS owns transfer timing.
- Cancelling a streaming request marks its next completion as cancelled. Video
  requests complete quickly, but there is no hard per-request abort.
- Devices are identified by VID/PID through the legacy IOS path. Two identical
  webcams connected simultaneously are not distinguishable yet.
- Disconnect handling and broader webcam mode fallback need hardware testing.

Implementation details are in [PORTING.md](PORTING.md).

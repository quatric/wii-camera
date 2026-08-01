#ifndef LIBUVC_CONFIG_H
#define LIBUVC_CONFIG_H

#define LIBUVC_VERSION_MAJOR 0
#define LIBUVC_VERSION_MINOR 0
#define LIBUVC_VERSION_PATCH 7
#define LIBUVC_VERSION_STR "0.0.7-wii"
#define LIBUVC_VERSION_INT 7
#define LIBUVC_VERSION_GTE(major, minor, patch) \
    (LIBUVC_VERSION_INT >= (((major) << 16) | ((minor) << 8) | (patch)))

/* WiiCam negotiates YUYV and does its own snapshot encoding. */

#endif


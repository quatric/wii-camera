#include "storage.h"

#include <errno.h>
#include <fat.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static bool directory_exists(const char *path) {
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

bool storage_init(char *folder, size_t folder_size,
                  char *error, size_t error_size) {
    const char *root = NULL;

    if (!fatInitDefault()) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "Could not mount SD or USB storage");
        return false;
    }
    if (directory_exists("sd:/")) root = "sd:/wiicam";
    else if (directory_exists("usb:/")) root = "usb:/wiicam";

    if (root == NULL) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "No writable SD or USB volume found");
        return false;
    }
    if (mkdir(root, 0777) != 0 && errno != EEXIST) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "Could not create %s", root);
        return false;
    }
    if (snprintf(folder, folder_size, "%s", root) >= (int)folder_size) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "Storage path is too long");
        return false;
    }
    return true;
}

bool storage_next_filename(const char *folder, char *path, size_t path_size) {
    time_t now = time(NULL);
    struct tm local;
    unsigned int suffix;

    if (localtime_r(&now, &local) == NULL) memset(&local, 0, sizeof(local));
    for (suffix = 0; suffix < 10000; ++suffix) {
        struct stat status;
        int length = snprintf(path, path_size,
            "%s/IMG_%04d%02d%02d_%02d%02d%02d_%03u.jpg",
            folder, local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
            local.tm_hour, local.tm_min, local.tm_sec, suffix);
        if (length < 0 || (size_t)length >= path_size) return false;
        if (stat(path, &status) != 0 && errno == ENOENT) return true;
    }
    return false;
}


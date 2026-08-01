#ifndef WIICAM_STORAGE_H
#define WIICAM_STORAGE_H

#include <stdbool.h>
#include <stddef.h>

bool storage_init(char *folder, size_t folder_size,
                  char *error, size_t error_size);
bool storage_next_filename(const char *folder, char *path, size_t path_size);

#endif


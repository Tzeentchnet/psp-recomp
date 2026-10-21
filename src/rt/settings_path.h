#ifndef PSP_RECOMP_SETTINGS_PATH_H
#define PSP_RECOMP_SETTINGS_PATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int sr_settings_path(
    const char *filename, char *path, size_t path_size,
    char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif

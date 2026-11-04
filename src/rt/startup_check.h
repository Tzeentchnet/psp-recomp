#ifndef PSP_RECOMP_STARTUP_CHECK_H
#define PSP_RECOMP_STARTUP_CHECK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int sr_startup_validate(void);
int sr_startup_resolve_iso_path(
    char *path, size_t path_size, char *error, size_t error_size);
int sr_startup_validate_iso_path(
    const char *path, char *error, size_t error_size);
int sr_startup_save_iso_path(
    const char *path, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif

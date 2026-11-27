#ifndef PSP_RECOMP_SETTINGS_PATH_H
#define PSP_RECOMP_SETTINGS_PATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The player's data folder from SR_DATA_DIR (set by --data-dir), or NULL. When set it
 * replaces the per-user data root and holds the settings files, memstick/ and logs/. */
const char *sr_data_dir(void);

/* Creates the missing directories above path. */
int sr_make_parent_directories(
    const char *path, char *error, size_t error_size);

/* A settings file: SR_SETTINGS_DIR, else the SR_DATA_DIR data folder, else the platform
 * default (beside the executable on Windows, the user data folder on macOS). */
int sr_settings_path(
    const char *filename, char *path, size_t path_size,
    char *error, size_t error_size);
int sr_resource_path(
    const char *filename, char *path, size_t path_size,
    char *error, size_t error_size);
int sr_data_root(
    char *path, size_t path_size, char *error, size_t error_size);
int sr_data_path(
    const char *filename, char *path, size_t path_size,
    char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif

#ifndef PSP_RECOMP_SETTINGS_PATH_H
#define PSP_RECOMP_SETTINGS_PATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The player's data folder from SR_DATA_DIR (set by --data-dir), or NULL.
 * It holds graphics.ini, input.ini, memstick\ and logs\. */
const char *sr_data_dir(void);

/* Creates directory and its missing parents. */
void sr_make_directories(const char *directory);

/* <data dir>\<relative>. Returns 0 when no data folder is set. */
int sr_data_path(const char *relative, char *path, size_t path_size);

/* A settings file: SR_SETTINGS_DIR, else the data folder, else beside the
 * executable. */
int sr_settings_path(
    const char *filename, char *path, size_t path_size,
    char *error, size_t error_size);

/* A file shipped beside the executable (assets, fonts). */
int sr_program_path(
    const char *filename, char *path, size_t path_size,
    char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "settings_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static void set_error(char *error, size_t error_size, const char *message) {
    if (error && error_size)
        snprintf(error, error_size, "%s", message);
}

int sr_settings_path(
    const char *filename, char *path, size_t path_size,
    char *error, size_t error_size) {
    char directory[1024];
    const char *override;
    size_t directory_length;
    int length;

    if (error && error_size) error[0] = 0;
    if (!filename || !filename[0] || !path || !path_size) {
        set_error(error, error_size, "invalid settings path arguments");
        return 0;
    }

    override = getenv("SR_SETTINGS_DIR");
    if (override && override[0]) {
        directory_length = strlen(override);
        if (directory_length >= sizeof(directory)) {
            set_error(error, error_size, "SR_SETTINGS_DIR is too long");
            return 0;
        }
        memcpy(directory, override, directory_length + 1u);
    } else {
        DWORD result = GetModuleFileNameA(
            NULL, directory, (DWORD)sizeof(directory));
        char *separator;
        if (!result) {
            char message[128];
            snprintf(
                message, sizeof(message),
                "cannot locate executable directory (error %lu)",
                (unsigned long)GetLastError());
            set_error(error, error_size, message);
            return 0;
        }
        if (result >= sizeof(directory)) {
            set_error(error, error_size, "executable path is too long");
            return 0;
        }
        separator = strrchr(directory, '\\');
        if (!separator) separator = strrchr(directory, '/');
        if (!separator) {
            set_error(error, error_size, "executable path has no directory");
            return 0;
        }
        *separator = 0;
        directory_length = (size_t)(separator - directory);
    }

    directory_length = strlen(directory);
    length = snprintf(
        path, path_size, "%s%s%s",
        directory,
        directory_length &&
                directory[directory_length - 1u] != '\\' &&
                directory[directory_length - 1u] != '/'
            ? "\\" : "",
        filename);
    if (length < 0 || (size_t)length >= path_size) {
        set_error(error, error_size, "settings path is too long");
        return 0;
    }
    return 1;
}

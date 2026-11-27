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

static int is_separator(char c) {
    return c == '\\' || c == '/';
}

static int join_path(
    const char *directory, const char *filename, char *path, size_t path_size) {
    size_t directory_length = strlen(directory);
    int length = snprintf(
        path, path_size, "%s%s%s",
        directory,
        directory_length && !is_separator(directory[directory_length - 1u])
            ? "\\" : "",
        filename);
    return length >= 0 && (size_t)length < path_size;
}

static int executable_directory(
    char *directory, size_t directory_size, char *error, size_t error_size) {
    DWORD result = GetModuleFileNameA(NULL, directory, (DWORD)directory_size);
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
    if (result >= directory_size) {
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
    return 1;
}

void sr_make_directories(const char *directory) {
    char path[1024];
    size_t length;
    if (!directory || !directory[0]) return;
    length = strlen(directory);
    if (length >= sizeof(path)) return;
    memcpy(path, directory, length + 1u);
    for (char *p = path + 1; *p; p++) {
        if (!is_separator(*p)) continue;
        if (p == path + 2 && path[1] == ':') continue;   /* C:\ */
        if (p == path + 1 && is_separator(path[0])) continue;  /* \\server */
        *p = 0;
        CreateDirectoryA(path, NULL);
        *p = '\\';
    }
    CreateDirectoryA(path, NULL);
}

const char *sr_data_dir(void) {
    const char *value = getenv("SR_DATA_DIR");
    return value && value[0] ? value : NULL;
}

int sr_data_path(const char *relative, char *path, size_t path_size) {
    const char *directory = sr_data_dir();
    if (!directory || !relative || !path || !path_size) return 0;
    return join_path(directory, relative, path, path_size);
}

int sr_settings_path(
    const char *filename, char *path, size_t path_size,
    char *error, size_t error_size) {
    char directory[1024];
    const char *override;

    if (error && error_size) error[0] = 0;
    if (!filename || !filename[0] || !path || !path_size) {
        set_error(error, error_size, "invalid settings path arguments");
        return 0;
    }

    override = getenv("SR_SETTINGS_DIR");
    if (!override || !override[0]) override = sr_data_dir();
    if (override) {
        if (strlen(override) >= sizeof(directory)) {
            set_error(error, error_size, "settings directory is too long");
            return 0;
        }
        strcpy(directory, override);
        sr_make_directories(directory);
    } else if (!executable_directory(
                   directory, sizeof(directory), error, error_size)) {
        return 0;
    }

    if (!join_path(directory, filename, path, path_size)) {
        set_error(error, error_size, "settings path is too long");
        return 0;
    }
    return 1;
}

int sr_program_path(
    const char *filename, char *path, size_t path_size,
    char *error, size_t error_size) {
    char directory[1024];
    if (error && error_size) error[0] = 0;
    if (!filename || !filename[0] || !path || !path_size) {
        set_error(error, error_size, "invalid program path arguments");
        return 0;
    }
    if (!executable_directory(directory, sizeof(directory), error, error_size))
        return 0;
    if (!join_path(directory, filename, path, path_size)) {
        set_error(error, error_size, "program path is too long");
        return 0;
    }
    return 1;
}

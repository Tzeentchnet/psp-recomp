#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "settings_path.h"
#include "platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SR_APPLICATION_DATA_NAME
#define SR_APPLICATION_DATA_NAME "PSP Recomp"
#endif

static void set_error(char *error, size_t error_size, const char *message) {
    if (error && error_size)
        snprintf(error, error_size, "%s", message);
}

static int is_separator(char value) {
    return value == '/' || value == '\\';
}

static int relative_path_valid(const char *path) {
    const char *component;

    if (!path || !path[0] || is_separator(path[0])) return 0;
    if (path[0] && path[1] == ':') return 0;
    component = path;
    for (const char *cursor = path;; cursor++) {
        if (*cursor && !is_separator(*cursor)) continue;
        if (cursor == component ||
            (cursor - component == 1 && component[0] == '.') ||
            (cursor - component == 2 &&
             component[0] == '.' && component[1] == '.')) {
            return 0;
        }
        if (!*cursor) break;
        component = cursor + 1;
    }
    return 1;
}

static int path_join(
    const char *directory, const char *filename,
    char *path, size_t path_size,
    char *error, size_t error_size,
    const char *too_long_message) {
    size_t directory_length;
    int length;

    if (!directory || !directory[0] ||
        !relative_path_valid(filename) || !path || !path_size) {
        set_error(error, error_size, "invalid path arguments");
        return 0;
    }
    directory_length = strlen(directory);
    length = snprintf(
        path, path_size, "%s%s%s",
        directory,
        directory_length && !is_separator(directory[directory_length - 1u])
            ? "/" : "",
        filename);
    if (length < 0 || (size_t)length >= path_size) {
        set_error(error, error_size, too_long_message);
        return 0;
    }
    return 1;
}

static int executable_directory(
    char *directory, size_t directory_size,
    char *error, size_t error_size) {
    char *separator;
    char *forward_separator;

    if (!sr_executable_path(directory, directory_size)) {
        set_error(error, error_size, "cannot locate executable directory");
        return 0;
    }
    separator = strrchr(directory, '\\');
    forward_separator = strrchr(directory, '/');
    if (!separator || (forward_separator && forward_separator > separator))
        separator = forward_separator;
    if (!separator) {
        set_error(error, error_size, "executable path has no directory");
        return 0;
    }
    *separator = 0;
    return 1;
}

static int ensure_directory(
    const char *directory, char *error, size_t error_size) {
    char copy[SR_PATH_MAX];
    size_t length;

    if (!directory || !directory[0]) {
        set_error(error, error_size, "invalid user data directory");
        return 0;
    }
    length = strlen(directory);
    if (length >= sizeof(copy)) {
        set_error(error, error_size, "user data directory is too long");
        return 0;
    }
    memcpy(copy, directory, length + 1u);
    while (length > 1u && is_separator(copy[length - 1u]))
        copy[--length] = 0;

    for (char *cursor = copy + 1; *cursor; cursor++) {
        char separator;
        if (!is_separator(*cursor)) continue;
#ifdef _WIN32
        if (cursor == copy + 2 && copy[1] == ':') continue;
#endif
        separator = *cursor;
        *cursor = 0;
        if (copy[0] && _mkdir(copy) != 0 && errno != EEXIST) {
            char message[SR_PATH_MAX + 64];
            snprintf(
                message, sizeof(message),
                "cannot create user data directory %s: %s",
                copy, strerror(errno));
            *cursor = separator;
            set_error(error, error_size, message);
            return 0;
        }
        *cursor = separator;
    }
    if (_mkdir(copy) != 0 && errno != EEXIST) {
        char message[SR_PATH_MAX + 64];
        snprintf(
            message, sizeof(message),
            "cannot create user data directory %s: %s",
            copy, strerror(errno));
        set_error(error, error_size, message);
        return 0;
    }
    return 1;
}

static int ensure_parent_directory(
    const char *path, char *error, size_t error_size) {
    char directory[SR_PATH_MAX];
    char *separator;
    char *forward_separator;
    size_t length;

    length = strlen(path);
    if (length >= sizeof(directory)) {
        set_error(error, error_size, "user data path is too long");
        return 0;
    }
    memcpy(directory, path, length + 1u);
    separator = strrchr(directory, '\\');
    forward_separator = strrchr(directory, '/');
    if (!separator || (forward_separator && forward_separator > separator))
        separator = forward_separator;
    if (!separator) {
        set_error(error, error_size, "user data path has no directory");
        return 0;
    }
    *separator = 0;
    return ensure_directory(directory, error, error_size);
}

int sr_data_root(
    char *path, size_t path_size, char *error, size_t error_size) {
    if (error && error_size) error[0] = 0;
    if (!path || !path_size) {
        set_error(error, error_size, "invalid user data path arguments");
        return 0;
    }
    if (sr_data_dir()) {
        if (strlen(sr_data_dir()) >= path_size) {
            set_error(error, error_size, "SR_DATA_DIR is too long");
            return 0;
        }
        strcpy(path, sr_data_dir());
    } else if (!sr_user_data_path(SR_APPLICATION_DATA_NAME, path, path_size)) {
        set_error(error, error_size, "cannot locate user data directory");
        return 0;
    }
    return ensure_directory(path, error, error_size);
}

const char *sr_data_dir(void) {
    const char *value = getenv("SR_DATA_DIR");
    return value && value[0] ? value : NULL;
}

int sr_make_parent_directories(
    const char *path, char *error, size_t error_size) {
    if (error && error_size) error[0] = 0;
    if (!path || !path[0]) {
        set_error(error, error_size, "invalid path arguments");
        return 0;
    }
    return ensure_parent_directory(path, error, error_size);
}

int sr_data_path(
    const char *filename, char *path, size_t path_size,
    char *error, size_t error_size) {
    char directory[SR_PATH_MAX];

    if (error && error_size) error[0] = 0;
    if (!sr_data_root(
            directory, sizeof(directory), error, error_size) ||
        !path_join(
            directory, filename, path, path_size,
            error, error_size, "user data path is too long")) {
        return 0;
    }
    return ensure_parent_directory(path, error, error_size);
}

int sr_resource_path(
    const char *filename, char *path, size_t path_size,
    char *error, size_t error_size) {
    char directory[SR_PATH_MAX];
    const char *override;

    if (error && error_size) error[0] = 0;
    override = getenv("SR_RESOURCE_DIR");
    if (override && override[0]) {
        if (strlen(override) >= sizeof(directory)) {
            set_error(error, error_size, "SR_RESOURCE_DIR is too long");
            return 0;
        }
        strcpy(directory, override);
    } else {
        if (!executable_directory(
                directory, sizeof(directory), error, error_size)) {
            return 0;
        }
#ifdef __APPLE__
        {
            static const char suffix[] = "/Contents/MacOS";
            size_t length = strlen(directory);
            size_t suffix_length = sizeof(suffix) - 1u;
            if (length >= suffix_length &&
                strcmp(directory + length - suffix_length, suffix) == 0) {
                size_t prefix = length - sizeof("MacOS") + 1u;
                int written;
                directory[prefix] = 0;
                written = snprintf(
                    directory + prefix,
                    sizeof(directory) - prefix,
                    "Resources");
                if (written < 0 ||
                    (size_t)written >= sizeof(directory) - prefix) {
                    set_error(
                        error, error_size,
                        "application resource directory is too long");
                    return 0;
                }
            }
        }
#endif
    }
    return path_join(
        directory, filename, path, path_size,
        error, error_size, "resource path is too long");
}

#ifdef __APPLE__
static int migrate_legacy_setting(
    const char *filename, const char *destination,
    char *error, size_t error_size) {
    char directory[SR_PATH_MAX];
    char source[SR_PATH_MAX];
    char temporary[SR_PATH_MAX + 8];
    unsigned char buffer[65536];
    FILE *input = NULL;
    FILE *output = NULL;
    int ok = 0;

    input = fopen(destination, "rb");
    if (input) {
        fclose(input);
        return 1;
    }
    if (errno != ENOENT) {
        char message[SR_PATH_MAX + 64];
        snprintf(
            message, sizeof(message),
            "cannot inspect settings file %s: %s",
            destination, strerror(errno));
        set_error(error, error_size, message);
        return 0;
    }
    if (!executable_directory(
            directory, sizeof(directory), error, error_size) ||
        !path_join(
            directory, filename, source, sizeof(source),
            error, error_size, "legacy settings path is too long")) {
        return 0;
    }
    input = fopen(source, "rb");
    if (!input) {
        if (errno == ENOENT) return 1;
        {
            char message[SR_PATH_MAX + 64];
            snprintf(
                message, sizeof(message),
                "cannot read legacy settings file %s: %s",
                source, strerror(errno));
            set_error(error, error_size, message);
        }
        return 0;
    }
    {
        int length = snprintf(
            temporary, sizeof(temporary), "%s.migrate", destination);
        if (length < 0 || (size_t)length >= sizeof(temporary)) {
            set_error(error, error_size, "migration path is too long");
            goto done;
        }
    }
    output = fopen(temporary, "wb");
    if (!output) {
        char message[SR_PATH_MAX + 64];
        snprintf(
            message, sizeof(message),
            "cannot create migrated settings file %s: %s",
            temporary, strerror(errno));
        set_error(error, error_size, message);
        goto done;
    }
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), input);
        if (count && fwrite(buffer, 1, count, output) != count) {
            set_error(error, error_size, "cannot write migrated settings file");
            goto done;
        }
        if (count < sizeof(buffer)) {
            if (ferror(input)) {
                set_error(error, error_size, "cannot read legacy settings file");
                goto done;
            }
            break;
        }
    }
    if (!sr_file_commit(output)) {
        set_error(error, error_size, "cannot commit migrated settings file");
        goto done;
    }
    if (fclose(output) != 0) {
        output = NULL;
        set_error(error, error_size, "cannot close migrated settings file");
        goto done;
    }
    output = NULL;
    if (!sr_file_replace(temporary, destination)) {
        char message[SR_PATH_MAX + 64];
        snprintf(
            message, sizeof(message),
            "cannot publish migrated settings file %s: %s",
            destination, strerror(errno));
        set_error(error, error_size, message);
        goto done;
    }
    fprintf(
        stderr, "settings: migrated %s to %s\n",
        source, destination);
    ok = 1;

done:
    if (output) fclose(output);
    if (input) fclose(input);
    if (!ok) sr_file_remove(temporary);
    return ok;
}
#endif

int sr_settings_path(
    const char *filename, char *path, size_t path_size,
    char *error, size_t error_size) {
    char directory[SR_PATH_MAX];
    const char *override;
    size_t directory_length;

    if (error && error_size) error[0] = 0;
    if (!relative_path_valid(filename) || !path || !path_size) {
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
    } else if (sr_data_dir()) {
        /* The player's data folder (--data-dir) holds the settings files. */
        return sr_data_path(filename, path, path_size, error, error_size);
    } else {
#ifdef __APPLE__
        if (!sr_data_path(
                filename, path, path_size, error, error_size)) {
            return 0;
        }
        return migrate_legacy_setting(
            filename, path, error, error_size);
#else
        if (!executable_directory(
                directory, sizeof(directory), error, error_size)) {
            return 0;
        }
#endif
    }

    if (!path_join(
            directory, filename, path, path_size,
            error, error_size, "settings path is too long")) {
        return 0;
    }
    return ensure_parent_directory(path, error, error_size);
}

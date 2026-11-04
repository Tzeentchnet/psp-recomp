#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "startup_check.h"
#include "platform.h"
#include "settings_path.h"

#include "../../third_party/ppsspp_savedata/SHA1.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SR_GAME_TITLE
#define SR_GAME_TITLE "PSP Recomp"
#endif

#define SR_LAUNCHER_CONFIG_HEADER "# PSP recomp launcher config v1"
#define SR_LAUNCHER_ISO_PREFIX "iso_path = "

static void set_error(
    char *error, size_t error_size, const char *message) {
    if (error && error_size)
        snprintf(error, error_size, "%s", message);
}

static int copy_path(
    const char *source, char *path, size_t path_size,
    char *error, size_t error_size) {
    size_t length;
    if (!source || !source[0] || !path || !path_size) {
        set_error(error, error_size, "invalid ISO path");
        return 0;
    }
    length = strlen(source);
    if (length >= path_size) {
        set_error(error, error_size, "ISO path is too long");
        return 0;
    }
    memcpy(path, source, length + 1u);
    return 1;
}

static void trim_line_ending(char *line) {
    size_t length = strlen(line);
    while (length && (line[length - 1u] == '\n' ||
                      line[length - 1u] == '\r')) {
        line[--length] = 0;
    }
}

int sr_startup_resolve_iso_path(
    char *path, size_t path_size, char *error, size_t error_size) {
    const char *override;
    char config_path[SR_PATH_MAX];
    char line[SR_PATH_MAX + 32];
    char path_error[256];
    FILE *file;
    size_t prefix_length = sizeof(SR_LAUNCHER_ISO_PREFIX) - 1u;

    if (error && error_size) error[0] = 0;
    override = getenv("PSP_ISO");
    if (override && override[0])
        return copy_path(
            override, path, path_size, error, error_size);

    if (!sr_settings_path(
            "launcher.ini", config_path, sizeof(config_path),
            path_error, sizeof(path_error))) {
        set_error(error, error_size, path_error);
        return 0;
    }
    file = fopen(config_path, "rb");
    if (!file) {
        char message[SR_PATH_MAX + 96];
        if (errno == ENOENT) {
            snprintf(
                message, sizeof(message),
                "no game ISO has been selected; set PSP_ISO or choose "
                "the supported disc image");
        } else {
            snprintf(
                message, sizeof(message),
                "cannot open launcher config %s: %s",
                config_path, strerror(errno));
        }
        set_error(error, error_size, message);
        return 0;
    }
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        set_error(error, error_size, "launcher config is empty");
        return 0;
    }
    trim_line_ending(line);
    if (strcmp(line, SR_LAUNCHER_CONFIG_HEADER) != 0) {
        fclose(file);
        set_error(error, error_size, "launcher config header is invalid");
        return 0;
    }
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        set_error(error, error_size, "launcher config has no ISO path");
        return 0;
    }
    if (!strchr(line, '\n') && !feof(file)) {
        fclose(file);
        set_error(error, error_size, "launcher ISO path is too long");
        return 0;
    }
    trim_line_ending(line);
    if (strncmp(line, SR_LAUNCHER_ISO_PREFIX, prefix_length) != 0 ||
        !line[prefix_length]) {
        fclose(file);
        set_error(error, error_size, "launcher ISO path is invalid");
        return 0;
    }
    while (fgets(config_path, sizeof(config_path), file)) {
        trim_line_ending(config_path);
        if (config_path[0]) {
            fclose(file);
            set_error(
                error, error_size,
                "launcher config contains unexpected data");
            return 0;
        }
    }
    if (ferror(file)) {
        fclose(file);
        set_error(error, error_size, "cannot read launcher config");
        return 0;
    }
    if (fclose(file) != 0) {
        set_error(error, error_size, "cannot close launcher config");
        return 0;
    }
    return copy_path(
        line + prefix_length, path, path_size, error, error_size);
}

#ifdef SR_EXPECTED_ISO_SIZE
#ifndef SR_EXPECTED_ISO_SHA1
#error SR_EXPECTED_ISO_SHA1 must accompany SR_EXPECTED_ISO_SIZE
#endif

static int hash_file_sha1(
    FILE *file, uint8_t output[20], char *error, size_t error_size) {
    SHA_CTX hash;
    uint8_t *buffer = (uint8_t *)malloc(1u << 20);
    int ok = 0;

    if (!buffer) {
        snprintf(error, error_size, "out of memory while hashing");
        goto done;
    }
    rewind(file);
    SHAInit(&hash);
    for (;;) {
        size_t bytes = fread(buffer, 1, 1u << 20, file);
        if (bytes) SHAUpdate(&hash, buffer, (int)bytes);
        if (bytes < (1u << 20) && ferror(file)) {
            snprintf(error, error_size, "read failed");
            goto done;
        }
        if (!bytes) break;
    }
    SHAFinal(output, &hash);
    ok = 1;

done:
    free(buffer);
    return ok;
}
#endif

int sr_startup_validate_iso_path(
    const char *path, char *error, size_t error_size) {
    FILE *file;

    if (error && error_size) error[0] = 0;
    if (!path || !path[0]) {
        set_error(error, error_size, "ISO path is empty");
        return 0;
    }

    file = fopen(path, "rb");
    if (!file) {
        char message[SR_PATH_MAX + 64];
        snprintf(
            message, sizeof(message),
            "cannot open ISO \"%s\": %s", path, strerror(errno));
        set_error(error, error_size, message);
        return 0;
    }

#ifdef SR_EXPECTED_ISO_SIZE
    if (fseek(file, 0, SEEK_END) != 0) {
        set_error(error, error_size, "cannot read ISO size");
        fclose(file);
        return 0;
    }
    long size = ftell(file);
    if (size < 0 || (uint64_t)size != (uint64_t)SR_EXPECTED_ISO_SIZE) {
        char message[256];
        snprintf(
            message, sizeof(message),
            "unsupported ISO size %llu bytes; expected %llu",
            size < 0 ? 0ull : (unsigned long long)size,
            (unsigned long long)SR_EXPECTED_ISO_SIZE);
        set_error(error, error_size, message);
        fclose(file);
        return 0;
    }

    uint8_t hash[20];
    char hash_error[256];
    if (!hash_file_sha1(
            file, hash, hash_error, sizeof(hash_error))) {
        char message[320];
        snprintf(
            message, sizeof(message),
            "cannot hash ISO: %s", hash_error);
        set_error(error, error_size, message);
        fclose(file);
        return 0;
    }
    if (fclose(file) != 0) {
        set_error(error, error_size, "cannot close ISO");
        return 0;
    }

    char actual[41];
    for (int i = 0; i < 20; i++)
        snprintf(actual + i * 2, sizeof(actual) - (size_t)i * 2, "%02x", hash[i]);
    actual[40] = 0;
    if (_stricmp(actual, SR_EXPECTED_ISO_SHA1) != 0) {
        char message[192];
        snprintf(
            message, sizeof(message),
            "unsupported ISO SHA-1 %s; expected %s",
            actual, SR_EXPECTED_ISO_SHA1);
        set_error(error, error_size, message);
        return 0;
    }
#else
    if (fclose(file) != 0) {
        set_error(error, error_size, "cannot close ISO");
        return 0;
    }
#endif
    return 1;
}

int sr_startup_save_iso_path(
    const char *path, char *error, size_t error_size) {
    char config_path[SR_PATH_MAX];
    char temporary[SR_PATH_MAX + 8];
    char path_error[256];
    FILE *file;
    int length;
    int ok;

    if (error && error_size) error[0] = 0;
    if (!path || !path[0] || strchr(path, '\n') || strchr(path, '\r')) {
        set_error(error, error_size, "ISO path cannot be persisted");
        return 0;
    }
    if (!sr_startup_validate_iso_path(path, error, error_size))
        return 0;
    if (!sr_settings_path(
            "launcher.ini", config_path, sizeof(config_path),
            path_error, sizeof(path_error))) {
        set_error(error, error_size, path_error);
        return 0;
    }
    length = snprintf(
        temporary, sizeof(temporary), "%s.tmp", config_path);
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        set_error(error, error_size, "temporary launcher path is too long");
        return 0;
    }
    file = fopen(temporary, "wb");
    if (!file) {
        char message[SR_PATH_MAX + 64];
        snprintf(
            message, sizeof(message),
            "cannot create launcher config %s: %s",
            temporary, strerror(errno));
        set_error(error, error_size, message);
        return 0;
    }
    ok = fprintf(
            file, "%s\n%s%s\n",
            SR_LAUNCHER_CONFIG_HEADER,
            SR_LAUNCHER_ISO_PREFIX,
            path) > 0 &&
         sr_file_commit(file);
    if (fclose(file) != 0) ok = 0;
    if (!ok) {
        sr_file_remove(temporary);
        set_error(error, error_size, "cannot write launcher config");
        return 0;
    }
    if (!sr_file_replace(temporary, config_path)) {
        char message[SR_PATH_MAX + 64];
        snprintf(
            message, sizeof(message),
            "cannot publish launcher config %s: %s",
            config_path, strerror(errno));
        sr_file_remove(temporary);
        set_error(error, error_size, message);
        return 0;
    }
    return 1;
}

int sr_startup_validate(void) {
#ifndef SR_EXPECTED_ISO_SIZE
    return 1;
#else
    char path[SR_PATH_MAX];
    char error[512];
    if (!sr_startup_resolve_iso_path(
            path, sizeof(path), error, sizeof(error)) ||
        !sr_startup_validate_iso_path(
            path, error, sizeof(error))) {
        fprintf(
            stderr, "%s startup: %s.\n",
            SR_GAME_TITLE, error);
        return 0;
    }
    fprintf(
        stderr, "%s startup: verified ISO %s.\n",
        SR_GAME_TITLE, path);
    return 1;
#endif
}

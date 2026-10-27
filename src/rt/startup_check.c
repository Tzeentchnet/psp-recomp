#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "startup_check.h"
#include "platform.h"

#include "../../third_party/ppsspp_savedata/SHA1.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SR_GAME_TITLE
#define SR_GAME_TITLE "PSP Recomp"
#endif

#ifndef SR_EXPECTED_ISO_SIZE

int sr_startup_validate(void) {
    return 1;
}

#else

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

int sr_startup_validate(void) {
    const char *path = getenv("PSP_ISO");
    if (!path || !path[0]) {
        fprintf(
            stderr,
            "%s startup: PSP_ISO is not set. Point it at your supported "
            "disc image.\n",
            SR_GAME_TITLE);
        return 0;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(
            stderr, "%s startup: cannot open PSP_ISO \"%s\".\n",
            SR_GAME_TITLE, path);
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(
            stderr, "%s startup: cannot read PSP_ISO size.\n",
            SR_GAME_TITLE);
        fclose(file);
        return 0;
    }
    long size = ftell(file);
    if (size < 0 || (uint64_t)size != (uint64_t)SR_EXPECTED_ISO_SIZE) {
        fprintf(
            stderr,
            "%s startup: unsupported ISO size %llu bytes; expected %llu.\n",
            SR_GAME_TITLE,
            size < 0 ? 0ull : (unsigned long long)size,
            (unsigned long long)SR_EXPECTED_ISO_SIZE);
        fclose(file);
        return 0;
    }

    uint8_t hash[20];
    char error[256];
    if (!hash_file_sha1(file, hash, error, sizeof(error))) {
        fprintf(
            stderr, "%s startup: cannot hash PSP_ISO: %s.\n",
            SR_GAME_TITLE, error);
        fclose(file);
        return 0;
    }
    fclose(file);

    char actual[41];
    for (int i = 0; i < 20; i++)
        snprintf(actual + i * 2, sizeof(actual) - (size_t)i * 2, "%02x", hash[i]);
    actual[40] = 0;
    if (_stricmp(actual, SR_EXPECTED_ISO_SHA1) != 0) {
        fprintf(
            stderr,
            "%s startup: unsupported ISO SHA-1 %s; expected %s.\n",
            SR_GAME_TITLE, actual, SR_EXPECTED_ISO_SHA1);
        return 0;
    }
    fprintf(
        stderr, "%s startup: verified PSP_ISO (%s).\n",
        SR_GAME_TITLE, actual);
    return 1;
}

#endif

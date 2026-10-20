#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "startup_check.h"

#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SR_EXPECTED_ISO_SIZE

int sr_startup_validate(void) {
    return 1;
}

#else

#ifndef SR_EXPECTED_ISO_SHA1
#error SR_EXPECTED_ISO_SHA1 must accompany SR_EXPECTED_ISO_SIZE
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)
#endif

static int hash_file_sha1(
    HANDLE file, uint8_t output[20], char *error, size_t error_size) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR object = NULL;
    uint8_t *buffer = NULL;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD result_size = 0;
    int ok = 0;

    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA1_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(status)) {
        snprintf(error, error_size, "BCryptOpenAlgorithmProvider failed (0x%08lx)",
                 (unsigned long)status);
        goto done;
    }
    status = BCryptGetProperty(
        algorithm, BCRYPT_OBJECT_LENGTH,
        (PUCHAR)&object_size, sizeof(object_size), &result_size, 0);
    if (!NT_SUCCESS(status) || result_size != sizeof(object_size)) {
        snprintf(error, error_size, "cannot query SHA-1 object size (0x%08lx)",
                 (unsigned long)status);
        goto done;
    }
    status = BCryptGetProperty(
        algorithm, BCRYPT_HASH_LENGTH,
        (PUCHAR)&hash_size, sizeof(hash_size), &result_size, 0);
    if (!NT_SUCCESS(status) || hash_size != 20) {
        snprintf(error, error_size, "unexpected SHA-1 length (0x%08lx)",
                 (unsigned long)status);
        goto done;
    }
    object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, object_size);
    buffer = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, 1u << 20);
    if (!object || !buffer) {
        snprintf(error, error_size, "out of memory while hashing");
        goto done;
    }
    status = BCryptCreateHash(
        algorithm, &hash, object, object_size, NULL, 0, 0);
    if (!NT_SUCCESS(status)) {
        snprintf(error, error_size, "BCryptCreateHash failed (0x%08lx)",
                 (unsigned long)status);
        goto done;
    }

    for (;;) {
        DWORD bytes = 0;
        if (!ReadFile(file, buffer, 1u << 20, &bytes, NULL)) {
            snprintf(error, error_size, "read failed (error %lu)",
                     (unsigned long)GetLastError());
            goto done;
        }
        if (!bytes) break;
        status = BCryptHashData(hash, buffer, bytes, 0);
        if (!NT_SUCCESS(status)) {
            snprintf(error, error_size, "BCryptHashData failed (0x%08lx)",
                     (unsigned long)status);
            goto done;
        }
    }
    status = BCryptFinishHash(hash, output, 20, 0);
    if (!NT_SUCCESS(status)) {
        snprintf(error, error_size, "BCryptFinishHash failed (0x%08lx)",
                 (unsigned long)status);
        goto done;
    }
    ok = 1;

done:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
    if (object) HeapFree(GetProcessHeap(), 0, object);
    return ok;
}

int sr_startup_validate(void) {
    const char *path = getenv("PSP_ISO");
    if (!path || !path[0]) {
        fprintf(
            stderr,
            "KZL startup: PSP_ISO is not set. Point it at your own supported "
            "Killzone: Liberation v1.20 US disc image.\n");
        return 0;
    }

    HANDLE file = CreateFileA(
        path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        fprintf(
            stderr, "KZL startup: cannot open PSP_ISO \"%s\" (error %lu).\n",
            path, (unsigned long)GetLastError());
        return 0;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) {
        fprintf(
            stderr, "KZL startup: cannot read PSP_ISO size (error %lu).\n",
            (unsigned long)GetLastError());
        CloseHandle(file);
        return 0;
    }
    if ((uint64_t)size.QuadPart != (uint64_t)SR_EXPECTED_ISO_SIZE) {
        fprintf(
            stderr,
            "KZL startup: unsupported ISO size %llu bytes; expected %llu "
            "for Killzone: Liberation v1.20 US.\n",
            (unsigned long long)size.QuadPart,
            (unsigned long long)SR_EXPECTED_ISO_SIZE);
        CloseHandle(file);
        return 0;
    }

    uint8_t hash[20];
    char error[256];
    if (!hash_file_sha1(file, hash, error, sizeof(error))) {
        fprintf(stderr, "KZL startup: cannot hash PSP_ISO: %s.\n", error);
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);

    char actual[41];
    for (int i = 0; i < 20; i++)
        snprintf(actual + i * 2, sizeof(actual) - (size_t)i * 2, "%02x", hash[i]);
    actual[40] = 0;
    if (_stricmp(actual, SR_EXPECTED_ISO_SHA1) != 0) {
        fprintf(
            stderr,
            "KZL startup: unsupported ISO SHA-1 %s; expected %s "
            "(Killzone: Liberation v1.20 US).\n",
            actual, SR_EXPECTED_ISO_SHA1);
        return 0;
    }
    fprintf(stderr, "KZL startup: verified PSP_ISO (%s).\n", actual);
    return 1;
}

#endif

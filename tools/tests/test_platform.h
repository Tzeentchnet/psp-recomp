#ifndef PSP_RECOMP_TEST_PLATFORM_H
#define PSP_RECOMP_TEST_PLATFORM_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define SR_TEST_PATH_MAX MAX_PATH
#define SR_TEST_PATH_SEPARATOR "\\"

static inline unsigned long sr_test_process_id(void) {
    return (unsigned long)GetCurrentProcessId();
}

static inline int sr_test_set_env(const char *name, const char *value) {
    return _putenv_s(name, value ? value : "") == 0;
}

static inline void sr_test_sleep_ms(unsigned milliseconds) {
    Sleep(milliseconds);
}

static inline int sr_test_mkdir(const char *path) {
    return CreateDirectoryA(path, NULL) ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

static inline int sr_test_rmdir(const char *path) {
    return RemoveDirectoryA(path) != 0;
}

static inline int sr_test_temp_directory(char *path, size_t path_size) {
    DWORD length = GetTempPathA((DWORD)path_size, path);
    return length > 0 && length < path_size;
}

#else

#include <sys/stat.h>
#include <unistd.h>

#define SR_TEST_PATH_MAX 1024
#define SR_TEST_PATH_SEPARATOR "/"

static inline unsigned long sr_test_process_id(void) {
    return (unsigned long)getpid();
}

static inline int sr_test_set_env(const char *name, const char *value) {
    if (value && value[0]) return setenv(name, value, 1) == 0;
    return unsetenv(name) == 0;
}

static inline void sr_test_sleep_ms(unsigned milliseconds) {
    struct timespec request = {
        (time_t)(milliseconds / 1000u),
        (long)(milliseconds % 1000u) * 1000000l,
    };
    while (nanosleep(&request, &request) != 0 && errno == EINTR) {
    }
}

static inline int sr_test_mkdir(const char *path) {
    return mkdir(path, 0777) == 0 || errno == EEXIST;
}

static inline int sr_test_rmdir(const char *path) {
    return rmdir(path) == 0;
}

static inline int sr_test_temp_directory(char *path, size_t path_size) {
    const char *temp = getenv("TMPDIR");
    int result;
    if (!temp || !temp[0]) temp = "/tmp";
    result = snprintf(path, path_size, "%s", temp);
    return result >= 0 && (size_t)result < path_size;
}

#endif

static inline int sr_test_temp_path(
    char *path, size_t path_size, const char *stem, const char *suffix) {
    char directory[SR_TEST_PATH_MAX];
    size_t length;
    const char *separator;
    int result;

    if (!sr_test_temp_directory(directory, sizeof(directory))) return 0;
    length = strlen(directory);
    separator =
        length && (directory[length - 1] == '/' || directory[length - 1] == '\\')
            ? ""
            : SR_TEST_PATH_SEPARATOR;
    result = snprintf(
        path, path_size, "%s%s%s-%lu%s",
        directory, separator, stem, sr_test_process_id(), suffix);
    return result >= 0 && (size_t)result < path_size;
}

#endif

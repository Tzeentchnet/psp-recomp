#ifndef PSP_RECOMP_PLATFORM_H
#define PSP_RECOMP_PLATFORM_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define SR_PATH_MAX 1024

typedef struct SrFileVersion {
    int exists;
    uint64_t size;
    uint64_t modified_ns;
} SrFileVersion;

uint64_t sr_time_now(void);
uint64_t sr_time_frequency(void);
void sr_sleep_ms(uint32_t milliseconds);

int sr_executable_path(char *path, size_t path_size);
int sr_user_data_path(
    const char *application, char *path, size_t path_size);
int sr_file_version(const char *path, SrFileVersion *version);
int sr_file_version_equal(
    const SrFileVersion *left, const SrFileVersion *right);
int sr_file_commit(FILE *file);
int sr_file_replace(const char *source, const char *destination);
int sr_file_remove(const char *path);

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>

#else

#include <stddef.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct LARGE_INTEGER {
    int64_t QuadPart;
} LARGE_INTEGER;
typedef int64_t LONGLONG;
typedef uint32_t DWORD;
typedef size_t SIZE_T;

#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef MAX_PATH
#define MAX_PATH SR_PATH_MAX
#endif
#ifndef _A_SUBDIR
#define _A_SUBDIR 0x10
#endif
#ifndef _S_IFDIR
#define _S_IFDIR S_IFDIR
#endif

struct _finddata_t {
    unsigned attrib;
    int64_t size;
    time_t time_create;
    time_t time_access;
    time_t time_write;
    char name[256];
};

intptr_t sr_findfirst(const char *pattern, struct _finddata_t *data);
int sr_findnext(intptr_t handle, struct _finddata_t *data);
int sr_findclose(intptr_t handle);

#define _findfirst sr_findfirst
#define _findnext sr_findnext
#define _findclose sr_findclose
#define _stat64 stat
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _unlink unlink
#define _rmdir rmdir
#define _mkdir(path) mkdir((path), 0777)

static inline int QueryPerformanceFrequency(LARGE_INTEGER *value) {
    if (!value) return 0;
    value->QuadPart = (int64_t)sr_time_frequency();
    return 1;
}

static inline int QueryPerformanceCounter(LARGE_INTEGER *value) {
    if (!value) return 0;
    value->QuadPart = (int64_t)sr_time_now();
    return 1;
}

static inline void Sleep(DWORD milliseconds) {
    sr_sleep_ms(milliseconds);
}

static inline int timeBeginPeriod(unsigned period) {
    (void)period;
    return 0;
}

typedef void (*SrFiberProc)(void *);
void *sr_fiber_convert_thread(void);
void *sr_fiber_create(size_t stack_size, SrFiberProc proc, void *parameter);
void sr_fiber_switch(void *fiber);
void sr_fiber_destroy(void *fiber);

#define ConvertThreadToFiber(parameter) sr_fiber_convert_thread()
#define CreateFiberEx(commit, reserve, flags, proc, parameter) \
    sr_fiber_create((reserve), (proc), (parameter))
#define SwitchToFiber(fiber) sr_fiber_switch((fiber))
#define DeleteFiber(fiber) sr_fiber_destroy((fiber))

#endif

#endif

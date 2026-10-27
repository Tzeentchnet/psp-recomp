#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif

#include "platform.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

uint64_t sr_time_now(void) {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
}

uint64_t sr_time_frequency(void) {
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    return (uint64_t)value.QuadPart;
}

void sr_sleep_ms(uint32_t milliseconds) {
    Sleep(milliseconds);
}

int sr_executable_path(char *path, size_t path_size) {
    if (!path || !path_size || path_size > MAXDWORD) return 0;
    DWORD length = GetModuleFileNameA(NULL, path, (DWORD)path_size);
    return length > 0 && length < path_size;
}

int sr_user_data_path(
    const char *application, char *path, size_t path_size) {
    const char *root = getenv("LOCALAPPDATA");
    if (!root || !root[0] || !application || !application[0])
        return 0;
    int length = snprintf(
        path, path_size, "%s/%s", root, application);
    return length >= 0 && (size_t)length < path_size;
}

int sr_file_version(const char *path, SrFileVersion *version) {
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!version) return 0;
    memset(version, 0, sizeof(*version));
    if (!GetFileAttributesExA(
            path, GetFileExInfoStandard, &attributes)) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ||
               GetLastError() == ERROR_PATH_NOT_FOUND;
    }
    ULARGE_INTEGER modified;
    modified.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    modified.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
    version->exists = 1;
    version->size =
        ((uint64_t)attributes.nFileSizeHigh << 32) |
        attributes.nFileSizeLow;
    version->modified_ns = modified.QuadPart * 100u;
    return 1;
}

int sr_file_commit(FILE *file) {
    return file && fflush(file) == 0 && _commit(_fileno(file)) == 0;
}

int sr_file_replace(const char *source, const char *destination) {
    return MoveFileExA(
        source, destination,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

int sr_file_remove(const char *path) {
    return DeleteFileA(path) != 0;
}

#else

#include <glob.h>
#include <limits.h>
#include <sys/types.h>
#include <ucontext.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#endif

typedef struct SrFiber {
    ucontext_t context;
    void *stack;
    size_t stack_size;
    SrFiberProc proc;
    void *parameter;
} SrFiber;

typedef struct SrFindHandle {
    glob_t paths;
    size_t index;
} SrFindHandle;

static SrFiber *s_current_fiber;

uint64_t sr_time_now(void) {
#ifdef __APPLE__
    static mach_timebase_info_data_t timebase;
    if (!timebase.denom) mach_timebase_info(&timebase);
    __uint128_t scaled =
        (__uint128_t)mach_continuous_time() * timebase.numer;
    return (uint64_t)(scaled / timebase.denom);
#else
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (uint64_t)value.tv_sec * 1000000000ull +
           (uint64_t)value.tv_nsec;
#endif
}

uint64_t sr_time_frequency(void) {
    return 1000000000ull;
}

void sr_sleep_ms(uint32_t milliseconds) {
    struct timespec request = {
        (time_t)(milliseconds / 1000u),
        (long)(milliseconds % 1000u) * 1000000l,
    };
    while (nanosleep(&request, &request) != 0 && errno == EINTR) {
    }
}

int sr_executable_path(char *path, size_t path_size) {
    if (!path || !path_size) return 0;
#ifdef __APPLE__
    uint32_t size = (uint32_t)path_size;
    if (_NSGetExecutablePath(path, &size) != 0) return 0;
    char resolved[SR_PATH_MAX];
    if (realpath(path, resolved)) {
        size_t length = strlen(resolved);
        if (length >= path_size) return 0;
        memcpy(path, resolved, length + 1u);
    }
    return 1;
#else
    ssize_t length = readlink("/proc/self/exe", path, path_size - 1u);
    if (length <= 0 || (size_t)length >= path_size) return 0;
    path[length] = 0;
    return 1;
#endif
}

int sr_user_data_path(
    const char *application, char *path, size_t path_size) {
    const char *home = getenv("HOME");
    if (!home || !home[0] || !application || !application[0])
        return 0;
#ifdef __APPLE__
    int length = snprintf(
        path, path_size, "%s/Library/Application Support/%s",
        home, application);
#else
    const char *root = getenv("XDG_DATA_HOME");
    int length = root && root[0]
        ? snprintf(path, path_size, "%s/%s", root, application)
        : snprintf(path, path_size, "%s/.local/share/%s", home, application);
#endif
    return length >= 0 && (size_t)length < path_size;
}

int sr_file_version(const char *path, SrFileVersion *version) {
    struct stat status;
    if (!version) return 0;
    memset(version, 0, sizeof(*version));
    if (stat(path, &status) != 0)
        return errno == ENOENT || errno == ENOTDIR;
    version->exists = 1;
    version->size = (uint64_t)status.st_size;
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
    version->modified_ns =
        (uint64_t)status.st_mtimespec.tv_sec * 1000000000ull +
        (uint64_t)status.st_mtimespec.tv_nsec;
#elif !defined(__APPLE__)
    version->modified_ns =
        (uint64_t)status.st_mtim.tv_sec * 1000000000ull +
        (uint64_t)status.st_mtim.tv_nsec;
#else
    version->modified_ns = (uint64_t)status.st_mtime * 1000000000ull;
#endif
    return 1;
}

int sr_file_commit(FILE *file) {
    return file && fflush(file) == 0 && fsync(fileno(file)) == 0;
}

int sr_file_replace(const char *source, const char *destination) {
    return rename(source, destination) == 0;
}

int sr_file_remove(const char *path) {
    return unlink(path) == 0;
}

static int find_fill(
    const SrFindHandle *handle, struct _finddata_t *data) {
    const char *path = handle->paths.gl_pathv[handle->index];
    const char *name = strrchr(path, '/');
    struct stat status;
    if (stat(path, &status) != 0) return -1;
    memset(data, 0, sizeof(*data));
    snprintf(data->name, sizeof(data->name), "%s", name ? name + 1 : path);
    data->attrib = S_ISDIR(status.st_mode) ? _A_SUBDIR : 0u;
    data->size = status.st_size;
#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
    data->time_create = status.st_birthtimespec.tv_sec;
#elif defined(__APPLE__)
    data->time_create = status.st_ctime;
#else
    data->time_create = status.st_ctim.tv_sec;
#endif
    data->time_access = status.st_atime;
    data->time_write = status.st_mtime;
    return 0;
}

intptr_t sr_findfirst(const char *pattern, struct _finddata_t *data) {
    char normalized[SR_PATH_MAX];
    size_t length = strlen(pattern);
    if (length >= sizeof(normalized)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    for (size_t i = 0; i <= length; i++)
        normalized[i] = pattern[i] == '\\' ? '/' : pattern[i];
    SrFindHandle *handle = (SrFindHandle *)calloc(1, sizeof(*handle));
    if (!handle) return -1;
    int result = glob(normalized, GLOB_NOSORT, NULL, &handle->paths);
    if (result != 0 || !handle->paths.gl_pathc) {
        globfree(&handle->paths);
        free(handle);
        errno = result == GLOB_NOSPACE ? ENOMEM : ENOENT;
        return -1;
    }
    if (find_fill(handle, data) != 0) {
        globfree(&handle->paths);
        free(handle);
        return -1;
    }
    return (intptr_t)handle;
}

int sr_findnext(intptr_t value, struct _finddata_t *data) {
    SrFindHandle *handle = (SrFindHandle *)value;
    if (!handle || ++handle->index >= handle->paths.gl_pathc) {
        errno = ENOENT;
        return -1;
    }
    return find_fill(handle, data);
}

int sr_findclose(intptr_t value) {
    SrFindHandle *handle = (SrFindHandle *)value;
    if (!handle) return -1;
    globfree(&handle->paths);
    free(handle);
    return 0;
}

static void fiber_entry(void) {
    SrFiber *fiber = s_current_fiber;
    fiber->proc(fiber->parameter);
    abort();
}

void *sr_fiber_convert_thread(void) {
    if (s_current_fiber) return s_current_fiber;
    SrFiber *fiber = (SrFiber *)calloc(1, sizeof(*fiber));
    if (!fiber || getcontext(&fiber->context) != 0) {
        free(fiber);
        return NULL;
    }
    s_current_fiber = fiber;
    return fiber;
}

void *sr_fiber_create(
    size_t stack_size, SrFiberProc proc, void *parameter) {
    SrFiber *fiber = (SrFiber *)calloc(1, sizeof(*fiber));
    if (!fiber) return NULL;
    fiber->stack = malloc(stack_size);
    if (!fiber->stack || getcontext(&fiber->context) != 0) {
        free(fiber->stack);
        free(fiber);
        return NULL;
    }
    fiber->stack_size = stack_size;
    fiber->proc = proc;
    fiber->parameter = parameter;
    fiber->context.uc_stack.ss_sp = fiber->stack;
    fiber->context.uc_stack.ss_size = stack_size;
    fiber->context.uc_link = NULL;
    makecontext(&fiber->context, fiber_entry, 0);
    return fiber;
}

void sr_fiber_switch(void *value) {
    SrFiber *target = (SrFiber *)value;
    SrFiber *source = s_current_fiber;
    if (!source || !target || source == target) return;
    s_current_fiber = target;
    if (swapcontext(&source->context, &target->context) != 0)
        abort();
}

void sr_fiber_destroy(void *value) {
    SrFiber *fiber = (SrFiber *)value;
    if (!fiber || fiber == s_current_fiber) return;
    free(fiber->stack);
    free(fiber);
}

#endif

int sr_file_version_equal(
    const SrFileVersion *left, const SrFileVersion *right) {
    return left && right &&
           left->exists == right->exists &&
           left->size == right->size &&
           left->modified_ns == right->modified_ns;
}

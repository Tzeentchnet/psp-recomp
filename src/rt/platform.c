#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
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
#if !defined(__APPLE__) || !defined(__aarch64__)
#include <ucontext.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#if defined(__aarch64__)
#include "fiber_arm64.h"
#include <sys/mman.h>
#endif
#endif

typedef struct SrFiber {
#if defined(__APPLE__) && defined(__aarch64__)
    SrArm64FiberContext context;
    void *stack_mapping;
    size_t stack_mapping_size;
#else
    ucontext_t context;
#endif
    void *stack;
    size_t stack_size;
    SrFiberProc proc;
    void *parameter;
    const void *owner;
    int is_thread;
} SrFiber;

typedef struct SrFindHandle {
    glob_t paths;
    size_t index;
} SrFindHandle;

static _Thread_local SrFiber *s_current_fiber;

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
    if (!fiber || !fiber->proc) {
        fprintf(stderr, "fiber: entered without a procedure\n");
        abort();
    }
    fiber->proc(fiber->parameter);
    fprintf(stderr, "fiber: procedure returned unexpectedly\n");
    abort();
}

void *sr_fiber_convert_thread(void) {
    if (s_current_fiber) return s_current_fiber;
    SrFiber *fiber = (SrFiber *)calloc(1, sizeof(*fiber));
#if defined(__APPLE__) && defined(__aarch64__)
    if (!fiber) return NULL;
#else
    if (!fiber || getcontext(&fiber->context) != 0) {
        free(fiber);
        return NULL;
    }
#endif
    fiber->owner = &s_current_fiber;
    fiber->is_thread = 1;
    s_current_fiber = fiber;
    return fiber;
}

#if defined(__APPLE__) && defined(__aarch64__)
static int fiber_allocate_stack(SrFiber *fiber, size_t stack_size) {
    long page_value = sysconf(_SC_PAGESIZE);
    if (page_value <= 0) {
        errno = EINVAL;
        return 0;
    }
    size_t page_size = (size_t)page_value;
    if (stack_size > SIZE_MAX - (page_size - 1u)) {
        errno = EOVERFLOW;
        return 0;
    }
    size_t usable_size =
        ((stack_size + page_size - 1u) / page_size) * page_size;
    if (usable_size > SIZE_MAX - 2u * page_size) {
        errno = EOVERFLOW;
        return 0;
    }
    size_t mapping_size = usable_size + 2u * page_size;
    void *mapping = mmap(
        NULL, mapping_size, PROT_NONE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) return 0;
    void *stack = (unsigned char *)mapping + page_size;
    if (mprotect(stack, usable_size, PROT_READ | PROT_WRITE) != 0) {
        int saved_errno = errno;
        munmap(mapping, mapping_size);
        errno = saved_errno;
        return 0;
    }
    fiber->stack_mapping = mapping;
    fiber->stack_mapping_size = mapping_size;
    fiber->stack = stack;
    fiber->stack_size = usable_size;
    return 1;
}

static uint64_t fiber_read_fpcr(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, fpcr" : "=r"(value));
    return value;
}

static uint64_t fiber_read_fpsr(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, fpsr" : "=r"(value));
    return value;
}
#endif

void *sr_fiber_create(
    size_t stack_size, SrFiberProc proc, void *parameter) {
    if (!s_current_fiber) {
        errno = EPERM;
        return NULL;
    }
    if (!proc || stack_size < 16u * 1024u) {
        errno = EINVAL;
        return NULL;
    }
    SrFiber *fiber = (SrFiber *)calloc(1, sizeof(*fiber));
    if (!fiber) return NULL;
#if defined(__APPLE__) && defined(__aarch64__)
    if (!fiber_allocate_stack(fiber, stack_size)) {
        free(fiber);
        return NULL;
    }
#else
    fiber->stack = malloc(stack_size);
    if (!fiber->stack || getcontext(&fiber->context) != 0) {
        free(fiber->stack);
        free(fiber);
        return NULL;
    }
    fiber->stack_size = stack_size;
#endif
    fiber->proc = proc;
    fiber->parameter = parameter;
    fiber->owner = &s_current_fiber;
#if defined(__APPLE__) && defined(__aarch64__)
    uintptr_t stack_top =
        (uintptr_t)fiber->stack + fiber->stack_size;
    stack_top &= ~(uintptr_t)15u;
    fiber->context.sp = (uint64_t)stack_top;
    fiber->context.x30 = (uint64_t)(uintptr_t)fiber_entry;
    fiber->context.fpcr = fiber_read_fpcr();
    fiber->context.fpsr = fiber_read_fpsr();
#else
    fiber->context.uc_stack.ss_sp = fiber->stack;
    fiber->context.uc_stack.ss_size = stack_size;
    fiber->context.uc_link = NULL;
    makecontext(&fiber->context, fiber_entry, 0);
#endif
    return fiber;
}

void sr_fiber_switch(void *value) {
    SrFiber *target = (SrFiber *)value;
    SrFiber *source = s_current_fiber;
    if (!source || !target) {
        fprintf(stderr, "fiber: switch requires valid source and target\n");
        abort();
    }
    if (source == target) return;
    if (source->owner != &s_current_fiber ||
        target->owner != &s_current_fiber) {
        fprintf(stderr, "fiber: cross-thread switch is not supported\n");
        abort();
    }
    s_current_fiber = target;
#if defined(__APPLE__) && defined(__aarch64__)
    sr_fiber_swap_context(&source->context, &target->context);
#else
    if (swapcontext(&source->context, &target->context) != 0)
        abort();
#endif
}

void sr_fiber_destroy(void *value) {
    SrFiber *fiber = (SrFiber *)value;
    if (!fiber) return;
    if (fiber == s_current_fiber || fiber->is_thread) {
        fprintf(stderr, "fiber: cannot destroy the active thread fiber\n");
        abort();
    }
    if (fiber->owner != &s_current_fiber) {
        fprintf(stderr, "fiber: cross-thread destruction is not supported\n");
        abort();
    }
#if defined(__APPLE__) && defined(__aarch64__)
    if (fiber->stack_mapping &&
        munmap(
            fiber->stack_mapping,
            fiber->stack_mapping_size) != 0) {
        fprintf(stderr, "fiber: failed to release stack: %s\n", strerror(errno));
        abort();
    }
#else
    free(fiber->stack);
#endif
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

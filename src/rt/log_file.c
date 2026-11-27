/* Log file.
 *
 * stderr is redirected into a pipe; a thread copies what arrives to the log file and to the
 * original console, so a crash report has the same text the console showed. stdout is left
 * alone because tools parse it.
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "log_file.h"
#include "platform.h"
#include "settings_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

static void keep_previous(const char *path) {
    char previous[SR_PATH_MAX];
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    const char *back = strrchr(path, '\\');
    size_t stem;
    if (back > slash) slash = back;
    stem = dot && dot > slash ? (size_t)(dot - path) : strlen(path);
    if (stem + sizeof(".previous.log") > sizeof(previous)) return;
    memcpy(previous, path, stem);
    strcpy(previous + stem, ".previous.log");
    sr_file_replace(path, previous);
}

const char *sr_log_default_path(const char *name) {
    static char path[SR_PATH_MAX];
    const char *setting = getenv("SR_LOG");
    const char *file = getenv("SR_LOG_FILE");
    char relative[128];
    char error[256];
    if (setting && strcmp(setting, "0") == 0) return NULL;
    if (file && file[0]) return file;
    if (!sr_data_dir()) return NULL;
    snprintf(relative, sizeof(relative), "logs/%s.log", name ? name : "game");
    return sr_data_path(relative, path, sizeof(path), error, sizeof(error)) ? path : NULL;
}

static void log_header(const char *path, const char *command_line) {
    time_t now = time(NULL);
    char stamp[64];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(stderr, "log: %s, %s\n", stamp, path);
    if (command_line) fprintf(stderr, "log: command line: %s\n", command_line);
}

#ifdef _WIN32

static HANDLE s_file = INVALID_HANDLE_VALUE;
static HANDLE s_console = INVALID_HANDLE_VALUE;
static HANDLE s_read = NULL;
static volatile LONG s_busy;

/* The pump peeks before it reads, so it is never blocked inside ReadFile: Windows serializes
 * synchronous I/O on a handle, and a blocked read would also block sr_log_flush's
 * PeekNamedPipe until more output arrived, hanging the process at exit. */
static DWORD WINAPI log_pump(LPVOID unused) {
    char buffer[4096];
    (void)unused;
    for (;;) {
        DWORD available = 0, count = 0, written;
        if (!PeekNamedPipe(s_read, NULL, 0, NULL, &available, NULL)) break;
        if (!available) {
            Sleep(5);
            continue;
        }
        if (available > sizeof(buffer)) available = sizeof(buffer);
        InterlockedExchange(&s_busy, 1);   /* before the read empties the pipe */
        if (!ReadFile(s_read, buffer, available, &count, NULL) || !count) {
            InterlockedExchange(&s_busy, 0);
            break;
        }
        WriteFile(s_file, buffer, count, &written, NULL);
        if (s_console != INVALID_HANDLE_VALUE)
            WriteFile(s_console, buffer, count, &written, NULL);
        InterlockedExchange(&s_busy, 0);
    }
    return 0;
}

int sr_log_start(const char *path) {
    HANDLE write_end = NULL;
    HANDLE thread;
    HANDLE console;
    int descriptor;
    char error[256];

    if (!path || !path[0] || s_file != INVALID_HANDLE_VALUE) return 0;
    if (!sr_make_parent_directories(path, error, sizeof(error)))
        fprintf(stderr, "log: %s\n", error);
    keep_previous(path);
    s_file = CreateFileA(
        path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (s_file == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "log: cannot create %s (error %lu)\n",
                path, (unsigned long)GetLastError());
        return 0;
    }

    fflush(stderr);
    console = (HANDLE)_get_osfhandle(_fileno(stderr));
    if (console != INVALID_HANDLE_VALUE && console != NULL &&
        !DuplicateHandle(GetCurrentProcess(), console, GetCurrentProcess(),
                         &s_console, 0, FALSE, DUPLICATE_SAME_ACCESS))
        s_console = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&s_read, &write_end, NULL, 0)) goto failed;
    descriptor = _open_osfhandle((intptr_t)write_end, _O_WRONLY | _O_TEXT);
    if (descriptor < 0) goto failed;
    write_end = NULL;   /* owned by descriptor now */
    if (_dup2(descriptor, _fileno(stderr)) != 0) {
        _close(descriptor);
        goto failed;
    }
    _close(descriptor);
    setvbuf(stderr, NULL, _IONBF, 0);
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));

    thread = CreateThread(NULL, 0, log_pump, NULL, 0, NULL);
    if (!thread) {
        fprintf(stderr, "log: cannot start the log thread\n");
        return 0;
    }
    CloseHandle(thread);
    atexit(sr_log_flush);
    log_header(path, GetCommandLineA());
    return 1;

failed:
    fprintf(stderr, "log: cannot redirect stderr to %s (error %lu)\n",
            path, (unsigned long)GetLastError());
    if (write_end) CloseHandle(write_end);
    if (s_read) CloseHandle(s_read);
    s_read = NULL;
    CloseHandle(s_file);
    s_file = INVALID_HANDLE_VALUE;
    return 0;
}

void sr_log_flush(void) {
    if (s_file == INVALID_HANDLE_VALUE) return;
    fflush(stderr);
    for (int waited = 0; waited < 1000; waited += 5) {
        DWORD pending = 0;
        if (!PeekNamedPipe(s_read, NULL, 0, NULL, &pending, NULL)) break;
        if (!pending && !InterlockedCompareExchange(&s_busy, 0, 0)) break;
        Sleep(5);
    }
    FlushFileBuffers(s_file);
}

#else

static int s_file = -1;
static int s_console = -1;
static int s_read = -1;
static volatile int s_busy;

static void write_all(int descriptor, const char *buffer, size_t count) {
    while (count) {
        ssize_t written = write(descriptor, buffer, count);
        if (written < 0) {
            if (errno == EINTR) continue;
            return;
        }
        buffer += written;
        count -= (size_t)written;
    }
}

static void *log_pump(void *unused) {
    char buffer[4096];
    (void)unused;
    for (;;) {
        struct pollfd ready = {s_read, POLLIN, 0};
        ssize_t count;
        if (poll(&ready, 1, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        __atomic_store_n(&s_busy, 1, __ATOMIC_SEQ_CST);   /* before the read empties the pipe */
        count = read(s_read, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            __atomic_store_n(&s_busy, 0, __ATOMIC_SEQ_CST);
            continue;
        }
        if (count <= 0) {
            __atomic_store_n(&s_busy, 0, __ATOMIC_SEQ_CST);
            break;
        }
        write_all(s_file, buffer, (size_t)count);
        if (s_console >= 0) write_all(s_console, buffer, (size_t)count);
        __atomic_store_n(&s_busy, 0, __ATOMIC_SEQ_CST);
    }
    return NULL;
}

int sr_log_start(const char *path) {
    int pipe_ends[2] = {-1, -1};
    pthread_t thread;
    char error[256];

    if (!path || !path[0] || s_file >= 0) return 0;
    if (!sr_make_parent_directories(path, error, sizeof(error)))
        fprintf(stderr, "log: %s\n", error);
    keep_previous(path);
    s_file = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (s_file < 0) {
        fprintf(stderr, "log: cannot create %s: %s\n", path, strerror(errno));
        return 0;
    }

    fflush(stderr);
    s_console = dup(STDERR_FILENO);
    if (pipe(pipe_ends) != 0) goto failed;
    if (dup2(pipe_ends[1], STDERR_FILENO) < 0) goto failed;
    close(pipe_ends[1]);
    pipe_ends[1] = -1;
    s_read = pipe_ends[0];
    setvbuf(stderr, NULL, _IONBF, 0);

    if (pthread_create(&thread, NULL, log_pump, NULL) != 0) {
        fprintf(stderr, "log: cannot start the log thread\n");
        return 0;
    }
    pthread_detach(thread);
    atexit(sr_log_flush);
    log_header(path, NULL);
    return 1;

failed:
    fprintf(stderr, "log: cannot redirect stderr to %s: %s\n", path, strerror(errno));
    if (pipe_ends[0] >= 0) close(pipe_ends[0]);
    if (pipe_ends[1] >= 0) close(pipe_ends[1]);
    if (s_console >= 0) close(s_console);
    s_console = -1;
    close(s_file);
    s_file = -1;
    return 0;
}

void sr_log_flush(void) {
    if (s_file < 0) return;
    fflush(stderr);
    for (int waited = 0; waited < 1000; waited += 5) {
        int pending = 0;
        if (ioctl(s_read, FIONREAD, &pending) != 0) break;
        if (!pending && !__atomic_load_n(&s_busy, __ATOMIC_SEQ_CST)) break;
        sr_sleep_ms(5);
    }
    fsync(s_file);
}

#endif

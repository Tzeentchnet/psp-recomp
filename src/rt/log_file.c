/* Log file.
 *
 * stderr is redirected into a pipe; a thread copies what arrives to the log file and to the
 * original console handle, so a crash report has the same text the console showed. stdout is
 * left alone because tools parse it.
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "log_file.h"
#include "settings_path.h"

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

static HANDLE s_file = INVALID_HANDLE_VALUE;
static HANDLE s_console = INVALID_HANDLE_VALUE;
static HANDLE s_read = NULL;
static volatile LONG s_busy;

static DWORD WINAPI log_pump(LPVOID unused) {
    char buffer[4096];
    (void)unused;
    for (;;) {
        DWORD count = 0, written;
        if (!ReadFile(s_read, buffer, sizeof(buffer), &count, NULL) || !count)
            break;
        InterlockedExchange(&s_busy, 1);
        WriteFile(s_file, buffer, count, &written, NULL);
        if (s_console != INVALID_HANDLE_VALUE)
            WriteFile(s_console, buffer, count, &written, NULL);
        InterlockedExchange(&s_busy, 0);
    }
    return 0;
}

static void keep_previous(const char *path) {
    char previous[MAX_PATH * 2];
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '\\');
    const char *forward = strrchr(path, '/');
    size_t stem;
    if (forward > slash) slash = forward;
    stem = dot && dot > slash ? (size_t)(dot - path) : strlen(path);
    if (stem + sizeof(".previous.log") > sizeof(previous)) return;
    memcpy(previous, path, stem);
    strcpy(previous + stem, ".previous.log");
    MoveFileExA(path, previous, MOVEFILE_REPLACE_EXISTING);
}

const char *sr_log_default_path(const char *name) {
    static char path[MAX_PATH * 2];
    const char *setting = getenv("SR_LOG");
    const char *file = getenv("SR_LOG_FILE");
    char relative[128];
    if (setting && strcmp(setting, "0") == 0) return NULL;
    if (file && file[0]) return file;
    snprintf(relative, sizeof(relative), "logs\\%s.log", name ? name : "game");
    return sr_data_path(relative, path, sizeof(path)) ? path : NULL;
}

int sr_log_start(const char *path) {
    char directory[MAX_PATH * 2];
    HANDLE write_end = NULL;
    HANDLE thread;
    HANDLE console;
    int descriptor;
    const char *slash;

    if (!path || !path[0] || s_file != INVALID_HANDLE_VALUE) return 0;
    slash = strrchr(path, '\\');
    if (!slash || (strrchr(path, '/') && strrchr(path, '/') > slash))
        slash = strrchr(path, '/');
    if (slash && (size_t)(slash - path) < sizeof(directory)) {
        memcpy(directory, path, (size_t)(slash - path));
        directory[slash - path] = 0;
        sr_make_directories(directory);
    }
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

    {
        time_t now = time(NULL);
        char stamp[64];
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(stderr, "log: %s, %s\nlog: command line: %s\n",
                stamp, path, GetCommandLineA());
    }
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
        if (!pending && !s_busy) break;
        Sleep(5);
    }
    FlushFileBuffers(s_file);
}

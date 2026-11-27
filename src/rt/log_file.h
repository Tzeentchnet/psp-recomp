/* Log file: a copy of everything the runtime prints to stderr (src/rt/log_file.c). */
#ifndef PSP_RECOMP_LOG_FILE_H
#define PSP_RECOMP_LOG_FILE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Starts copying stderr to path while still showing it on the console. A log left by the
 * previous run is kept as <name>.previous.log. Returns 1 on success. */
int sr_log_start(const char *path);

/* The log path chosen by --log, SR_LOG_FILE or the data folder (<data>/logs/<name>.log), or
 * NULL when nothing asks for a log or SR_LOG=0. */
const char *sr_log_default_path(const char *name);

/* Waits briefly until everything printed so far is in the log file. Safe to call from a crash
 * handler. */
void sr_log_flush(void);

#ifdef __cplusplus
}
#endif

#endif

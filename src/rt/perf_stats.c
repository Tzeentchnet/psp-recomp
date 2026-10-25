/* Exclusive runtime phase timing. The runtime executes on one host thread, including fibers. */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include "perf_stats.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef struct SrPerfState {
    LARGE_INTEGER frequency;
    LONGLONG last;
    uint64_t ticks[SR_PERF_CATEGORY_COUNT];
    uint64_t counters[SR_PERF_COUNTER_COUNT];
    SrPerfCategory current;
    int initialized;
    int enabled;
} SrPerfState;

static SrPerfState s_perf = {
    .current = SR_PERF_NATIVE,
};

static void perf_init(void) {
    LARGE_INTEGER now;
    if (s_perf.initialized) return;
    s_perf.initialized = 1;
    s_perf.enabled =
        getenv("SR_PERF_LOG") != NULL ||
        (getenv("SR_PERF_FILE") && getenv("SR_PERF_FILE")[0]);
    if (!s_perf.enabled) return;
    QueryPerformanceFrequency(&s_perf.frequency);
    QueryPerformanceCounter(&now);
    s_perf.last = now.QuadPart;
}

static void perf_record_now(void) {
    LARGE_INTEGER now;
    if (!s_perf.enabled) return;
    QueryPerformanceCounter(&now);
    if (now.QuadPart >= s_perf.last)
        s_perf.ticks[s_perf.current] +=
            (uint64_t)(now.QuadPart - s_perf.last);
    s_perf.last = now.QuadPart;
}

int sr_perf_enabled(void) {
    perf_init();
    return s_perf.enabled;
}

SrPerfCategory sr_perf_current(void) {
    return s_perf.current;
}

SrPerfCategory sr_perf_switch(SrPerfCategory category) {
    SrPerfCategory previous = s_perf.current;
    perf_init();
    if (category < 0 || category >= SR_PERF_CATEGORY_COUNT)
        return previous;
    if (s_perf.enabled && category != previous)
        perf_record_now();
    s_perf.current = category;
    return previous;
}

void sr_perf_counter_add(SrPerfCounter counter, uint64_t value) {
    perf_init();
    if (!s_perf.enabled ||
        counter < 0 || counter >= SR_PERF_COUNTER_COUNT) {
        return;
    }
    s_perf.counters[counter] += value;
}

void sr_perf_snapshot(SrPerfFrame *out) {
    perf_init();
    memset(out, 0, sizeof(*out));
    if (!s_perf.enabled) return;
    perf_record_now();
    for (int i = 0; i < SR_PERF_CATEGORY_COUNT; i++) {
        out->category_ms[i] =
            (double)s_perf.ticks[i] * 1000.0 /
            (double)s_perf.frequency.QuadPart;
    }
    memcpy(out->counters, s_perf.counters, sizeof(out->counters));
    memset(s_perf.ticks, 0, sizeof(s_perf.ticks));
    memset(s_perf.counters, 0, sizeof(s_perf.counters));
}

/* Exclusive runtime phase timing. The runtime executes on one host thread, including fibers. */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include "perf_stats.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        getenv("SR_TRACE_STALLS") != NULL ||
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

volatile uint32_t sr_perf_alternate_mask;

static const char *const k_path_names[SR_PATH_COUNT] = {
    "texdecode", "merge", "memo", "indexed",
};

static struct {
    uint32_t paths;     /* bit per SrPerfPath named in SR_PERF_ALTERNATE */
    uint32_t period;    /* presentations per on/off half */
    int configured;
} s_alternate;

static void alternate_configure(void) {
    s_alternate.configured = 1;
    s_alternate.period = 60;
    const char *periods = getenv("SR_PERF_ALTERNATE_PRESENTS");
    if (periods && periods[0]) {
        unsigned long parsed = strtoul(periods, NULL, 10);
        if (parsed == 0 || parsed > 100000ul) {
            fprintf(stderr,
                    "SR_PERF_ALTERNATE_PRESENTS: expected 1..100000, got %s\n",
                    periods);
            _Exit(2);
        }
        s_alternate.period = (uint32_t)parsed;
    }
    const char *list = getenv("SR_PERF_ALTERNATE");
    while (list && list[0]) {
        const char *end = strchr(list, ',');
        size_t length = end ? (size_t)(end - list) : strlen(list);
        int found = 0;
        for (int i = 0; i < SR_PATH_COUNT; i++) {
            if (strlen(k_path_names[i]) == length &&
                strncmp(list, k_path_names[i], length) == 0) {
                s_alternate.paths |= 1u << i;
                found = 1;
            }
        }
        if (!found) {
            fprintf(stderr,
                    "SR_PERF_ALTERNATE: unknown path '%.*s' (known: "
                    "texdecode, merge, memo, indexed)\n",
                    (int)length, list);
            _Exit(2);
        }
        list = end ? end + 1 : NULL;
    }
}

/* Called once per presentation, before the next frame starts. The first half of every cycle
 * keeps the paths on and the second half switches them off. */
void sr_perf_alternate_tick(uint32_t present) {
    if (!s_alternate.configured) alternate_configure();
    if (!s_alternate.paths) return;
    sr_perf_alternate_mask =
        ((present / s_alternate.period) & 1u) ? s_alternate.paths : 0u;
}

/* Diagnostic instruction-pointer sampler (SR_SAMPLE_START=<present>, SR_SAMPLE_WINDOW=<presents>,
 * SR_SAMPLE_FILE=<path>). A helper thread suspends the main thread, copies only RIP, and
 * resumes it. Samples are bucketed by module RVA after the window; resolve against a linker map. */
#ifdef _WIN32
#define SR_SAMPLE_CAPACITY (1u << 20)

static struct {
    HANDLE target;
    HANDLE thread;
    volatile LONG stop;
    uint32_t *rvas;
    uintptr_t *ips;
    volatile LONG count;
    uint32_t start, end;
    int configured, running, done;
} s_sampler;

static int compare_rva(const void *left, const void *right) {
    uint32_t a = *(const uint32_t *)left, b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

static DWORD WINAPI sampler_main(LPVOID unused) {
    (void)unused;
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    LARGE_INTEGER frequency, now, next;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&next);
    LONGLONG interval = frequency.QuadPart / 4000;
    while (!s_sampler.stop) {
        next.QuadPart += interval;
        do { QueryPerformanceCounter(&now); } while (now.QuadPart < next.QuadPart && !s_sampler.stop);
        if (SuspendThread(s_sampler.target) == (DWORD)-1) continue;
        CONTEXT context;
        memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_CONTROL;
        BOOL got = GetThreadContext(s_sampler.target, &context);
        uintptr_t rip = got ? (uintptr_t)context.Rip : 0;
        ResumeThread(s_sampler.target);
        LONG index = s_sampler.count;
        if (got && index < (LONG)SR_SAMPLE_CAPACITY) {
            s_sampler.ips[index] = rip;
            s_sampler.rvas[index] = rip >= base && rip - base < 0xffffffffu
                ? (uint32_t)(rip - base) : 0xffffffffu;
            s_sampler.count = index + 1;
        }
    }
    return 0;
}

static void sampler_finish(void) {
    InterlockedExchange(&s_sampler.stop, 1);
    WaitForSingleObject(s_sampler.thread, INFINITE);
    CloseHandle(s_sampler.thread);
    CloseHandle(s_sampler.target);
    s_sampler.running = 0;
    s_sampler.done = 1;
    uint32_t count = (uint32_t)s_sampler.count;
    /* Name the module of every sample outside the executable so driver time is attributable. */
    const char *path0 = getenv("SR_SAMPLE_FILE");
    char modules_path[MAX_PATH + 16];
    snprintf(modules_path, sizeof(modules_path), "%s.modules.txt", path0 && path0[0] ? path0 : "ip_samples.txt");
    FILE *modules = fopen(modules_path, "w");
    if (modules) {
        for (uint32_t i = 0; i < count; i++) {
            if (s_sampler.rvas[i] != 0xffffffffu) continue;
            HMODULE module = NULL;
            wchar_t name[MAX_PATH] = L"?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)s_sampler.ips[i], &module))
                GetModuleFileNameW(module, name, MAX_PATH);
            fprintf(modules, "%ls %llx\n", name,
                    (unsigned long long)((uintptr_t)s_sampler.ips[i] - (uintptr_t)module));
        }
        fclose(modules);
    }
    qsort(s_sampler.rvas, count, sizeof(uint32_t), compare_rva);
    const char *path = getenv("SR_SAMPLE_FILE");
    FILE *file = fopen(path && path[0] ? path : "ip_samples.txt", "w");
    if (file) {
        fprintf(file, "# samples %u\n", count);
        for (uint32_t i = 0; i < count;) {
            uint32_t j = i;
            while (j < count && s_sampler.rvas[j] == s_sampler.rvas[i]) j++;
            fprintf(file, "%08x %u\n", s_sampler.rvas[i], j - i);
            i = j;
        }
        fclose(file);
    }
    free(s_sampler.rvas);
    free(s_sampler.ips);
    s_sampler.rvas = NULL;
    s_sampler.ips = NULL;
}

void sr_ip_sampler_present(uint32_t present) {
    if (s_sampler.done) return;
    if (!s_sampler.configured) {
        s_sampler.configured = 1;
        const char *start = getenv("SR_SAMPLE_START");
        if (!start || !start[0]) { s_sampler.done = 1; return; }
        const char *window = getenv("SR_SAMPLE_WINDOW");
        s_sampler.start = (uint32_t)strtoul(start, NULL, 10);
        s_sampler.end = s_sampler.start +
            (window && window[0] ? (uint32_t)strtoul(window, NULL, 10) : 120u);
    }
    if (!s_sampler.running && present >= s_sampler.start && present < s_sampler.end) {
        s_sampler.rvas = (uint32_t *)malloc(SR_SAMPLE_CAPACITY * sizeof(uint32_t));
        s_sampler.ips = (uintptr_t *)malloc(SR_SAMPLE_CAPACITY * sizeof(uintptr_t));
        if (!s_sampler.rvas || !s_sampler.ips ||
            !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                             &s_sampler.target, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                             FALSE, 0)) {
            s_sampler.done = 1;
            return;
        }
        s_sampler.thread = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
        if (!s_sampler.thread) { s_sampler.done = 1; return; }
        s_sampler.running = 1;
    } else if (s_sampler.running && present >= s_sampler.end) {
        sampler_finish();
    }
}
#else
void sr_ip_sampler_present(uint32_t present) {
    static int warned;
    (void)present;
    if (!warned && getenv("SR_SAMPLE_START")) {
        warned = 1;
        fprintf(
            stderr,
            "SR_SAMPLE_START: instruction-pointer sampling is only "
            "available on Windows\n");
    }
}
#endif

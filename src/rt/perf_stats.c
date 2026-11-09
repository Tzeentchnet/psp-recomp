/* Exclusive runtime phase timing. The runtime executes on one host thread, including fibers. */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include "perf_stats.h"
#include "platform.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <dlfcn.h>
#include <mach/arm/thread_status.h>
#include <mach/i386/thread_status.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdatomic.h>
#endif

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

#ifdef _WIN32
static volatile LONG s_sample_category = SR_PERF_NATIVE;
static void sampler_category_store(SrPerfCategory category) {
    InterlockedExchange(&s_sample_category, (LONG)category);
}
static uint8_t sampler_category_load(void) {
    return (uint8_t)InterlockedCompareExchange(
        &s_sample_category, 0, 0);
}
#elif defined(__APPLE__)
static atomic_uint s_sample_category = SR_PERF_NATIVE;
static void sampler_category_store(SrPerfCategory category) {
    atomic_store_explicit(
        &s_sample_category, (unsigned)category,
        memory_order_relaxed);
}
static uint8_t sampler_category_load(void) {
    return (uint8_t)atomic_load_explicit(
        &s_sample_category, memory_order_relaxed);
}
#else
static void sampler_category_store(SrPerfCategory category) {
    (void)category;
}
#endif

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
    sampler_category_store(category);
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
    "texdecode", "merge", "memo", "indexed", "gpuupload", "gputnl",
    "gputexdecode", "gpuprim",
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
                    "texdecode, merge, memo, indexed, gpuupload, gputnl, "
                    "gputexdecode, gpuprim)\n",
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

/* Diagnostic instruction-pointer sampler (SR_SAMPLE_START=<present>,
 * SR_SAMPLE_WINDOW=<presents>, SR_SAMPLE_FILE=<path>). A helper thread briefly
 * suspends the main thread, copies only its instruction pointer, and resumes it.
 * Samples are bucketed by executable RVA after the window; resolve against a
 * linker map or symbol table. */
#if defined(_WIN32) || defined(__APPLE__)
#define SR_SAMPLE_CAPACITY (1u << 20)
#define SR_SAMPLE_HZ 4000u

static int sampler_configure_window(uint32_t *start_out, uint32_t *end_out) {
    const char *start_setting = getenv("SR_SAMPLE_START");
    if (!start_setting || !start_setting[0]) return 0;
    char *end = NULL;
    errno = 0;
    unsigned long start = strtoul(start_setting, &end, 10);
    if (errno || end == start_setting || *end != '\0' ||
        start > UINT32_MAX) {
        fprintf(
            stderr,
            "SR_SAMPLE_START: expected 0..%u, got '%s'\n",
            UINT32_MAX, start_setting);
        _Exit(2);
    }
    const char *window_setting = getenv("SR_SAMPLE_WINDOW");
    unsigned long window = 120u;
    if (window_setting && window_setting[0]) {
        end = NULL;
        errno = 0;
        window = strtoul(window_setting, &end, 10);
        if (errno || end == window_setting || *end != '\0' ||
            window == 0 || window > UINT32_MAX) {
            fprintf(
                stderr,
                "SR_SAMPLE_WINDOW: expected 1..%u, got '%s'\n",
                UINT32_MAX, window_setting);
            _Exit(2);
        }
    }
    if (start > UINT32_MAX - window) {
        fprintf(
            stderr,
            "SR_SAMPLE_START + SR_SAMPLE_WINDOW exceeds %u\n",
            UINT32_MAX);
        _Exit(2);
    }
    *start_out = (uint32_t)start;
    *end_out = (uint32_t)(start + window);
    return 1;
}

typedef struct SrIpSampleKey {
    uint32_t rva;
    uint8_t category;
} SrIpSampleKey;

static int compare_sample_key(const void *left, const void *right) {
    const SrIpSampleKey *a = (const SrIpSampleKey *)left;
    const SrIpSampleKey *b = (const SrIpSampleKey *)right;
    if (a->rva != b->rva)
        return (a->rva > b->rva) - (a->rva < b->rva);
    return (a->category > b->category) -
        (a->category < b->category);
}

static uint32_t sampler_executable_rva(uintptr_t ip);
static int sampler_write_external(
    FILE *file, uintptr_t ip, uint8_t category);

static int sampler_write_results(
    const uintptr_t *ips, const uint8_t *categories,
    uint32_t count) {
    SrIpSampleKey *keys = (SrIpSampleKey *)malloc(
        (count ? count : 1u) * sizeof(*keys));
    if (!keys) {
        fprintf(stderr, "instruction-pointer sampler: out of memory\n");
        return 0;
    }
    const char *path = getenv("SR_SAMPLE_FILE");
    if (!path || !path[0]) path = "ip_samples.txt";
    char modules_path[SR_PATH_MAX + 32];
    int modules_length = snprintf(
        modules_path, sizeof(modules_path), "%s.modules.txt", path);
    if (modules_length < 0 ||
        (size_t)modules_length >= sizeof(modules_path)) {
        fprintf(
            stderr,
            "SR_SAMPLE_FILE path is too long for the module report\n");
        free(keys);
        return 0;
    }
    char categories_path[SR_PATH_MAX + 32];
    int categories_length = snprintf(
        categories_path, sizeof(categories_path),
        "%s.categories.txt", path);
    if (categories_length < 0 ||
        (size_t)categories_length >= sizeof(categories_path)) {
        fprintf(
            stderr,
            "SR_SAMPLE_FILE path is too long for the category report\n");
        free(keys);
        return 0;
    }

    int ok = 1;
    FILE *modules = fopen(modules_path, "wb");
    if (!modules) {
        fprintf(
            stderr,
            "instruction-pointer sampler: cannot create %s\n",
            modules_path);
        ok = 0;
    } else if (fprintf(
                   modules,
                   "# psp-recomp external instruction-pointer samples v2\n"
                   "# category\tmodule\tmodule_offset\n") <
               0) {
        ok = 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        keys[i].rva = sampler_executable_rva(ips[i]);
        keys[i].category =
            categories[i] < SR_PERF_CATEGORY_COUNT
                ? categories[i] : UINT8_MAX;
        if (keys[i].rva == UINT32_MAX && modules &&
            !sampler_write_external(
                modules, ips[i], keys[i].category)) {
            ok = 0;
        }
    }
    if (modules && fclose(modules) != 0) ok = 0;
    if (!ok) {
        fprintf(
            stderr,
            "instruction-pointer sampler: cannot write %s\n",
            modules_path);
    }

    qsort(keys, count, sizeof(*keys), compare_sample_key);
    FILE *category_file = fopen(categories_path, "wb");
    if (!category_file) {
        fprintf(
            stderr,
            "instruction-pointer sampler: cannot create %s\n",
            categories_path);
        ok = 0;
    } else if (fprintf(
                   category_file,
                   "# psp-recomp categorized instruction-pointer "
                   "samples v1\n"
                   "# rva\tcategory\tsamples\n") < 0) {
        ok = 0;
    }
    for (uint32_t i = 0; i < count && category_file;) {
        uint32_t j = i + 1u;
        while (j < count &&
               keys[j].rva == keys[i].rva &&
               keys[j].category == keys[i].category) {
            j++;
        }
        if (fprintf(
                category_file, "%08x\t%u\t%u\n",
                keys[i].rva, keys[i].category, j - i) < 0) {
            ok = 0;
        }
        i = j;
    }
    if (category_file && fclose(category_file) != 0) ok = 0;

    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(
            stderr,
            "instruction-pointer sampler: cannot create %s\n",
            path);
        free(keys);
        return 0;
    }
    if (fprintf(
            file,
            "# psp-recomp instruction-pointer samples v1\n"
            "# samples %u\n",
            count) < 0) {
        ok = 0;
    }
    for (uint32_t i = 0; i < count && ok;) {
        uint32_t j = i + 1u;
        while (j < count && keys[j].rva == keys[i].rva)
            j++;
        if (fprintf(
                file, "%08x %u\n",
                keys[i].rva, j - i) < 0) {
            ok = 0;
        }
        i = j;
    }
    if (fclose(file) != 0) ok = 0;
    if (!ok) {
        fprintf(
            stderr,
            "instruction-pointer sampler: cannot write %s\n",
            path);
    } else {
        fprintf(
            stderr,
            "instruction-pointer sampler: samples=%u -> %s\n",
            count, path);
    }
    free(keys);
    return ok;
}
#endif

#ifdef _WIN32

static struct {
    HANDLE target;
    HANDLE thread;
    volatile LONG stop;
    uintptr_t *ips;
    uint8_t *categories;
    volatile LONG count;
    uint32_t start, end;
    int configured, running, done, ok;
} s_sampler;

static uint32_t sampler_executable_rva(uintptr_t ip) {
    HMODULE executable = GetModuleHandleW(NULL);
    HMODULE module = NULL;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)ip, &module) ||
        module != executable ||
        ip < (uintptr_t)executable ||
        ip - (uintptr_t)executable >= UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)(ip - (uintptr_t)executable);
}

static int sampler_write_external(
    FILE *file, uintptr_t ip, uint8_t category) {
    HMODULE module = NULL;
    wchar_t name[MAX_PATH] = L"?";
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)ip, &module)) {
        GetModuleFileNameW(module, name, MAX_PATH);
    }
    return fprintf(
               file, "%u\t%ls\t%llx\n", category, name,
               (unsigned long long)(
                   module ? ip - (uintptr_t)module : ip)) >= 0;
}

static DWORD WINAPI sampler_main(LPVOID unused) {
    (void)unused;
    LARGE_INTEGER frequency, now, next;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&next);
    LONGLONG interval = frequency.QuadPart / SR_SAMPLE_HZ;
    if (interval < 1) interval = 1;
    while (!s_sampler.stop) {
        next.QuadPart += interval;
        do {
            QueryPerformanceCounter(&now);
        } while (now.QuadPart < next.QuadPart && !s_sampler.stop);
        if (SuspendThread(s_sampler.target) == (DWORD)-1) continue;
        CONTEXT context;
        memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_CONTROL;
        BOOL got = GetThreadContext(s_sampler.target, &context);
        uintptr_t rip = got ? (uintptr_t)context.Rip : 0;
        uint8_t category = sampler_category_load();
        ResumeThread(s_sampler.target);
        LONG index = s_sampler.count;
        if (got && index < (LONG)SR_SAMPLE_CAPACITY) {
            s_sampler.ips[index] = rip;
            s_sampler.categories[index] = category;
            s_sampler.count = index + 1;
        }
    }
    return 0;
}

static void sampler_finish(void) {
    if (!s_sampler.running) return;
    InterlockedExchange(&s_sampler.stop, 1);
    WaitForSingleObject(s_sampler.thread, INFINITE);
    CloseHandle(s_sampler.thread);
    CloseHandle(s_sampler.target);
    s_sampler.running = 0;
    s_sampler.done = 1;
    uint32_t count = (uint32_t)s_sampler.count;
    if (!sampler_write_results(
            s_sampler.ips, s_sampler.categories, count)) {
        s_sampler.ok = 0;
    }
    free(s_sampler.ips);
    free(s_sampler.categories);
    s_sampler.ips = NULL;
    s_sampler.categories = NULL;
}

void sr_ip_sampler_present(uint32_t present) {
    if (s_sampler.done) return;
    if (!s_sampler.configured) {
        s_sampler.configured = 1;
        s_sampler.ok = 1;
        if (!sampler_configure_window(
                &s_sampler.start, &s_sampler.end)) {
            s_sampler.done = 1;
            return;
        }
    }
    if (!s_sampler.running && present >= s_sampler.start && present < s_sampler.end) {
        s_sampler.ips = (uintptr_t *)malloc(SR_SAMPLE_CAPACITY * sizeof(uintptr_t));
        s_sampler.categories =
            (uint8_t *)malloc(SR_SAMPLE_CAPACITY);
        if (!s_sampler.ips || !s_sampler.categories ||
            !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                             &s_sampler.target, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                             FALSE, 0)) {
            fprintf(
                stderr,
                "instruction-pointer sampler: cannot capture the main thread\n");
            free(s_sampler.ips);
            free(s_sampler.categories);
            s_sampler.ips = NULL;
            s_sampler.categories = NULL;
            s_sampler.ok = 0;
            s_sampler.done = 1;
            return;
        }
        s_sampler.thread = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
        if (!s_sampler.thread) {
            fprintf(
                stderr,
                "instruction-pointer sampler: cannot create sampler thread\n");
            CloseHandle(s_sampler.target);
            free(s_sampler.ips);
            free(s_sampler.categories);
            s_sampler.ips = NULL;
            s_sampler.categories = NULL;
            s_sampler.ok = 0;
            s_sampler.done = 1;
            return;
        }
        s_sampler.running = 1;
    } else if (s_sampler.running && present >= s_sampler.end) {
        sampler_finish();
    }
}

int sr_ip_sampler_shutdown(void) {
    sampler_finish();
    return !s_sampler.configured || s_sampler.ok;
}
#elif defined(__APPLE__)
static struct {
    thread_t target;
    pthread_t thread;
    atomic_int stop;
    atomic_uint count;
    uintptr_t *ips;
    uint8_t *categories;
    uint32_t start, end;
    uint32_t suspend_failures, state_failures;
    int configured, running, done, ok, thread_failed;
} s_sampler;

static uint32_t sampler_executable_rva(uintptr_t ip) {
    Dl_info info;
    const void *executable = _dyld_get_image_header(0);
    if (!dladdr((const void *)ip, &info) ||
        info.dli_fbase != executable ||
        ip < (uintptr_t)executable ||
        ip - (uintptr_t)executable >= UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)(ip - (uintptr_t)executable);
}

static int sampler_write_external(
    FILE *file, uintptr_t ip, uint8_t category) {
    Dl_info info;
    if (dladdr((const void *)ip, &info) &&
        info.dli_fname && info.dli_fbase) {
        return fprintf(
                   file, "%u\t%s\t%llx\n", category, info.dli_fname,
                   (unsigned long long)(
                       ip - (uintptr_t)info.dli_fbase)) >= 0;
    }
    return fprintf(
               file, "%u\t?\t%llx\n", category,
               (unsigned long long)ip) >= 0;
}

static int sampler_read_pc(thread_t target, uintptr_t *pc_out) {
#if defined(__aarch64__)
    arm_thread_state64_t state;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    kern_return_t result = thread_get_state(
        target, ARM_THREAD_STATE64,
        (thread_state_t)&state, &count);
    if (result != KERN_SUCCESS) return 0;
    *pc_out = arm_thread_state64_get_pc(state);
    return 1;
#elif defined(__x86_64__)
    x86_thread_state64_t state;
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    kern_return_t result = thread_get_state(
        target, x86_THREAD_STATE64,
        (thread_state_t)&state, &count);
    if (result != KERN_SUCCESS) return 0;
    *pc_out = (uintptr_t)state.__rip;
    return 1;
#else
    (void)target;
    (void)pc_out;
    return 0;
#endif
}

static void *sampler_main(void *unused) {
    (void)unused;
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    uint64_t interval =
        (1000000000ull / SR_SAMPLE_HZ) * timebase.denom /
        timebase.numer;
    if (!interval) interval = 1;
    uint64_t next = mach_absolute_time();
    while (!atomic_load_explicit(
               &s_sampler.stop, memory_order_relaxed)) {
        next += interval;
        mach_wait_until(next);
        if (atomic_load_explicit(
                &s_sampler.stop, memory_order_relaxed)) {
            break;
        }
        if (thread_suspend(s_sampler.target) != KERN_SUCCESS) {
            s_sampler.suspend_failures++;
            continue;
        }
        uintptr_t pc = 0;
        int got = sampler_read_pc(s_sampler.target, &pc);
        uint8_t category = sampler_category_load();
        kern_return_t resumed = thread_resume(s_sampler.target);
        if (resumed != KERN_SUCCESS) {
            s_sampler.thread_failed = 1;
            break;
        }
        if (!got) {
            s_sampler.state_failures++;
            continue;
        }
        unsigned index = atomic_load_explicit(
            &s_sampler.count, memory_order_relaxed);
        if (index < SR_SAMPLE_CAPACITY) {
            s_sampler.ips[index] = pc;
            s_sampler.categories[index] = category;
            atomic_store_explicit(
                &s_sampler.count, index + 1u,
                memory_order_relaxed);
        }
    }
    return NULL;
}

static void sampler_finish(void) {
    if (!s_sampler.running) return;
    atomic_store_explicit(
        &s_sampler.stop, 1, memory_order_relaxed);
    if (pthread_join(s_sampler.thread, NULL) != 0) {
        fprintf(
            stderr,
            "instruction-pointer sampler: cannot join sampler thread\n");
        s_sampler.ok = 0;
    }
    mach_port_deallocate(mach_task_self(), s_sampler.target);
    s_sampler.running = 0;
    s_sampler.done = 1;
    uint32_t count = atomic_load_explicit(
        &s_sampler.count, memory_order_relaxed);
    if (s_sampler.thread_failed) {
        fprintf(
            stderr,
            "instruction-pointer sampler: target thread resume failed\n");
        s_sampler.ok = 0;
    }
    if (s_sampler.suspend_failures || s_sampler.state_failures) {
        fprintf(
            stderr,
            "instruction-pointer sampler: skipped suspend=%u state=%u\n",
            s_sampler.suspend_failures,
            s_sampler.state_failures);
    }
    if (!sampler_write_results(
            s_sampler.ips, s_sampler.categories, count)) {
        s_sampler.ok = 0;
    }
    free(s_sampler.ips);
    free(s_sampler.categories);
    s_sampler.ips = NULL;
    s_sampler.categories = NULL;
}

void sr_ip_sampler_present(uint32_t present) {
    if (s_sampler.done) return;
    if (!s_sampler.configured) {
        s_sampler.configured = 1;
        s_sampler.ok = 1;
        if (!sampler_configure_window(
                &s_sampler.start, &s_sampler.end)) {
            s_sampler.done = 1;
            return;
        }
    }
    if (!s_sampler.running &&
        present >= s_sampler.start &&
        present < s_sampler.end) {
        s_sampler.ips = (uintptr_t *)malloc(
            SR_SAMPLE_CAPACITY * sizeof(*s_sampler.ips));
        s_sampler.categories =
            (uint8_t *)malloc(SR_SAMPLE_CAPACITY);
        if (!s_sampler.ips || !s_sampler.categories) {
            fprintf(
                stderr,
                "instruction-pointer sampler: out of memory\n");
            free(s_sampler.ips);
            free(s_sampler.categories);
            s_sampler.ips = NULL;
            s_sampler.categories = NULL;
            s_sampler.ok = 0;
            s_sampler.done = 1;
            return;
        }
        s_sampler.target = mach_thread_self();
        atomic_store_explicit(
            &s_sampler.stop, 0, memory_order_relaxed);
        atomic_store_explicit(
            &s_sampler.count, 0, memory_order_relaxed);
        int result = pthread_create(
            &s_sampler.thread, NULL, sampler_main, NULL);
        if (result != 0) {
            fprintf(
                stderr,
                "instruction-pointer sampler: cannot create sampler "
                "thread: %s\n",
                strerror(result));
            mach_port_deallocate(
                mach_task_self(), s_sampler.target);
            free(s_sampler.ips);
            free(s_sampler.categories);
            s_sampler.ips = NULL;
            s_sampler.categories = NULL;
            s_sampler.ok = 0;
            s_sampler.done = 1;
            return;
        }
        s_sampler.running = 1;
    } else if (s_sampler.running && present >= s_sampler.end) {
        sampler_finish();
    }
}

int sr_ip_sampler_shutdown(void) {
    sampler_finish();
    return !s_sampler.configured || s_sampler.ok;
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
            "available on Windows and macOS\n");
    }
}

int sr_ip_sampler_shutdown(void) {
    return 1;
}
#endif

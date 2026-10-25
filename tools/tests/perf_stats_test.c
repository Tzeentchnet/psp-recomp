#define WIN32_LEAN_AND_MEAN
#include "perf_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static void require(int condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

int main(void) {
    SrPerfFrame frame;
    _putenv_s("SR_PERF_LOG", "1");
    require(sr_perf_enabled(), "profiler did not enable");
    require(sr_perf_current() == SR_PERF_NATIVE, "unexpected initial category");

    Sleep(2);
    SrPerfCategory previous = sr_perf_switch(SR_PERF_HLE);
    require(previous == SR_PERF_NATIVE, "category switch did not return native");
    Sleep(2);
    sr_perf_counter_add(SR_PERF_HLE_CALLS, 3);
    sr_perf_counter_add(SR_PERF_GE_COMMANDS, 7);
    sr_perf_switch(previous);
    sr_perf_snapshot(&frame);

    require(frame.category_ms[SR_PERF_NATIVE] > 0.0, "native time missing");
    require(frame.category_ms[SR_PERF_HLE] > 0.0, "HLE time missing");
    require(frame.counters[SR_PERF_HLE_CALLS] == 3, "HLE count mismatch");
    require(frame.counters[SR_PERF_GE_COMMANDS] == 7, "GE count mismatch");

    sr_perf_snapshot(&frame);
    require(frame.counters[SR_PERF_HLE_CALLS] == 0, "counters did not reset");
    puts("perf_stats_test: PASS");
    return 0;
}

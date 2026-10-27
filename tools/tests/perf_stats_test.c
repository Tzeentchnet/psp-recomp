#define WIN32_LEAN_AND_MEAN
#include "perf_stats.h"
#include "test_platform.h"

#include <stdio.h>
#include <stdlib.h>

static void require(int condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

int main(void) {
    SrPerfFrame frame;
    require(sr_test_set_env("SR_PERF_LOG", "1"), "could not set profiler");
    require(sr_perf_enabled(), "profiler did not enable");
    require(sr_perf_current() == SR_PERF_NATIVE, "unexpected initial category");

    sr_test_sleep_ms(2);
    SrPerfCategory previous = sr_perf_switch(SR_PERF_HLE);
    require(previous == SR_PERF_NATIVE, "category switch did not return native");
    sr_test_sleep_ms(2);
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

    /* A/B alternation: ten presentations per half, two paths. */
    require(
        sr_test_set_env("SR_PERF_ALTERNATE", "merge,memo"),
        "could not set alternating paths");
    require(
        sr_test_set_env("SR_PERF_ALTERNATE_PRESENTS", "10"),
        "could not set alternation interval");
    sr_perf_alternate_tick(0);
    require(sr_perf_alternate_mask == 0, "alternation starts with paths on");
    require(!sr_perf_path_off(SR_PATH_DRAW_MERGE), "merge should start on");
    sr_perf_alternate_tick(9);
    require(sr_perf_alternate_mask == 0, "first half keeps paths on");
    sr_perf_alternate_tick(10);
    require(sr_perf_path_off(SR_PATH_DRAW_MERGE), "merge should switch off");
    require(sr_perf_path_off(SR_PATH_STATE_MEMO), "memo should switch off");
    require(!sr_perf_path_off(SR_PATH_TEXTURE_DECODE),
            "unnamed paths must stay on");
    require(!sr_perf_path_off(SR_PATH_INDEXED_CAPTURE),
            "unnamed paths must stay on");
    sr_perf_alternate_tick(20);
    require(sr_perf_alternate_mask == 0, "paths should switch back on");
    puts("perf_stats_test: PASS");
    return 0;
}

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
    sr_perf_counter_add(SR_PERF_GPU_TNL_ELIGIBLE_DRAWS, 11);
    sr_perf_counter_add(SR_PERF_GPU_TNL_ACCEPTED_DRAWS, 9);
    sr_perf_counter_add(SR_PERF_GPU_TNL_DECLINED_DRAWS, 2);
    sr_perf_counter_add(SR_PERF_GPU_TNL_CANONICAL_VERTICES, 31);
    sr_perf_counter_add(SR_PERF_GPU_TNL_SUBMITTED_VERTICES, 54);
    sr_perf_counter_add(SR_PERF_GPU_TNL_CPU_DECODE_US, 13);
    sr_perf_counter_add(SR_PERF_GPU_TNL_CPU_REFERENCE_US, 17);
    sr_perf_counter_add(SR_PERF_GPU_TNL_UPLOAD_BYTES, 4096);
    sr_perf_counter_add(SR_PERF_GPU_TEXTURE_DECODE_JOBS, 5);
    sr_perf_counter_add(SR_PERF_GPU_TEXTURE_DECODE_FALLBACKS, 2);
    sr_perf_counter_add(
        SR_PERF_GPU_TEXTURE_DECODE_SOURCE_BYTES, 8192);
    sr_perf_counter_add(SR_PERF_GE_POINT_DRAWS, 2);
    sr_perf_counter_add(SR_PERF_GE_POINTS, 7);
    sr_perf_counter_add(SR_PERF_GE_LINE_DRAWS, 3);
    sr_perf_counter_add(SR_PERF_GE_LINE_SEGMENTS, 11);
    sr_perf_counter_add(SR_PERF_GE_SPRITE_DRAWS, 5);
    sr_perf_counter_add(SR_PERF_GE_SPRITES, 13);
    sr_perf_counter_add(SR_PERF_GE_BEZIER_DRAWS, 17);
    sr_perf_counter_add(SR_PERF_GE_BEZIER_CONTROL_POINTS, 19);
    sr_perf_counter_add(
        SR_PERF_GE_BEZIER_EVALUATED_VERTICES, 23);
    sr_perf_counter_add(SR_PERF_GE_SPLINE_DRAWS, 29);
    sr_perf_counter_add(SR_PERF_GE_SPLINE_CONTROL_POINTS, 31);
    sr_perf_counter_add(
        SR_PERF_GE_SPLINE_EVALUATED_VERTICES, 37);
    sr_perf_counter_add(SR_PERF_NATIVE_DISPATCHES, 41);
    sr_perf_counter_add(SR_PERF_SCHEDULER_YIELDS, 43);
    sr_perf_switch(previous);
    sr_perf_snapshot(&frame);

    require(frame.category_ms[SR_PERF_NATIVE] > 0.0, "native time missing");
    require(frame.category_ms[SR_PERF_HLE] > 0.0, "HLE time missing");
    require(frame.counters[SR_PERF_HLE_CALLS] == 3, "HLE count mismatch");
    require(frame.counters[SR_PERF_GE_COMMANDS] == 7, "GE count mismatch");
    require(
        frame.counters[SR_PERF_GPU_TNL_ELIGIBLE_DRAWS] == 11,
        "GPU T&L eligible count mismatch");
    require(
        frame.counters[SR_PERF_GPU_TNL_ACCEPTED_DRAWS] == 9,
        "GPU T&L accepted count mismatch");
    require(
        frame.counters[SR_PERF_GPU_TNL_DECLINED_DRAWS] == 2,
        "GPU T&L declined count mismatch");
    require(
        frame.counters[SR_PERF_GPU_TNL_CANONICAL_VERTICES] == 31,
        "GPU T&L canonical vertex count mismatch");
    require(
        frame.counters[SR_PERF_GPU_TNL_SUBMITTED_VERTICES] == 54,
        "GPU T&L submitted vertex count mismatch");
    require(
        frame.counters[SR_PERF_GPU_TNL_CPU_DECODE_US] == 13,
        "GPU T&L decode time mismatch");
    require(
        frame.counters[SR_PERF_GPU_TNL_CPU_REFERENCE_US] == 17,
        "GPU T&L reference time mismatch");
    require(
        frame.counters[SR_PERF_GPU_TNL_UPLOAD_BYTES] == 4096,
        "GPU T&L upload byte count mismatch");
    require(
        frame.counters[SR_PERF_GPU_TEXTURE_DECODE_JOBS] == 5,
        "GPU texture decode job count mismatch");
    require(
        frame.counters[SR_PERF_GPU_TEXTURE_DECODE_FALLBACKS] == 2,
        "GPU texture decode fallback count mismatch");
    require(
        frame.counters[SR_PERF_GPU_TEXTURE_DECODE_SOURCE_BYTES] == 8192,
        "GPU texture decode source byte count mismatch");
    require(
        frame.counters[SR_PERF_GE_POINT_DRAWS] == 2 &&
            frame.counters[SR_PERF_GE_POINTS] == 7,
        "point workload mismatch");
    require(
        frame.counters[SR_PERF_GE_LINE_DRAWS] == 3 &&
            frame.counters[SR_PERF_GE_LINE_SEGMENTS] == 11,
        "line workload mismatch");
    require(
        frame.counters[SR_PERF_GE_SPRITE_DRAWS] == 5 &&
            frame.counters[SR_PERF_GE_SPRITES] == 13,
        "sprite workload mismatch");
    require(
        frame.counters[SR_PERF_GE_BEZIER_DRAWS] == 17 &&
            frame.counters[SR_PERF_GE_BEZIER_CONTROL_POINTS] == 19 &&
            frame.counters[
                SR_PERF_GE_BEZIER_EVALUATED_VERTICES] == 23,
        "Bezier workload mismatch");
    require(
        frame.counters[SR_PERF_GE_SPLINE_DRAWS] == 29 &&
            frame.counters[SR_PERF_GE_SPLINE_CONTROL_POINTS] == 31 &&
            frame.counters[
                SR_PERF_GE_SPLINE_EVALUATED_VERTICES] == 37,
        "spline workload mismatch");
    require(
        frame.counters[SR_PERF_NATIVE_DISPATCHES] == 41,
        "native dispatch count mismatch");
    require(
        frame.counters[SR_PERF_SCHEDULER_YIELDS] == 43,
        "scheduler yield count mismatch");

    sr_perf_snapshot(&frame);
    require(frame.counters[SR_PERF_HLE_CALLS] == 0, "counters did not reset");
    require(
        frame.counters[SR_PERF_GPU_TNL_UPLOAD_BYTES] == 0,
        "GPU T&L counters did not reset");
    require(
        frame.counters[SR_PERF_GPU_TEXTURE_DECODE_JOBS] == 0,
        "GPU texture decode counters did not reset");
    require(
        frame.counters[SR_PERF_GE_SPLINE_EVALUATED_VERTICES] == 0,
        "geometry workload counters did not reset");
    require(
        frame.counters[SR_PERF_NATIVE_DISPATCHES] == 0 &&
            frame.counters[SR_PERF_SCHEDULER_YIELDS] == 0,
        "runtime counters did not reset");

    /* A/B alternation: ten presentations per half. */
    require(
        sr_test_set_env(
            "SR_PERF_ALTERNATE",
            "merge,memo,gpuupload,gputnl,gputexdecode,gpuprim"),
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
    require(sr_perf_path_off(SR_PATH_GPU_UPLOAD),
            "GPU upload should switch off");
    require(sr_perf_path_off(SR_PATH_GPU_TNL),
            "GPU T&L should switch off");
    require(sr_perf_path_off(SR_PATH_GPU_TEXTURE_DECODE),
            "GPU texture decode should switch off");
    require(sr_perf_path_off(SR_PATH_GPU_PRIMITIVE_EXPANSION),
            "GPU primitive expansion should switch off");
    require(!sr_perf_path_off(SR_PATH_TEXTURE_DECODE),
            "unnamed paths must stay on");
    require(!sr_perf_path_off(SR_PATH_INDEXED_CAPTURE),
            "unnamed paths must stay on");
    sr_perf_alternate_tick(20);
    require(sr_perf_alternate_mask == 0, "paths should switch back on");

#if defined(_WIN32) || defined(__APPLE__)
    const char *samples = "perf_stats_test.samples";
    const char *modules = "perf_stats_test.samples.modules.txt";
    const char *categories = "perf_stats_test.samples.categories.txt";
    remove(samples);
    remove(modules);
    remove(categories);
    require(
        sr_test_set_env("SR_SAMPLE_START", "1"),
        "could not set sample start");
    require(
        sr_test_set_env("SR_SAMPLE_WINDOW", "1"),
        "could not set sample window");
    require(
        sr_test_set_env("SR_SAMPLE_FILE", samples),
        "could not set sample path");
    sr_ip_sampler_present(1);
    sr_test_sleep_ms(20);
    sr_ip_sampler_present(2);
    require(sr_ip_sampler_shutdown(), "sampler shutdown failed");
    FILE *sample_file = fopen(samples, "rb");
    require(sample_file != NULL, "sample report missing");
    char line[128];
    unsigned sample_count = 0;
    while (fgets(line, sizeof(line), sample_file)) {
        if (sscanf(line, "# samples %u", &sample_count) == 1)
            break;
    }
    require(fclose(sample_file) == 0, "could not close sample report");
    require(sample_count > 0, "sample report was empty");
    sample_file = fopen(modules, "rb");
    require(sample_file != NULL, "sample module report missing");
    require(fclose(sample_file) == 0, "could not close module report");
    sample_file = fopen(categories, "rb");
    require(sample_file != NULL, "sample category report missing");
    int found_category = 0;
    while (fgets(line, sizeof(line), sample_file)) {
        unsigned rva, category, count;
        if (sscanf(line, "%x\t%u\t%u", &rva, &category, &count) == 3 &&
            category == SR_PERF_NATIVE && count > 0) {
            found_category = 1;
            break;
        }
    }
    require(fclose(sample_file) == 0, "could not close category report");
    require(found_category, "native sample category missing");
    require(remove(samples) == 0, "could not remove sample report");
    require(remove(modules) == 0, "could not remove module report");
    require(remove(categories) == 0, "could not remove category report");
#else
    require(sr_ip_sampler_shutdown(), "unsupported sampler shutdown failed");
#endif

    puts("perf_stats_test: PASS");
    return 0;
}

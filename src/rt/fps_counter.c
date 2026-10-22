#include "fps_counter.h"

#include <string.h>

void sr_fps_counter_reset(SrFpsCounter *counter) {
    if (counter) memset(counter, 0, sizeof(*counter));
}

int sr_fps_counter_tick(
    SrFpsCounter *counter,
    uint64_t ticks,
    uint64_t ticks_per_second,
    double update_seconds,
    double *fps,
    double *frame_ms) {
    if (!counter || !ticks_per_second || update_seconds <= 0.0 ||
        !fps || !frame_ms)
        return 0;
    if (!counter->initialized || ticks < counter->window_start) {
        counter->window_start = ticks;
        counter->frame_intervals = 0;
        counter->initialized = 1;
        return 0;
    }
    counter->frame_intervals++;
    double elapsed =
        (double)(ticks - counter->window_start) /
        (double)ticks_per_second;
    if (elapsed < update_seconds) return 0;
    *fps = counter->frame_intervals / elapsed;
    *frame_ms = *fps > 0.0 ? 1000.0 / *fps : 0.0;
    counter->window_start = ticks;
    counter->frame_intervals = 0;
    return 1;
}

#ifndef SR_FPS_COUNTER_H
#define SR_FPS_COUNTER_H

#include <stdint.h>

typedef struct SrFpsCounter {
    uint64_t window_start;
    uint32_t frame_intervals;
    int initialized;
} SrFpsCounter;

void sr_fps_counter_reset(SrFpsCounter *counter);
int sr_fps_counter_tick(
    SrFpsCounter *counter,
    uint64_t ticks,
    uint64_t ticks_per_second,
    double update_seconds,
    double *fps,
    double *frame_ms);

#endif

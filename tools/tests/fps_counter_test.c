#include "fps_counter.h"

#include <math.h>
#include <stdio.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf( \
                stderr, "fps_counter_test:%d: %s\n", \
                __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

int main(void) {
    SrFpsCounter counter;
    double fps = 0.0;
    double frame_ms = 0.0;
    sr_fps_counter_reset(&counter);

    CHECK(!sr_fps_counter_tick(
        &counter, 0, 60000, 0.5, &fps, &frame_ms));
    for (uint64_t frame = 1; frame < 30; frame++) {
        CHECK(!sr_fps_counter_tick(
            &counter, frame * 1000, 60000, 0.5,
            &fps, &frame_ms));
    }
    CHECK(sr_fps_counter_tick(
        &counter, 30000, 60000, 0.5, &fps, &frame_ms));
    CHECK(fabs(fps - 60.0) < 0.0001);
    CHECK(fabs(frame_ms - (1000.0 / 60.0)) < 0.0001);

    CHECK(!sr_fps_counter_tick(
        &counter, 100, 60000, 0.5, &fps, &frame_ms));
    CHECK(counter.frame_intervals == 0);

    puts("FPS counter tests passed");
    return 0;
}

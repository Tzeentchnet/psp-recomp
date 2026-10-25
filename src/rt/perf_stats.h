/* Low-overhead exclusive runtime phase timing for performance samples. */
#ifndef PSP_RECOMP_PERF_STATS_H
#define PSP_RECOMP_PERF_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SrPerfCategory {
    SR_PERF_NATIVE = 0,
    SR_PERF_HLE,
    SR_PERF_INTERPRETER,
    SR_PERF_GE,
    SR_PERF_VERTEX,
    SR_PERF_SYNC,
    SR_PERF_BACKEND,
    SR_PERF_SCHEDULER,
    SR_PERF_PRIMITIVE,
    SR_PERF_BEZIER,
    SR_PERF_SPLINE,
    SR_PERF_PATCH_EVAL,
    SR_PERF_PATCH_DRAW,
    SR_PERF_VERTEX_DECODE,
    SR_PERF_CAPTURE,
    SR_PERF_TEXTURE_CAPTURE,
    SR_PERF_STATE_CAPTURE,
    SR_PERF_CATEGORY_COUNT
} SrPerfCategory;

typedef enum SrPerfCounter {
    SR_PERF_HLE_CALLS = 0,
    SR_PERF_INTERP_INSTRUCTIONS,
    SR_PERF_GE_LISTS,
    SR_PERF_GE_COMMANDS,
    SR_PERF_GE_DRAWS,
    SR_PERF_GE_VERTICES,
    SR_PERF_GE_THROUGH_DRAWS,
    SR_PERF_GE_THROUGH_VERTICES,
    SR_PERF_GE_TRANSFORM_DRAWS,
    SR_PERF_GE_TRANSFORM_VERTICES,
    SR_PERF_GE_TRANSFORM_UNIQUE_VERTICES,
    SR_PERF_COUNTER_COUNT
} SrPerfCounter;

typedef struct SrPerfFrame {
    double category_ms[SR_PERF_CATEGORY_COUNT];
    uint64_t counters[SR_PERF_COUNTER_COUNT];
} SrPerfFrame;

int sr_perf_enabled(void);
SrPerfCategory sr_perf_current(void);
SrPerfCategory sr_perf_switch(SrPerfCategory category);
void sr_perf_counter_add(SrPerfCounter counter, uint64_t value);
void sr_perf_snapshot(SrPerfFrame *out);
void sr_ip_sampler_present(uint32_t present);

#ifdef __cplusplus
}
#endif

#endif

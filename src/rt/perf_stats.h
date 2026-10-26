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
    SR_PERF_TEXTURE_MISSES,
    SR_PERF_TEXTURE_DECODE_WAIT_US,
    SR_PERF_PSO_FALLBACKS,
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

/* Optimization paths that SR_PERF_ALTERNATE=<name>[,<name>...] can switch off every other
 * SR_PERF_ALTERNATE_PRESENTS presentations (default 60) so one run measures a path on and off
 * in the same scene. Only output-identical paths are listed. The state changes at presentation
 * boundaries, so a frame never sees a path half on. */
typedef enum SrPerfPath {
    SR_PATH_TEXTURE_DECODE = 0, /* background texture decode and scale */
    SR_PATH_DRAW_MERGE,         /* adjacent direct draw merging */
    SR_PATH_STATE_MEMO,         /* pipeline key and shader constant memo */
    SR_PATH_INDEXED_CAPTURE,    /* indexed triangle capture */
    SR_PATH_COUNT
} SrPerfPath;

extern volatile uint32_t sr_perf_alternate_mask;
void sr_perf_alternate_tick(uint32_t present);
/* Nonzero while an alternating run has `path` switched off. */
static inline int sr_perf_path_off(SrPerfPath path) {
    return (int)((sr_perf_alternate_mask >> path) & 1u);
}

#ifdef __cplusplus
}
#endif

#endif

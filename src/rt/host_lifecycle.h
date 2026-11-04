#ifndef PSP_RECOMP_HOST_LIFECYCLE_H
#define PSP_RECOMP_HOST_LIFECYCLE_H

#include <stdint.h>

typedef enum SrHostLifecycleEvent {
    SR_HOST_SYSTEM_SUSPEND = 0,
    SR_HOST_SYSTEM_RESUME,
    SR_HOST_FOCUS_LOST,
    SR_HOST_FOCUS_GAINED,
    SR_HOST_WINDOW_MINIMIZED,
    SR_HOST_WINDOW_RESTORED,
} SrHostLifecycleEvent;

enum {
    SR_HOST_ACTION_CLEAR_INPUT = 1u << 0,
    SR_HOST_ACTION_AUDIO_SUSPEND = 1u << 1,
    SR_HOST_ACTION_AUDIO_RESUME = 1u << 2,
    SR_HOST_ACTION_REBASE_CLOCK = 1u << 3,
    SR_HOST_ACTION_REDRAW = 1u << 4,
};

typedef struct SrHostLifecycle {
    int system_suspended;
    int window_focused;
    int window_minimized;
} SrHostLifecycle;

void sr_host_lifecycle_init(
    SrHostLifecycle *state,
    int system_suspended,
    int window_focused,
    int window_minimized);
uint32_t sr_host_lifecycle_apply(
    SrHostLifecycle *state,
    SrHostLifecycleEvent event);
int sr_host_lifecycle_input_active(
    const SrHostLifecycle *state);

#endif

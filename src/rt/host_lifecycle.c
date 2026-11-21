#include "host_lifecycle.h"

#include <string.h>

void sr_host_lifecycle_init(
    SrHostLifecycle *state,
    int system_suspended,
    int window_focused,
    int window_minimized) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->system_suspended = system_suspended != 0;
    state->window_focused = window_focused != 0;
    state->window_minimized = window_minimized != 0;
}

uint32_t sr_host_lifecycle_apply(
    SrHostLifecycle *state,
    SrHostLifecycleEvent event) {
    if (!state) return 0;
    switch (event) {
    case SR_HOST_SYSTEM_SUSPEND:
        if (state->system_suspended) return 0;
        state->system_suspended = 1;
        return SR_HOST_ACTION_CLEAR_INPUT |
               SR_HOST_ACTION_AUDIO_SUSPEND;
    case SR_HOST_SYSTEM_RESUME:
        if (!state->system_suspended) return 0;
        state->system_suspended = 0;
        return SR_HOST_ACTION_AUDIO_RESUME |
               SR_HOST_ACTION_REBASE_CLOCK |
               SR_HOST_ACTION_REDRAW;
    case SR_HOST_FOCUS_LOST:
        if (!state->window_focused) return 0;
        state->window_focused = 0;
        return SR_HOST_ACTION_CLEAR_INPUT;
    case SR_HOST_FOCUS_GAINED:
        if (state->window_focused) return 0;
        state->window_focused = 1;
        return SR_HOST_ACTION_REDRAW;
    case SR_HOST_WINDOW_MINIMIZED:
        if (state->window_minimized) return 0;
        state->window_minimized = 1;
        return SR_HOST_ACTION_CLEAR_INPUT;
    case SR_HOST_WINDOW_RESTORED:
        if (!state->window_minimized) return 0;
        state->window_minimized = 0;
        return SR_HOST_ACTION_REDRAW;
    default:
        return 0;
    }
}

int sr_host_lifecycle_input_active(
    const SrHostLifecycle *state) {
    return state && !state->system_suspended &&
           state->window_focused && !state->window_minimized;
}

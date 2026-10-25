#include "display_state.h"

#include <string.h>

void sr_display_state_init(SrDisplayState *state) {
    memset(state, 0, sizeof(*state));
}

int sr_display_state_set_framebuffer(
    SrDisplayState *state, const SrDisplayFramebuffer *framebuffer, int sync) {
    if (sync == SR_DISPLAY_SETBUF_IMMEDIATE) {
        state->current = *framebuffer;
        state->latched = *framebuffer;
        if (!state->flipped_this_frame) {
            state->flipped_this_frame = 1;
            return SR_DISPLAY_FLIP;
        }
        return SR_DISPLAY_NO_FLIP;
    }
    if (sync == SR_DISPLAY_SETBUF_NEXTFRAME) {
        state->latched = *framebuffer;
        state->latched_pending = 1;
        state->current.stride = framebuffer->stride;
        state->current.format = framebuffer->format;
        return SR_DISPLAY_NO_FLIP;
    }
    return SR_DISPLAY_INVALID_SYNC;
}

int sr_display_state_begin_vblank(SrDisplayState *state) {
    if (state->latched_pending) {
        state->current = state->latched;
        state->latched_pending = 0;
        state->flipped_this_frame = 1;
        return SR_DISPLAY_FLIP;
    }
    if (!state->flipped_this_frame) {
        state->flipped_this_frame = 1;
        return SR_DISPLAY_FLIP;
    }
    return SR_DISPLAY_NO_FLIP;
}

void sr_display_state_end_vblank(SrDisplayState *state) {
    state->flipped_this_frame = 0;
}

const SrDisplayFramebuffer *sr_display_state_get_framebuffer(
    const SrDisplayState *state, int latched) {
    return latched ? &state->latched : &state->current;
}

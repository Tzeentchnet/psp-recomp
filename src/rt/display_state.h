#ifndef SR_DISPLAY_STATE_H
#define SR_DISPLAY_STATE_H

#include <stdint.h>

enum {
    SR_DISPLAY_SETBUF_IMMEDIATE = 0,
    SR_DISPLAY_SETBUF_NEXTFRAME = 1,
};

typedef struct SrDisplayFramebuffer {
    uint32_t address;
    uint32_t stride;
    uint32_t format;
} SrDisplayFramebuffer;

typedef struct SrDisplayState {
    SrDisplayFramebuffer current;
    SrDisplayFramebuffer latched;
    int latched_pending;
    int flipped_this_frame;
} SrDisplayState;

enum {
    SR_DISPLAY_INVALID_SYNC = -1,
    SR_DISPLAY_NO_FLIP = 0,
    SR_DISPLAY_FLIP = 1,
};

void sr_display_state_init(SrDisplayState *state);
int sr_display_state_set_framebuffer(
    SrDisplayState *state, const SrDisplayFramebuffer *framebuffer, int sync);
int sr_display_state_begin_vblank(SrDisplayState *state);
void sr_display_state_end_vblank(SrDisplayState *state);
const SrDisplayFramebuffer *sr_display_state_get_framebuffer(
    const SrDisplayState *state, int latched);

#endif

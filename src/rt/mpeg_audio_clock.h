#ifndef SR_MPEG_AUDIO_CLOCK_H
#define SR_MPEG_AUDIO_CLOCK_H

#include <stdint.h>

static inline int64_t sr_mpeg_audio_next_pts(
    int64_t current, int64_t pes_pts, int64_t first_pts,
    int64_t frame_step, int first_frame) {
    if (pes_pts >= 0) {
        int64_t relative = pes_pts - first_pts;
        if (first_frame || relative > current) current = relative;
    }
    return current + frame_step;
}

#endif

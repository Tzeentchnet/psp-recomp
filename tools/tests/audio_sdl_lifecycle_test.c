#include "audio.h"

#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sched_fast_forwarding(void) {
    return 0;
}

int main(void) {
    if (setenv("SDL_AUDIODRIVER", "dummy", 1) != 0) {
        perror("setenv SDL_AUDIODRIVER");
        return 2;
    }

    sr_audio_set_suspended(1);
    if (!sr_audio_init()) {
        fprintf(stderr, "FAIL: suspended audio initialization\n");
        return 1;
    }
    if (sr_audio_device_latency_frames() != -1) {
        fprintf(stderr, "FAIL: suspended audio reported latency\n");
        return 1;
    }

    sr_audio_set_suspended(0);
    if (sr_audio_device_latency_frames() <= 0) {
        fprintf(stderr, "FAIL: audio did not resume\n");
        return 1;
    }

    int16_t block[256 * 2];
    memset(block, 0, sizeof(block));
    sr_audio_push(0, block, 256, 32768, 32768);
    SDL_Delay(20);

    sr_audio_set_suspended(1);
    if (sr_audio_queued(0) != -1) {
        fprintf(stderr, "FAIL: suspended audio reported a queue\n");
        return 1;
    }
    sr_audio_set_suspended(0);
    if (sr_audio_device_latency_frames() <= 0) {
        fprintf(stderr, "FAIL: second audio resume\n");
        return 1;
    }

    sr_audio_shutdown();
    puts("SDL audio lifecycle tests passed");
    return 0;
}

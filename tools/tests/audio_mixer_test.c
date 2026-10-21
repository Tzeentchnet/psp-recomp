#include "audio_mixer.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
static SrAudioMixer mixer;

static void check_i64(const char *name, int64_t got, int64_t expected) {
    if (got != expected) {
        fprintf(
            stderr, "FAIL: %s: got %lld, expected %lld\n",
            name, (long long)got, (long long)expected);
        failures++;
    }
}

int main(void) {
    int16_t first[] = {1000, -1000, 2000, -2000};
    int16_t second[] = {3000, 3000, -5000, 5000};
    int16_t output[16];

    sr_audio_mixer_init(&mixer);
    check_i64("initial queue", sr_audio_mixer_queued(&mixer, 0), 0);
    check_i64("initial available", sr_audio_mixer_available(&mixer), 0);
    check_i64(
        "push first", sr_audio_mixer_push(
            &mixer, 0, first, 2, 0x8000, 0x8000), 2);
    check_i64(
        "push second", sr_audio_mixer_push(
            &mixer, 1, second, 2, 0x4000, 0x4000), 2);
    check_i64("queued before render", sr_audio_mixer_queued(&mixer, 0), 2);
    check_i64("available before render", sr_audio_mixer_available(&mixer), 2);
    sr_audio_mixer_render(&mixer, output, 2);
    check_i64("available after render", sr_audio_mixer_available(&mixer), 0);
    check_i64("mixed frame 0 left", output[0], 2500);
    check_i64("mixed frame 0 right", output[1], 500);
    check_i64("mixed frame 1 left", output[2], -500);
    check_i64("mixed frame 1 right", output[3], 500);
    check_i64("queued while submitted", sr_audio_mixer_queued(&mixer, 0), 2);
    sr_audio_mixer_complete(&mixer, 1);
    check_i64("queued after one complete", sr_audio_mixer_queued(&mixer, 0), 1);
    sr_audio_mixer_complete(&mixer, 1);
    check_i64("queued after playback", sr_audio_mixer_queued(&mixer, 0), 0);

    sr_audio_mixer_render(&mixer, output, 4);
    for (int i = 0; i < 8; i++)
        check_i64("silent render", output[i], 0);
    int16_t late[] = {1234, -4321};
    check_i64(
        "late push", sr_audio_mixer_push(
            &mixer, 0, late, 1, 0x8000, 0x8000), 1);
    sr_audio_mixer_render(&mixer, output, 1);
    check_i64("late frame left", output[0], 1234);
    check_i64("late frame right", output[1], -4321);
    check_i64("resynced frame accounting", mixer.stats.resynced_frames, 4);

    sr_audio_mixer_init(&mixer);
    int16_t loud[] = {30000, 30000};
    sr_audio_mixer_push(&mixer, 0, loud, 1, 0x8000, 0x8000);
    sr_audio_mixer_push(&mixer, 1, loud, 1, 0x8000, 0x8000);
    sr_audio_mixer_render(&mixer, output, 1);
    check_i64("positive clamp", output[0], 32767);
    check_i64("positive clamp right", output[1], 32767);

    sr_audio_mixer_init(&mixer);
    mixer.channel_write[0] = SR_AUDIO_RING_FRAMES - 1u;
    check_i64(
        "capacity clamp", sr_audio_mixer_push(
            &mixer, 0, first, 2, 0x8000, 0x8000), 1);
    check_i64("dropped frame", mixer.stats.dropped_frames, 1);
    check_i64("maximum queue", mixer.stats.max_queued_frames, SR_AUDIO_RING_FRAMES);

    const SrAudioMixerStats *stats = sr_audio_mixer_stats(&mixer);
    check_i64("stats accessor", stats->pushed_frames, 1);

    if (failures) {
        fprintf(stderr, "audio mixer tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("audio mixer tests passed\n");
    return 0;
}

#include "audio.h"
#include "audio_mixer.h"

#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sched_fast_forwarding(void);

#define SAMPLE_RATE 44100
#define BLOCK_FRAMES 256

static SrAudioMixer s_mixer;
static SDL_AudioStream *s_stream;
static int s_initialized;
static int s_ok;
static int s_failed;
static int s_log;

static void SDLCALL audio_callback(
    void *userdata, SDL_AudioStream *stream,
    int additional_amount, int total_amount) {
    int16_t block[BLOCK_FRAMES * 2];
    int frames = (additional_amount + 3) / 4;
    (void)userdata;
    (void)total_amount;

    while (frames > 0 && !s_failed) {
        int count = frames < BLOCK_FRAMES ? frames : BLOCK_FRAMES;
        int guest_audio =
            sr_audio_mixer_available(&s_mixer) >= count;
        if (guest_audio)
            sr_audio_mixer_render(&s_mixer, block, count);
        else
            memset(block, 0, (size_t)count * 2u * sizeof(int16_t));
        if (!SDL_PutAudioStreamData(
                stream, block,
                count * 2 * (int)sizeof(int16_t))) {
            fprintf(
                stderr, "audio: SDL_PutAudioStreamData failed: %s\n",
                SDL_GetError());
            s_failed = 1;
            return;
        }
        if (guest_audio)
            sr_audio_mixer_complete(&s_mixer, count);
        frames -= count;
    }
}

int sr_audio_init(void) {
    if (s_initialized) return s_ok;
    s_initialized = 1;
    s_log = getenv("SR_AUDIO_LOG") ? 1 : 0;
    if (getenv("SR_NOAUDIO")) return 0;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "audio: SDL audio initialization failed: %s\n",
                SDL_GetError());
        return 0;
    }
    sr_audio_mixer_init(&s_mixer);
    SDL_AudioSpec format = {
        .format = SDL_AUDIO_S16,
        .channels = 2,
        .freq = SAMPLE_RATE,
    };
    s_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &format, audio_callback, NULL);
    if (!s_stream) {
        fprintf(stderr, "audio: cannot open SDL audio stream: %s\n",
                SDL_GetError());
        return 0;
    }
    if (!SDL_ResumeAudioStreamDevice(s_stream)) {
        fprintf(stderr, "audio: cannot start SDL audio stream: %s\n",
                SDL_GetError());
        SDL_DestroyAudioStream(s_stream);
        s_stream = NULL;
        return 0;
    }
    atexit(sr_audio_shutdown);
    s_ok = 1;
    fprintf(stderr, "audio: SDL 44.1 kHz stereo\n");
    return 1;
}

void sr_audio_push(
    int channel, const int16_t *stereo, int frames,
    int volume_left, int volume_right) {
    if (sched_fast_forwarding()) return;
    if (!sr_audio_init() || frames <= 0 || s_failed) return;
    if (!SDL_LockAudioStream(s_stream)) {
        fprintf(stderr, "audio: cannot lock SDL audio stream: %s\n",
                SDL_GetError());
        s_failed = 1;
        return;
    }
    sr_audio_mixer_push(
        &s_mixer, channel, stereo, frames,
        volume_left, volume_right);
    SDL_UnlockAudioStream(s_stream);
}

int sr_audio_queued(int channel) {
    if (sched_fast_forwarding()) return -1;
    if (!s_ok || s_failed) return -1;
    if (!SDL_LockAudioStream(s_stream)) return -1;
    int queued = sr_audio_mixer_queued(&s_mixer, channel);
    SDL_UnlockAudioStream(s_stream);
    return queued;
}

int sr_audio_device_latency_frames(void) {
    if (sched_fast_forwarding() || !s_ok || s_failed) return -1;
    return BLOCK_FRAMES * 2;
}

const char *sr_audio_backend_name(void) {
    return "SDL";
}

void sr_audio_shutdown(void) {
    if (!s_initialized) return;
    SrAudioMixerStats stats = {0};
    if (s_stream && SDL_LockAudioStream(s_stream)) {
        stats = *sr_audio_mixer_stats(&s_mixer);
        SDL_UnlockAudioStream(s_stream);
    }
    if (s_ok || s_log) {
        fprintf(
            stderr,
            "audio: SDL shutdown played=%llu frames, "
            "max_guest_queue=%llu, dropped=%llu, resynced=%llu, "
            "silent_blocks=%llu%s\n",
            (unsigned long long)stats.completed_frames,
            (unsigned long long)stats.max_queued_frames,
            (unsigned long long)stats.dropped_frames,
            (unsigned long long)stats.resynced_frames,
            (unsigned long long)stats.silent_blocks,
            s_failed ? ", stream_error" : "");
    }
    if (s_stream) SDL_DestroyAudioStream(s_stream);
    s_stream = NULL;
    s_ok = 0;
    s_failed = 0;
    s_initialized = 0;
}

#ifndef PSP_RECOMP_AUDIO_MIXER_H
#define PSP_RECOMP_AUDIO_MIXER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_AUDIO_CHANNELS 9
#define SR_AUDIO_RING_FRAMES (1u << 16)

typedef struct SrAudioMixerStats {
    uint64_t pushed_frames;
    uint64_t dropped_frames;
    uint64_t resynced_frames;
    uint64_t rendered_frames;
    uint64_t completed_frames;
    uint64_t silent_blocks;
    uint64_t max_queued_frames;
} SrAudioMixerStats;

typedef struct SrAudioMixer {
    int32_t left[SR_AUDIO_RING_FRAMES];
    int32_t right[SR_AUDIO_RING_FRAMES];
    uint64_t channel_write[SR_AUDIO_CHANNELS];
    uint8_t channel_started[SR_AUDIO_CHANNELS];
    uint64_t submit;
    uint64_t play;
    SrAudioMixerStats stats;
} SrAudioMixer;

void sr_audio_mixer_init(SrAudioMixer *mixer);
int sr_audio_mixer_push(
    SrAudioMixer *mixer, int channel, const int16_t *stereo,
    int frames, int volume_left, int volume_right);
void sr_audio_mixer_render(
    SrAudioMixer *mixer, int16_t *stereo, int frames);
void sr_audio_mixer_complete(SrAudioMixer *mixer, int frames);
int sr_audio_mixer_queued(const SrAudioMixer *mixer, int channel);
int sr_audio_mixer_available(const SrAudioMixer *mixer);
const SrAudioMixerStats *sr_audio_mixer_stats(const SrAudioMixer *mixer);

#ifdef __cplusplus
}
#endif

#endif

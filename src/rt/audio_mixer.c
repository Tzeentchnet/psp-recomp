#include "audio_mixer.h"

#include <limits.h>
#include <string.h>

#define RING_MASK (SR_AUDIO_RING_FRAMES - 1u)

static int16_t clamp_s16(int32_t value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return (int16_t)value;
}

void sr_audio_mixer_init(SrAudioMixer *mixer) {
    memset(mixer, 0, sizeof(*mixer));
}

int sr_audio_mixer_push(
    SrAudioMixer *mixer, int channel, const int16_t *stereo,
    int frames, int volume_left, int volume_right) {
    if (!stereo || frames <= 0) return 0;
    if (channel < 0) channel = 0;
    if (channel >= SR_AUDIO_CHANNELS) channel = SR_AUDIO_CHANNELS - 1;

    uint64_t write = mixer->channel_write[channel];
    if (write < mixer->submit) {
        if (mixer->channel_started[channel])
            mixer->stats.resynced_frames += mixer->submit - write;
        write = mixer->submit;
    }
    mixer->channel_started[channel] = 1;
    uint64_t limit = mixer->play + SR_AUDIO_RING_FRAMES;
    int accepted = frames;
    if (write >= limit) {
        accepted = 0;
    } else if ((uint64_t)accepted > limit - write) {
        accepted = (int)(limit - write);
    }
    mixer->stats.dropped_frames += (uint64_t)(frames - accepted);

    for (int i = 0; i < accepted; i++) {
        uint32_t index = (uint32_t)(write + (uint64_t)i) & RING_MASK;
        mixer->left[index] +=
            (int32_t)stereo[i * 2] * volume_left >> 15;
        mixer->right[index] +=
            (int32_t)stereo[i * 2 + 1] * volume_right >> 15;
    }
    mixer->channel_write[channel] = write + (uint64_t)accepted;
    mixer->stats.pushed_frames += (uint64_t)accepted;
    uint64_t queued = mixer->channel_write[channel] - mixer->play;
    if (queued > mixer->stats.max_queued_frames)
        mixer->stats.max_queued_frames = queued;
    return accepted;
}

void sr_audio_mixer_render(
    SrAudioMixer *mixer, int16_t *stereo, int frames) {
    if (!stereo || frames <= 0) return;
    int nonzero = 0;
    for (int i = 0; i < frames; i++) {
        uint32_t index = (uint32_t)(mixer->submit + (uint64_t)i) & RING_MASK;
        stereo[i * 2] = clamp_s16(mixer->left[index]);
        stereo[i * 2 + 1] = clamp_s16(mixer->right[index]);
        nonzero |= stereo[i * 2] | stereo[i * 2 + 1];
        mixer->left[index] = 0;
        mixer->right[index] = 0;
    }
    mixer->submit += (uint64_t)frames;
    mixer->stats.rendered_frames += (uint64_t)frames;
    if (!nonzero) mixer->stats.silent_blocks++;
}

void sr_audio_mixer_complete(SrAudioMixer *mixer, int frames) {
    if (frames <= 0) return;
    uint64_t available = mixer->submit - mixer->play;
    uint64_t completed = (uint64_t)frames;
    if (completed > available) completed = available;
    mixer->play += completed;
    mixer->stats.completed_frames += completed;
}

int sr_audio_mixer_queued(const SrAudioMixer *mixer, int channel) {
    if (channel < 0) channel = 0;
    if (channel >= SR_AUDIO_CHANNELS) channel = SR_AUDIO_CHANNELS - 1;
    uint64_t write = mixer->channel_write[channel];
    uint64_t queued = write > mixer->play ? write - mixer->play : 0;
    return queued > INT_MAX ? INT_MAX : (int)queued;
}

int sr_audio_mixer_available(const SrAudioMixer *mixer) {
    uint64_t end = mixer->submit;
    for (int channel = 0; channel < SR_AUDIO_CHANNELS; channel++) {
        if (mixer->channel_started[channel] &&
            mixer->channel_write[channel] > end) {
            end = mixer->channel_write[channel];
        }
    }
    uint64_t available = end - mixer->submit;
    return available > INT_MAX ? INT_MAX : (int)available;
}

const SrAudioMixerStats *sr_audio_mixer_stats(const SrAudioMixer *mixer) {
    return &mixer->stats;
}

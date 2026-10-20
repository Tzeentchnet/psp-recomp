#ifndef PSP_RECOMP_AUDIO_H
#define PSP_RECOMP_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int sr_audio_init(void);
void sr_audio_shutdown(void);
void sr_audio_push(
    int channel, const int16_t *stereo, int frames,
    int volume_left, int volume_right);
int sr_audio_queued(int channel);
int sr_audio_device_latency_frames(void);
const char *sr_audio_backend_name(void);

#ifdef __cplusplus
}
#endif

#endif

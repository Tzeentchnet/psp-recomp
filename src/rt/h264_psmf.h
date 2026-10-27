#ifndef SR_H264_PSMF_H
#define SR_H264_PSMF_H

#include <stdint.h>

typedef struct {
    uint32_t off, len;
    int64_t pts;
} SrPsmfChunk;

typedef struct {
    uint8_t *ps;
    uint32_t psLen, psCap, psPos;
    uint8_t *es;
    uint32_t esLen, esCap, esPos;
    uint8_t *audio;
    uint32_t audioLen, audioCap, audioPos;
    SrPsmfChunk *audioCk;
    uint32_t nAudioCk, capAudioCk;
} SrPsmfDemux;

int sr_psmf_feed(SrPsmfDemux *d, const uint8_t *data, uint32_t len);
int sr_psmf_finish(const SrPsmfDemux *d);
void sr_psmf_compact_video(SrPsmfDemux *d);
void sr_psmf_destroy(SrPsmfDemux *d);
int sr_psmf_audio_available(SrPsmfDemux *d);
int sr_psmf_audio_frame(
    SrPsmfDemux *d, uint8_t *dst, int capacity, int *channels, int64_t *pts);
int sr_h264_find_nal(
    const uint8_t *data, uint32_t length, uint32_t from,
    uint32_t *offset, uint32_t *header);

#endif

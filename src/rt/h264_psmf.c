#include "h264_psmf.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int reserve(uint8_t **data, uint32_t *capacity, uint32_t need) {
    if (need <= *capacity) return 1;
    uint32_t n = *capacity ? *capacity : 65536u;
    while (n < need) {
        if (n > UINT32_MAX / 2u) {
            n = need;
            break;
        }
        n *= 2u;
    }
    uint8_t *p = (uint8_t *)realloc(*data, n);
    if (!p) return 0;
    *data = p;
    *capacity = n;
    return 1;
}

static int append_video(SrPsmfDemux *d, const uint8_t *p, uint32_t n) {
    if (n > UINT32_MAX - d->esLen ||
        !reserve(&d->es, &d->esCap, d->esLen + n)) return 0;
    memcpy(d->es + d->esLen, p, n);
    d->esLen += n;
    return 1;
}

static int append_audio(SrPsmfDemux *d, const uint8_t *p, uint32_t n, int64_t pts) {
    if (n > UINT32_MAX - d->audioLen ||
        !reserve(&d->audio, &d->audioCap, d->audioLen + n)) return 0;
    if (d->nAudioCk == d->capAudioCk) {
        uint32_t capacity = d->capAudioCk ? d->capAudioCk * 2u : 256u;
        if (capacity < d->capAudioCk ||
            (uint64_t)capacity * sizeof(SrPsmfChunk) > SIZE_MAX) return 0;
        SrPsmfChunk *chunks =
            (SrPsmfChunk *)realloc(d->audioCk, (size_t)capacity * sizeof(*chunks));
        if (!chunks) return 0;
        d->audioCk = chunks;
        d->capAudioCk = capacity;
    }
    memcpy(d->audio + d->audioLen, p, n);
    d->audioCk[d->nAudioCk++] = (SrPsmfChunk){d->audioLen, n, pts};
    d->audioLen += n;
    return 1;
}

void sr_psmf_compact_video(SrPsmfDemux *d) {
    if (d->esPos < (1u << 20)) return;
    memmove(d->es, d->es + d->esPos, d->esLen - d->esPos);
    d->esLen -= d->esPos;
    d->esPos = 0;
}

/* 1: consumed, 0: incomplete, -1: malformed or out of memory. */
static int parse_one(SrPsmfDemux *d) {
    const uint8_t *p = d->ps + d->psPos;
    uint32_t n = d->psLen - d->psPos;
    if (n < 4) return 0;
    if (!(p[0] == 0 && p[1] == 0 && p[2] == 1)) {
        uint32_t i = 1;
        while (i + 3 <= n && !(p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)) i++;
        d->psPos += i;
        return d->psLen - d->psPos >= 4;
    }
    uint8_t code = p[3];
    if (code == 0xBA) {
        if (n < 14) return 0;
        uint32_t skip = 14u + (p[13] & 7u);
        if (n < skip) return 0;
        d->psPos += skip;
        return 1;
    }
    if (code < 0xBB || code == 0xB9) {
        d->psPos += 4;
        return 1;
    }
    if (n < 6) return 0;
    uint32_t len = ((uint32_t)p[4] << 8) | p[5];
    uint32_t total = 6u + len;
    if (!len && code >= 0xE0 && code <= 0xEF) return -1;
    if (n < total) return 0;
    if (((code >= 0xE0 && code <= 0xEF) || code == 0xBD) &&
        len >= 3 && (p[6] & 0xC0) == 0x80) {
        uint32_t hdr = 9u + p[8];
        if (hdr > total) return -1;
        int64_t pts = -1;
        if ((p[7] & 0x80) && p[8] >= 5 && len >= 8) {
            pts = ((int64_t)(p[9] >> 1 & 7) << 30) |
                  ((int64_t)p[10] << 22) | ((int64_t)(p[11] >> 1) << 15) |
                  ((int64_t)p[12] << 7) | ((int64_t)(p[13] >> 1));
        }
        if (total > hdr && code >= 0xE0 && code <= 0xEF) {
            if (!append_video(d, p + hdr, total - hdr)) return -1;
        } else if (total > hdr + 4 && code == 0xBD) {
            uint8_t channel = p[hdr];
            uint32_t audio_header = channel >= 0xB0 && channel <= 0xBF ? 5u : 4u;
            if (total > hdr + audio_header &&
                !append_audio(d, p + hdr + audio_header,
                              total - hdr - audio_header, pts)) return -1;
        }
    }
    d->psPos += total;
    return 1;
}

int sr_psmf_feed(SrPsmfDemux *d, const uint8_t *data, uint32_t len) {
    if (!data || !len) return 1;
    if (len > UINT32_MAX - d->psLen ||
        !reserve(&d->ps, &d->psCap, d->psLen + len)) return 0;
    memcpy(d->ps + d->psLen, data, len);
    d->psLen += len;
    int r;
    while ((r = parse_one(d)) > 0) {}
    if (r < 0) return 0;
    if (d->psPos > (1u << 20)) {
        memmove(d->ps, d->ps + d->psPos, d->psLen - d->psPos);
        d->psLen -= d->psPos;
        d->psPos = 0;
    }
    return 1;
}

int sr_psmf_finish(const SrPsmfDemux *d) {
    uint32_t remaining = d->psLen - d->psPos;
    if (remaining < 4) return 1;
    const uint8_t *p = d->ps + d->psPos;
    return !(p[0] == 0 && p[1] == 0 && p[2] == 1);
}

static int audio_frame_size(SrPsmfDemux *d) {
    while (d->audioLen - d->audioPos >= 2 &&
           !(d->audio[d->audioPos] == 0x0F && d->audio[d->audioPos + 1] == 0xD0))
        d->audioPos++;
    if (d->audioLen - d->audioPos < 4) return 0;
    uint32_t size = (((uint32_t)d->audio[d->audioPos + 2] & 3u) << 8) |
                    ((uint32_t)d->audio[d->audioPos + 3] << 3);
    size += 0x10u;
    return d->audioLen - d->audioPos < size ? 0 : (int)size;
}

int sr_psmf_audio_available(SrPsmfDemux *d) {
    return audio_frame_size(d) > 0;
}

int sr_psmf_audio_frame(
    SrPsmfDemux *d, uint8_t *dst, int capacity, int *channels, int64_t *pts) {
    if (!dst || capacity <= 0) return -1;
    int frame_size = audio_frame_size(d);
    if (!frame_size) return 0;
    uint8_t code1 = d->audio[d->audioPos + 2];
    uint32_t payload = (uint32_t)frame_size - 8u;
    if (payload > (uint32_t)capacity) return -1;
    memcpy(dst, d->audio + d->audioPos + 8u, payload);
    if (pts) {
        *pts = -1;
        for (uint32_t i = 0; i < d->nAudioCk; i++) {
            SrPsmfChunk *chunk = &d->audioCk[i];
            if (chunk->off > d->audioPos) break;
            if (d->audioPos < chunk->off + chunk->len && chunk->pts >= 0)
                *pts = chunk->pts;
        }
    }
    d->audioPos += (uint32_t)frame_size;
    if (channels) *channels = code1 == 0x24 ? 1 : 2;
    if (d->audioPos >= (1u << 20)) {
        uint32_t base = d->audioPos, live = 0;
        memmove(d->audio, d->audio + base, d->audioLen - base);
        d->audioLen -= base;
        for (uint32_t i = 0; i < d->nAudioCk; i++) {
            SrPsmfChunk chunk = d->audioCk[i];
            uint32_t end = chunk.off + chunk.len;
            if (end <= base) continue;
            uint32_t start = chunk.off > base ? chunk.off : base;
            chunk.off = start - base;
            chunk.len = end - start;
            d->audioCk[live++] = chunk;
        }
        d->nAudioCk = live;
        d->audioPos = 0;
    }
    return (int)payload;
}

void sr_psmf_destroy(SrPsmfDemux *d) {
    free(d->ps);
    free(d->es);
    free(d->audio);
    free(d->audioCk);
    memset(d, 0, sizeof(*d));
}

int sr_h264_find_nal(
    const uint8_t *data, uint32_t length, uint32_t from,
    uint32_t *offset, uint32_t *header) {
    if (length < 4u || from > length - 4u) return 0;
    for (uint32_t i = from; i <= length - 4u; i++) {
        if (data[i] != 0 || data[i + 1] != 0) continue;
        if (data[i + 2] == 1) {
            *offset = i;
            *header = i + 3u;
            return 1;
        }
        if (i < length - 4u && data[i + 2] == 0 && data[i + 3] == 1) {
            *offset = i;
            *header = i + 4u;
            return 1;
        }
    }
    return 0;
}

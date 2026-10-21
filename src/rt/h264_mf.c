/* H.264 video decode for sceMpeg (PSMF movies) via Windows Media Foundation.
 *
 * The game feeds 2048-byte MPEG-PS packets into the mpeg ring buffer; mpeg.c hands those
 * bytes here. We demux the program stream into H.264 and ATRAC3+ elementary streams, then run
 * video through the OS H.264 decoder MFT (msmpeg2vdec.dll), converting each NV12 frame
 * into the guest video buffer in the PSP pixel format the game asked for.
 *
 * Pull model: sr_h264_feed() only demuxes (cheap); sr_h264_frame() pushes video ES chunks into
 * the MFT until it yields one frame, while sr_h264_audio_frame() returns complete ATRAC3+
 * frames. This keeps the decoder queues bounded to work requested by sceMpegAvcDecode/AtracDecode.
 *
 * No external dependencies: mfplat.dll / msmpeg2vdec.dll ship with Windows.
 */

#define _CRT_SECURE_NO_WARNINGS
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* GUIDs defined locally (avoids depending on which GUIDs this mingw's mfuuid.a carries). */
static const GUID L_CLSID_CMSH264Decoder = {0x62CE7E72,0x4C71,0x4D20,{0xB1,0x5D,0x45,0x28,0x31,0xA8,0x7D,0x9D}};
static const GUID L_IID_IMFTransform    = {0xbf94c121,0x5b05,0x4e6f,{0x80,0x00,0xba,0x59,0x89,0x61,0x41,0x4d}};
static const GUID L_MFMediaType_Video   = {0x73646976,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID L_MFVideoFormat_H264  = {0x34363248,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID L_MFVideoFormat_NV12  = {0x3231564E,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};
static const GUID L_MF_MT_MAJOR_TYPE    = {0x48eba18e,0xf8c9,0x4687,{0xbf,0x11,0x0a,0x74,0xc9,0xf9,0x6a,0x8f}};
static const GUID L_MF_MT_SUBTYPE       = {0xf7e34c9a,0x42e8,0x4714,{0xb7,0x4b,0xcb,0x29,0xd7,0x2c,0x35,0xe5}};
static const GUID L_MF_MT_FRAME_SIZE    = {0x1652c33d,0xd6b2,0x4012,{0xb8,0x34,0x72,0x03,0x08,0x49,0xa3,0x7d}};
static const GUID L_MF_MT_FRAME_RATE    = {0xc459a2e8,0x3d2c,0x4e44,{0xb1,0x32,0xfe,0xe5,0x15,0x6c,0x7b,0xb0}};
static const GUID L_MF_MT_DEFAULT_STRIDE= {0x644b4e48,0x1e02,0x4516,{0xb0,0xeb,0xc0,0x1c,0xa9,0xd4,0x9a,0xc6}};

#ifndef MF_E_TRANSFORM_NEED_MORE_INPUT
#define MF_E_TRANSFORM_NEED_MORE_INPUT ((HRESULT)0xC00D6D72L)
#endif
#ifndef MF_E_TRANSFORM_STREAM_CHANGE
#define MF_E_TRANSFORM_STREAM_CHANGE ((HRESULT)0xC00D6D61L)
#endif
#ifndef MF_E_NOTACCEPTING
#define MF_E_NOTACCEPTING ((HRESULT)0xC00D36B5L)
#endif

typedef struct { uint32_t off, len; int64_t pts; } EsChunk;

typedef struct {
    int used;
    IMFTransform *xf;
    int outW, outH, outStride;
    int spsSet, ppsSet;
    uint64_t submittedFrames, returnedFrames;
    int failed;
    int drained;
    /* MPEG-PS input accumulation (unparsed tail) */
    uint8_t *ps; uint32_t psLen, psCap, psPos;
    /* demuxed H.264 ES byte fifo + chunk table (one chunk per video PES payload) */
    uint8_t *es; uint32_t esLen, esCap;
    EsChunk *ck; uint32_t nCk, capCk, esPos;
    uint8_t *audio; uint32_t audioLen, audioCap, audioPos;
    EsChunk *audioCk; uint32_t nAudioCk, capAudioCk;
    LONGLONG fakeTime;             /* monotonic 100 ns timestamp for frame-aligned NAL samples */
} Dec;

#define MAX_DEC 4
static Dec s_dec[MAX_DEC];
static int s_mf_state = 0;         /* 0 = not tried, 1 = up, -1 = failed */

static int mflog(void) { static int v = -1; if (v < 0) v = getenv("SR_MPEGLOG") ? 1 : 0; return v; }

/* ---- buffers -------------------------------------------------------------------------- */
static int buf_reserve(uint8_t **d, uint32_t *cap, uint32_t need) {
    if (need <= *cap) return 1;
    uint32_t n = *cap ? *cap : 65536;
    while (n < need) n *= 2;
    uint8_t *p = (uint8_t *)realloc(*d, n);
    if (!p) return 0;
    *d = p; *cap = n;
    return 1;
}

static void es_append(Dec *d, const uint8_t *p, uint32_t n, int64_t pts) {
    if (!buf_reserve(&d->es, &d->esCap, d->esLen + n)) return;
    if (d->nCk == d->capCk) {
        uint32_t c = d->capCk ? d->capCk * 2 : 256;
        EsChunk *t = (EsChunk *)realloc(d->ck, c * sizeof(EsChunk));
        if (!t) return;
        d->ck = t; d->capCk = c;
    }
    memcpy(d->es + d->esLen, p, n);
    d->ck[d->nCk].off = d->esLen;
    d->ck[d->nCk].len = n;
    d->ck[d->nCk].pts = pts;
    d->nCk++;
    d->esLen += n;
}

static void audio_append(Dec *d, const uint8_t *p, uint32_t n, int64_t pts) {
    if (!buf_reserve(&d->audio, &d->audioCap, d->audioLen + n)) return;
    if (d->nAudioCk == d->capAudioCk) {
        uint32_t capacity = d->capAudioCk ? d->capAudioCk * 2u : 256u;
        EsChunk *chunks =
            (EsChunk *)realloc(d->audioCk, capacity * sizeof(EsChunk));
        if (!chunks) return;
        d->audioCk = chunks;
        d->capAudioCk = capacity;
    }
    memcpy(d->audio + d->audioLen, p, n);
    d->audioCk[d->nAudioCk].off = d->audioLen;
    d->audioCk[d->nAudioCk].len = n;
    d->audioCk[d->nAudioCk].pts = pts;
    d->nAudioCk++;
    d->audioLen += n;
}

/* Drop consumed bytes so the fifos stay small (the ring is ~1.2MB; the game feeds as we drain). */
static void es_compact(Dec *d) {
    if (!d->esPos || d->esPos < (1u << 20)) return;
    uint32_t base = d->esPos;
    memmove(d->es, d->es + base, d->esLen - base);
    d->esLen -= base;
    uint32_t live = 0;
    for (uint32_t i = 0; i < d->nCk; i++) {
        uint32_t end = d->ck[i].off + d->ck[i].len;
        if (end <= base) continue;
        uint32_t start = d->ck[i].off > base ? d->ck[i].off : base;
        d->ck[live] = d->ck[i];
        d->ck[live].off = start - base;
        d->ck[live].len = end - start;
        live++;
    }
    d->nCk = live;
    d->esPos = 0;
}

/* ---- MPEG program-stream demux --------------------------------------------------------- */
/* Parse one syntactic element at psPos. Returns 1 if it consumed something, 0 if it needs
 * more bytes. Video PES payloads (stream id 0xE0-0xEF) are appended to the ES fifo with the
 * PES PTS (90 kHz) when present, -1 otherwise. */
static int ps_parse_one(Dec *d) {
    const uint8_t *p = d->ps + d->psPos;
    uint32_t n = d->psLen - d->psPos;
    if (n < 4) return 0;
    /* resync to a start code */
    if (!(p[0] == 0 && p[1] == 0 && p[2] == 1)) {
        uint32_t i = 1;
        while (i + 3 <= n && !(p[i] == 0 && p[i+1] == 0 && p[i+2] == 1)) i++;
        d->psPos += i;
        return (d->psLen - d->psPos) >= 4;
    }
    uint8_t code = p[3];
    if (code == 0xBA) {                          /* pack header: 14 bytes + stuffing */
        if (n < 14) return 0;
        uint32_t skip = 14 + (p[13] & 7);
        if (n < skip) return 0;
        d->psPos += skip;
        return 1;
    }
    if (code == 0xB9) { d->psPos += 4; return 1; }   /* program end */
    if (code < 0xBB) { d->psPos += 4; return 1; }    /* unexpected: skip the start code */
    if (n < 6) return 0;
    uint32_t len = ((uint32_t)p[4] << 8) | p[5];
    uint32_t tot = 6 + len;
    if (n < tot) return 0;
    if (((code >= 0xE0 && code <= 0xEF) || code == 0xBD) &&
        len >= 3 && (p[6] & 0xC0) == 0x80) {
        uint32_t hdr = 9 + p[8];
        int64_t pts = -1;
        if ((p[7] & 0x80) && p[8] >= 5 && len >= 8) {
            pts = ((int64_t)(p[9]  >> 1 & 7) << 30) |
                  ((int64_t) p[10]          << 22) |
                  ((int64_t)(p[11] >> 1)    << 15) |
                  ((int64_t) p[12]          << 7)  |
                  ((int64_t)(p[13] >> 1));
        }
        if (tot > hdr && code >= 0xE0 && code <= 0xEF) {
            es_append(d, p + hdr, tot - hdr, pts);
        } else if (tot > hdr + 4 && code == 0xBD) {
            uint8_t channel = p[hdr];
            uint32_t audio_header =
                channel >= 0xB0 && channel <= 0xBF ? 5u : 4u;
            if (tot > hdr + audio_header)
                audio_append(
                    d, p + hdr + audio_header,
                    tot - hdr - audio_header, pts);
        }
    }
    d->psPos += tot;
    return 1;
}

/* ---- Media Foundation ------------------------------------------------------------------- */
static int mf_up(void) {
    if (s_mf_state) return s_mf_state > 0;
    s_mf_state = -1;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return 0;
    if (FAILED(MFStartup(0x00020070 /* MF_VERSION */, MFSTARTUP_LITE))) return 0;
    s_mf_state = 1;
    return 1;
}

static int negotiate_output(Dec *d) {
    for (DWORD i = 0; ; i++) {
        IMFMediaType *t = NULL;
        if (FAILED(IMFTransform_GetOutputAvailableType(d->xf, 0, i, &t)) || !t) break;
        GUID sub;
        if (SUCCEEDED(IMFMediaType_GetGUID(t, &L_MF_MT_SUBTYPE, &sub)) &&
            IsEqualGUID(&sub, &L_MFVideoFormat_NV12) &&
            SUCCEEDED(IMFTransform_SetOutputType(d->xf, 0, t, 0))) {
            UINT64 fs = 0;
            if (SUCCEEDED(IMFMediaType_GetUINT64(t, &L_MF_MT_FRAME_SIZE, &fs))) {
                d->outW = (int)(fs >> 32);
                d->outH = (int)(UINT32)fs;
            }
            UINT32 st = 0;
            d->outStride = (SUCCEEDED(IMFMediaType_GetUINT32(t, &L_MF_MT_DEFAULT_STRIDE, &st)) && (int)st > 0)
                           ? (int)st : d->outW;
            IMFMediaType_Release(t);
            if (mflog()) fprintf(stderr, "h264: output %dx%d stride %d\n", d->outW, d->outH, d->outStride);
            return 1;
        }
        IMFMediaType_Release(t);
    }
    return 0;
}

static int dec_open(Dec *d) {
    if (!mf_up()) return 0;
    HRESULT hr = CoCreateInstance(&L_CLSID_CMSH264Decoder, NULL, CLSCTX_INPROC_SERVER,
                                  &L_IID_IMFTransform, (void **)&d->xf);
    if (FAILED(hr) || !d->xf) {
        fprintf(stderr, "h264: H.264 decoder MFT unavailable (hr=0x%08lx)\n", (unsigned long)hr);
        return 0;
    }
    IMFMediaType *mt = NULL;
    if (FAILED(MFCreateMediaType(&mt))) return 0;
    IMFMediaType_SetGUID(mt, &L_MF_MT_MAJOR_TYPE, &L_MFMediaType_Video);
    IMFMediaType_SetGUID(mt, &L_MF_MT_SUBTYPE, &L_MFVideoFormat_H264);
    IMFMediaType_SetUINT64(
        mt, &L_MF_MT_FRAME_SIZE,
        ((UINT64)480u << 32) | (UINT64)272u);
    IMFMediaType_SetUINT64(
        mt, &L_MF_MT_FRAME_RATE,
        ((UINT64)30000u << 32) | (UINT64)1001u);
    hr = IMFTransform_SetInputType(d->xf, 0, mt, 0);
    IMFMediaType_Release(mt);
    if (FAILED(hr)) {
        fprintf(stderr, "h264: SetInputType(H264) failed (hr=0x%08lx)\n", (unsigned long)hr);
        return 0;
    }
    negotiate_output(d);   /* may only succeed after the first stream change; that's fine */
    IMFTransform_ProcessMessage(d->xf, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(d->xf, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return 1;
}

static int find_nal_start(
    const uint8_t *data, uint32_t length, uint32_t from,
    uint32_t *offset, uint32_t *header) {
    for (uint32_t i = from; i + 3u < length; i++) {
        if (data[i] != 0 || data[i + 1] != 0) continue;
        if (data[i + 2] == 1) {
            *offset = i;
            *header = i + 3u;
            return 1;
        }
        if (i + 4u < length && data[i + 2] == 0 && data[i + 3] == 1) {
            *offset = i;
            *header = i + 4u;
            return 1;
        }
    }
    return 0;
}

/* Feed one Annex-B NAL per sample. The Microsoft decoder establishes SPS/PPS state from
 * parameter-set samples before accepting IDR/P slices; combining all of them behind one
 * AUD made it negotiate dimensions but conceal the pictures. */
static int feed_one(Dec *d, int eos) {
    uint32_t begin = 0, header = 0;
    if (!find_nal_start(
            d->es, d->esLen, d->esPos, &begin, &header)) {
        return 0;
    }
    uint32_t end = 0, next_header = 0;
    if (!find_nal_start(
            d->es, d->esLen, header + 1u, &end, &next_header)) {
        if (!eos) return 0;
        end = d->esLen;
    }
    if (end <= begin) return -1;
    uint32_t length = end - begin;
    uint8_t nal_type = d->es[header] & 31u;
    int vcl = nal_type == 1u || nal_type == 5u;
    if (nal_type == 9u ||
        (nal_type == 8u && !d->spsSet) ||
        ((nal_type != 7u && nal_type != 8u) &&
         (!d->spsSet || !d->ppsSet))) {
        d->esPos = end;
        es_compact(d);
        return 1;
    }
    IMFSample *smp = NULL; IMFMediaBuffer *mb = NULL;
    BYTE *base = NULL;
    int ret = -1;
    if (FAILED(MFCreateMemoryBuffer(length, &mb))) return -1;
    if (SUCCEEDED(IMFMediaBuffer_Lock(mb, &base, NULL, NULL))) {
        memcpy(base, d->es + begin, length);
        IMFMediaBuffer_Unlock(mb);
        IMFMediaBuffer_SetCurrentLength(mb, length);
        if (SUCCEEDED(MFCreateSample(&smp))) {
            IMFSample_AddBuffer(smp, mb);
            LONGLONG t = d->fakeTime;
            IMFSample_SetSampleTime(smp, t);
            IMFSample_SetSampleDuration(smp, 333667);
            if (nal_type == 5u)
                IMFSample_SetUINT32(
                    smp, &MFSampleExtension_CleanPoint, TRUE);
            HRESULT hr = IMFTransform_ProcessInput(d->xf, 0, smp, 0);
            if (SUCCEEDED(hr)) {
                d->esPos = end;
                if (nal_type == 7u) d->spsSet = 1;
                if (nal_type == 8u) d->ppsSet = 1;
                if (vcl) {
                    d->fakeTime += 333667;
                    d->submittedFrames++;
                }
                es_compact(d);
                ret = 1;
            }
            else ret = (hr == MF_E_NOTACCEPTING) ? 0 : -1;
            IMFSample_Release(smp);
        }
    }
    IMFMediaBuffer_Release(mb);
    return ret;
}

static uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

/* NV12 -> PSP pixel format (BT.601 limited range). dst is the guest video buffer (host ptr),
 * frameWidth its stride in pixels; PSP movie buffers are 272 rows tall. */
static int convert_frame(Dec *d, const uint8_t *src, uint32_t srcLen,
                         uint8_t *dst, int frameWidth, int pixelMode) {
    int stride = d->outStride > 0 ? d->outStride : d->outW;
    int w = d->outW, h = d->outH;
    if (stride <= 0 || w <= 0 || h <= 0 ||
        (uint64_t)(uint32_t)stride * (uint32_t)h * 3u / 2u > srcLen) {
        return 0;
    }
    if (w > frameWidth) w = frameWidth;
    if (h > 272) h = 272;
    const uint8_t *yp = src, *uvp = src + (size_t)stride * d->outH;
    for (int y = 0; y < h; y++) {
        const uint8_t *yr = yp + (size_t)y * stride;
        const uint8_t *uv = uvp + (size_t)(y / 2) * stride;
        uint8_t *o8 = dst + (size_t)y * frameWidth * 4;
        uint16_t *o16 = (uint16_t *)(dst + (size_t)y * frameWidth * 2);
        for (int x = 0; x < w; x++) {
            int c = yr[x] - 16, dd = uv[x & ~1] - 128, e = uv[(x & ~1) + 1] - 128;
            int r = clamp8((298 * c + 409 * e + 128) >> 8);
            int g = clamp8((298 * c - 100 * dd - 208 * e + 128) >> 8);
            int b = clamp8((298 * c + 516 * dd + 128) >> 8);
            switch (pixelMode) {
                case 0:  /* 5650 */
                    o16[x] = (uint16_t)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
                    break;
                case 1:  /* 5551 */
                    o16[x] = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000);
                    break;
                case 2:  /* 4444 */
                    o16[x] = (uint16_t)((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | 0xF000);
                    break;
                default: /* 8888: R,G,B,A byte order in guest memory */
                    o8[x * 4 + 0] = (uint8_t)r;
                    o8[x * 4 + 1] = (uint8_t)g;
                    o8[x * 4 + 2] = (uint8_t)b;
                    o8[x * 4 + 3] = 0xFF;
                    break;
            }
        }
    }
    return 1;
}

static void release_output_objects(
    IMFSample *provided_sample, IMFMediaBuffer *provided_buffer,
    IMFSample *output_sample) {
    if (output_sample && output_sample != provided_sample)
        IMFSample_Release(output_sample);
    if (provided_sample) IMFSample_Release(provided_sample);
    if (provided_buffer) IMFMediaBuffer_Release(provided_buffer);
}

/* Try to pull one decoded frame. 1 = frame written to dst, 0 = need more input, -1 = failure. */
static int pump_out(Dec *d, uint8_t *dst, int frameWidth, int pixelMode) {
    int discarded = 0;
    for (;;) {
        MFT_OUTPUT_STREAM_INFO si;
        memset(&si, 0, sizeof(si));
        if (FAILED(IMFTransform_GetOutputStreamInfo(d->xf, 0, &si))) return -1;
        DWORD cb =
            si.cbSize ? si.cbSize :
            (DWORD)((d->outW > 0 ? d->outW : 512) *
                    (d->outH > 0 ? d->outH : 288) * 3 / 2 + 4096);
        int provides_samples =
            (si.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        IMFSample *provided_sample = NULL;
        IMFMediaBuffer *provided_buffer = NULL;
        if (!provides_samples) {
            if (FAILED(MFCreateSample(&provided_sample))) return -1;
            if (FAILED(MFCreateMemoryBuffer(cb, &provided_buffer))) {
                IMFSample_Release(provided_sample);
                return -1;
            }
            if (FAILED(IMFSample_AddBuffer(
                    provided_sample, provided_buffer))) {
                release_output_objects(
                    provided_sample, provided_buffer, NULL);
                return -1;
            }
        }
        MFT_OUTPUT_DATA_BUFFER ob;
        memset(&ob, 0, sizeof(ob));
        ob.pSample = provided_sample;
        DWORD status = 0;
        HRESULT hr = IMFTransform_ProcessOutput(d->xf, 0, 1, &ob, &status);
        if (ob.pEvents) IUnknown_Release((IUnknown *)ob.pEvents);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            release_output_objects(
                provided_sample, provided_buffer, ob.pSample);
            if (!negotiate_output(d)) return -1;
            continue;                       /* output size may have changed: retry */
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            release_output_objects(
                provided_sample, provided_buffer, ob.pSample);
            return 0;
        }
        if (FAILED(hr)) {
            release_output_objects(
                provided_sample, provided_buffer, ob.pSample);
            if (mflog()) fprintf(stderr, "h264: ProcessOutput hr=0x%08lx\n", (unsigned long)hr);
            return -1;
        }
        IMFSample *output_sample = ob.pSample;
        if (!output_sample) {
            release_output_objects(
                provided_sample, provided_buffer, output_sample);
            if (mflog()) fprintf(stderr, "h264: ProcessOutput returned no sample\n");
            return -1;
        }
        if (d->returnedFrames >= d->submittedFrames) {
            if (mflog() && d->returnedFrames < 4u) {
                fprintf(
                    stderr,
                    "h264: discarded pre-VCL output #%llu\n",
                    (unsigned long long)(d->returnedFrames + 1u));
            }
            release_output_objects(
                provided_sample, provided_buffer, output_sample);
            if (++discarded >= 8) return 0;
            continue;
        }
        if (mflog()) {
            static int logged;
            if (!logged) {
                fprintf(
                    stderr,
                    "h264: output flags=0x%lx cb=%lu sample=%s\n",
                    (unsigned long)si.dwFlags, (unsigned long)si.cbSize,
                    output_sample == provided_sample ? "caller" : "decoder");
                logged = 1;
            }
        }
        int converted = dst == NULL;
        if (dst) {
            IMFMediaBuffer *cbuf = NULL;
            if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(
                    output_sample, &cbuf)) && cbuf) {
                BYTE *base = NULL; DWORD cur = 0;
                if (SUCCEEDED(IMFMediaBuffer_Lock(cbuf, &base, NULL, &cur))) {
                    converted =
                        convert_frame(
                            d, base, cur, dst, frameWidth, pixelMode);
                    IMFMediaBuffer_Unlock(cbuf);
                }
                IMFMediaBuffer_Release(cbuf);
            }
        }
        if (!converted) {
            release_output_objects(
                provided_sample, provided_buffer, output_sample);
            if (mflog())
                fprintf(stderr, "h264: could not convert decoded output frame\n");
            return -1;
        }
        d->returnedFrames++;
        release_output_objects(
            provided_sample, provided_buffer, output_sample);
        return 1;
    }
}

/* ================= public API (called from mpeg.c) ================= */

int sr_h264_create(void) {
    if (getenv("SR_NOH264")) return -1;
    for (int i = 0; i < MAX_DEC; i++) if (!s_dec[i].used) {
        Dec *d = &s_dec[i];
        memset(d, 0, sizeof(*d));
        d->used = 1;
        if (!dec_open(d)) {
            if (d->xf) { IMFTransform_Release(d->xf); d->xf = NULL; }
            d->used = 0;
            return -1;
        }
        if (mflog()) fprintf(stderr, "h264: decoder %d open\n", i);
        return i;
    }
    return -1;
}

void sr_h264_destroy(int id) {
    if (id < 0 || id >= MAX_DEC || !s_dec[id].used) return;
    Dec *d = &s_dec[id];
    if (d->xf) IMFTransform_Release(d->xf);
    free(d->ps); free(d->es); free(d->ck);
    free(d->audio); free(d->audioCk);
    memset(d, 0, sizeof(*d));
}

void sr_h264_feed(int id, const uint8_t *data, uint32_t len) {
    if (id < 0 || id >= MAX_DEC || !s_dec[id].used || !data || !len) return;
    Dec *d = &s_dec[id];
    if (d->failed) return;
    if (!buf_reserve(&d->ps, &d->psCap, d->psLen + len)) return;
    memcpy(d->ps + d->psLen, data, len);
    d->psLen += len;
    while (ps_parse_one(d)) {}
    if (d->psPos > (1u << 20)) {            /* drop the parsed prefix */
        memmove(d->ps, d->ps + d->psPos, d->psLen - d->psPos);
        d->psLen -= d->psPos;
        d->psPos = 0;
    }
}

static int audio_frame_size(Dec *d) {
    while (d->audioPos + 2 <= d->audioLen &&
           !(d->audio[d->audioPos] == 0x0F &&
             d->audio[d->audioPos + 1] == 0xD0))
        d->audioPos++;
    if (d->audioLen - d->audioPos < 4) return 0;
    uint8_t code1 = d->audio[d->audioPos + 2];
    uint8_t code2 = d->audio[d->audioPos + 3];
    uint32_t frame_size =
        (((uint32_t)code1 & 3u) << 8) | ((uint32_t)code2 << 3);
    frame_size += 0x10u;
    if (frame_size < 8 || d->audioLen - d->audioPos < frame_size) return 0;
    return (int)frame_size;
}

int sr_h264_audio_available(int id) {
    if (id < 0 || id >= MAX_DEC || !s_dec[id].used) return 0;
    return audio_frame_size(&s_dec[id]) > 0;
}

/* Return one demuxed PSP ATRAC3+ frame, without its 8-byte 0F D0 frame header. */
int sr_h264_audio_frame(
    int id, uint8_t *dst, int capacity, int *channels, int64_t *pts) {
    if (id < 0 || id >= MAX_DEC || !s_dec[id].used || !dst || capacity <= 0)
        return -1;
    Dec *d = &s_dec[id];
    int frame_size_int = audio_frame_size(d);
    if (frame_size_int <= 0) return frame_size_int;
    uint32_t frame_size = (uint32_t)frame_size_int;
    uint8_t code1 = d->audio[d->audioPos + 2];
    uint32_t payload = frame_size - 8u;
    if (payload > (uint32_t)capacity) return -1;

    memcpy(dst, d->audio + d->audioPos + 8u, payload);
    if (pts) {
        *pts = -1;
        for (uint32_t i = 0; i < d->nAudioCk; i++) {
            if (d->audioCk[i].off > d->audioPos) break;
            if (d->audioPos < d->audioCk[i].off + d->audioCk[i].len &&
                d->audioCk[i].pts >= 0)
                *pts = d->audioCk[i].pts;
        }
    }
    d->audioPos += frame_size;
    if (channels) *channels = code1 == 0x24 ? 1 : 2;
    return (int)payload;
}

/* Decode the next frame into dst (host pointer to the guest video buffer). eos != 0 once the
 * game has fed the whole movie, so the MFT gets drained for the last buffered frames.
 * Returns 1 if a frame was written, 0 if none is available yet, -1 if decoding failed. */
int sr_h264_frame(int id, int eos, uint8_t *dst, int frameWidth, int pixelMode) {
    if (id < 0 || id >= MAX_DEC || !s_dec[id].used) return -1;
    Dec *d = &s_dec[id];
    if (d->failed || !d->xf) return -1;
    for (int guard = 0; guard < 4096; guard++) {
        int r = pump_out(d, dst, frameWidth, pixelMode);
        if (r > 0) return 1;
        if (r < 0) { d->failed = 1; return -1; }
        if (d->esPos < d->esLen) {
            int f = feed_one(d, eos);
            if (f < 0) { d->failed = 1; return -1; }
            if (f == 0) return 0;           /* MFT full but no output: shouldn't happen */
        } else if (eos && !d->drained) {
            d->drained = 1;
            IMFTransform_ProcessMessage(d->xf, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            IMFTransform_ProcessMessage(d->xf, MFT_MESSAGE_COMMAND_DRAIN, 0);
        } else {
            return 0;                       /* out of compressed data: wait for more feed */
        }
    }
    return 0;
}

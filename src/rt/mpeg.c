/* sceMpeg (PSMF video) — ported from PPSSPP (Core/HLE/sceMpeg.cpp, rev 4e109dd6, GPLv2+).
 *
 * This is a faithful port of PPSSPP's sceMpeg control flow: PSMF header analysis, ring-buffer
 * packet accounting, MPEG handle/context creation, stream registration, and the access-unit
 * getters with timestamp progression and end-of-stream detection. Windows GPU builds demux the
 * program stream into Media Foundation H.264 video and standalone ATRAC3+ audio; timestamp-only
 * operation remains the fallback for renderer-less tools.
 *
 * Because the project links the recompiled game (GPLv2+ via this port), this binds to GPLv2+.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#include "atrac_decode.h"
#if defined(SR_SDL3VK) || defined(SR_D3D12)
/* Media Foundation H.264 decode (h264_mf.c): real movie frames for Windows GPU builds. */
int  sr_h264_create(void);
void sr_h264_destroy(int id);
void sr_h264_feed(int id, const uint8_t *data, uint32_t len);
int  sr_h264_frame(int id, int eos, uint8_t *dst, int frameWidth, int pixelMode);
int  sr_h264_audio_available(int id);
int  sr_h264_audio_frame(
    int id, uint8_t *dst, int capacity, int *channels, int64_t *pts);
#endif
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "sr_env.h"

/* ---- constants (PPSSPP sceMpeg.cpp/.h) ---- */
#define PSMF_MAGIC                 0x464D5350u
#define PSMF_STREAM_VERSION_OFFSET 0x4
#define PSMF_STREAM_OFFSET_OFFSET  0x8
#define PSMF_STREAM_SIZE_OFFSET    0xC
#define PSMF_FIRST_TIMESTAMP_OFFSET 0x54
#define PSMF_LAST_TIMESTAMP_OFFSET  0x5A

#define MPEG_AVC_STREAM   0
#define MPEG_ATRAC_STREAM 1
#define MPEG_PCM_STREAM   2
#define MPEG_AUDIO_STREAM 15
#define MPEG_DATA_STREAM  16

#define MPEG_AVC_ES_SIZE   2048
#define MPEG_ATRAC_ES_SIZE 2112
#define MPEG_ATRAC_ES_OUTPUT_SIZE 8192
#define MPEG_ATRAC_SAMPLES_PER_FRAME 2048
#define MPEG_ATRAC_CODEC_PLUS 0x1000

#define MPEG_MEMSIZE_0105 0x10000u

#define SCE_MPEG_ERROR_INVALID_VALUE 0x806100FEu
#define SCE_MPEG_ERROR_BAD_VERSION   0x806100A0u
#define SCE_MPEG_ERROR_NO_DATA       0x80618001u
#define SCE_MPEG_ERROR_NO_MEMORY     0x80610022u

static const int videoTimestampStep = 3003;   /* mpegTimestampPerSecond / 29.97 */
static const int audioTimestampStep = 4180;   /* 2048 samples / 44100 Hz */
static const int64_t mpegTimestampPerSecond = 90000;

/* ---- ring-buffer field offsets (SceMpegRingBuffer, all 32-bit) ---- */
enum {
    RB_packets = 0, RB_packetsRead = 4, RB_packetsWritePos = 8, RB_packetsAvail = 12,
    RB_packetSize = 16, RB_data = 20, RB_callback_addr = 24, RB_callback_args = 28,
    RB_dataUpperBound = 32, RB_semaID = 36, RB_mpeg = 40, RB_gp = 44,
};
static uint32_t rb_get(uint32_t ring, int f) { return MEM_R32(ring + (uint32_t)f); }
static void rb_set(uint32_t ring, int f, uint32_t v) { MEM_W32(ring + (uint32_t)f, v); }

/* ---- host-side MPEG context, keyed by guest mpeg handle ---- */
typedef struct {
    int used;
    uint32_t handle;            /* the value stored at *mpegAddr (dataPtr+0x30) */
    uint32_t ringAddr;
    uint32_t magic, rawVersion; int version;
    uint32_t offset, streamSize;
    int64_t firstTimestamp, lastTimestamp;
    int avcRegistered, atracRegistered;
    int isAnalyzed;
    int64_t videoPts, audioPts;  /* media-engine timestamp model */
    int videoEnd, audioEnd, videoTailBuffered;
    uint32_t totalPackets;       /* streamSize/packetSize: whole-movie packet count */
    uint32_t fedPackets;         /* cumulative packets the game has put into the ring */
    uint32_t consumedPackets;    /* packets consumed by timestamp-proportional demux */
    uint32_t headerAddr;         /* guest address of the PSMF header passed to QueryStreamOffset */
    int h264;                    /* Windows GPU build: PSMF demux/H.264 decoder id (-1 = none) */
    int h264Init, h264Frames;
    SrAtracDecoder *pmfAtrac;
    int pmfAtracChannels, pmfAtracBlock, pmfAudioFrames;
    int64_t maxAvDrift;
    int defaultFrameWidth, pixelMode;
    int esBuffers[2];           /* MPEG_DATA_ES_BUFFERS: allocated-flag per ES buffer */
    /* stream map: small fixed table sid -> (type,num,needsReset) */
    struct { int used, type, num, needsReset; uint32_t sid; } streams[8];
} Mpeg;

static Mpeg s_mpeg[8];
static int s_mpegInit = 0;
static uint32_t s_streamIdGen = 1;

static Mpeg *mpeg_find(uint32_t mpegAddr) {
    if (!mpegAddr) return 0;
    uint32_t h = MEM_R32(mpegAddr);
    for (int i = 0; i < 8; i++) if (s_mpeg[i].used && s_mpeg[i].handle == h) return &s_mpeg[i];
    return 0;
}

static uint32_t call_guest3(CpuState *s, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    if (!s || !fn) return 0;
    CpuState save;
    memcpy(&save, s, sizeof(CpuState));
    int32_t save_slice = sr_timeslice;
    memset(s, 0, sizeof(CpuState));
    s->r[4] = a0;
    s->r[5] = a1;
    s->r[6] = a2;
    s->r[28] = save.r[28];
    s->r[29] = sr_hle_stack_top(1);
    s->r[31] = 0;
    s->vfpuCtrl[0] = 0xe4; s->vfpuCtrl[1] = 0xe4;
    sr_timeslice = 20000;
    dispatch(s, fn);
    uint32_t ret = s->r[2];
    memcpy(s, &save, sizeof(CpuState));
    sr_timeslice = save_slice;
    return ret;
}

/* big-endian 32 read from guest memory (PSMF header is big-endian) */
static uint32_t be32(uint32_t a) {
    return ((uint32_t)MEM_R8(a) << 24) | ((uint32_t)MEM_R8(a+1) << 16) | ((uint32_t)MEM_R8(a+2) << 8) | MEM_R8(a+3);
}
/* PSMF 6-byte timestamp, matching PPSSPP MediaEngine.h::getMpegTimeStamp(). */
static int64_t mpeg_ts(uint32_t a) {
    return (int64_t)MEM_R8(a + 5) | ((int64_t)MEM_R8(a + 4) << 8) |
           ((int64_t)MEM_R8(a + 3) << 16) | ((int64_t)MEM_R8(a + 2) << 24) |
           ((int64_t)MEM_R8(a + 1) << 32) | ((int64_t)MEM_R8(a) << 36);
}

/* MPEG-2 pack SCR_base (90 kHz). */
static int64_t mpeg_scr(uint32_t a) {
    if (MEM_R8(a) != 0 || MEM_R8(a + 1) != 0 || MEM_R8(a + 2) != 1 ||
        MEM_R8(a + 3) != 0xba) {
        return -1;
    }
    uint8_t b0 = MEM_R8(a + 4), b1 = MEM_R8(a + 5), b2 = MEM_R8(a + 6);
    uint8_t b3 = MEM_R8(a + 7), b4 = MEM_R8(a + 8);
    if ((b0 & 0xc4) != 0x44 || (b2 & 0x04) == 0 || (b4 & 0x04) == 0) return -1;
    return ((int64_t)(b0 & 0x38) << 27) | ((int64_t)(b0 & 0x03) << 28) |
           ((int64_t)b1 << 20) | ((int64_t)(b2 & 0xf8) << 12) |
           ((int64_t)(b2 & 0x03) << 13) | ((int64_t)b3 << 5) |
           ((int64_t)(b4 & 0xf8) >> 3);
}

static int getMpegVersion(uint32_t raw) {
    switch (raw) {
        case 0x32313030: return 0; case 0x33313030: return 1;
        case 0x34313030: return 2; case 0x35313030: return 3; default: return -1;
    }
}

/* AnalyzeMpeg: parse the PSMF header at bufferAddr into the context. */
static void analyze(uint32_t buffer, Mpeg *ctx) {
    ctx->magic = be32(buffer) == 0 ? 0 : 0; /* placeholder to avoid warning */
    /* magic is stored little-endian in memory but compared as PSMF_MAGIC ('PSMF' LE) */
    ctx->magic = MEM_R32(buffer);
    ctx->rawVersion = MEM_R32(buffer + PSMF_STREAM_VERSION_OFFSET);
    ctx->version = getMpegVersion(ctx->rawVersion);
    ctx->offset = be32(buffer + PSMF_STREAM_OFFSET_OFFSET);
    ctx->streamSize = be32(buffer + PSMF_STREAM_SIZE_OFFSET);
    ctx->firstTimestamp = mpeg_ts(buffer + PSMF_FIRST_TIMESTAMP_OFFSET);
    ctx->lastTimestamp = mpeg_ts(buffer + PSMF_LAST_TIMESTAMP_OFFSET);
    ctx->videoPts = 0; ctx->audioPts = 0; ctx->videoEnd = 0; ctx->audioEnd = 0;
    ctx->videoTailBuffered = 0;
    ctx->fedPackets = 0;
    ctx->consumedPackets = 0;
    ctx->totalPackets = ctx->streamSize / 2048u;
    if (getenv("SR_MPEGLOG"))
        fprintf(stderr, "MpegAnalyze offset=%u size=%u packets=%u timestamps=%lld..%lld duration=%.3fs\n",
                ctx->offset, ctx->streamSize, ctx->totalPackets,
                (long long)ctx->firstTimestamp, (long long)ctx->lastTimestamp,
                (double)(ctx->lastTimestamp - ctx->firstTimestamp) / (double)mpegTimestampPerSecond);
}

/* ---- SceMpegAu (auAddr): pts/dts stored as 32-bit-word-swapped s64 ---- */
static void au_write_pts(uint32_t auAddr, int off, int64_t v) {
    /* PPSSPP write(): pts = (lo<<32)|hi, then store. i.e. swap the two 32-bit halves. */
    uint32_t lo = (uint32_t)v, hi = (uint32_t)((uint64_t)v >> 32);
    MEM_W32(auAddr + (uint32_t)off, hi);          /* swapped: high word first */
    MEM_W32(auAddr + (uint32_t)off + 4, lo);
}

/* ================= exported handlers (called from hle.c) ================= */
/* Each returns the v0 value; out-params are written to guest memory directly. */

uint32_t mpeg_init(void) { s_mpegInit = 1; return 0; }
uint32_t mpeg_finish(void) {
    for (int i = 0; i < 8; i++) if (s_mpeg[i].used) {
#if defined(SR_SDL3VK) || defined(SR_D3D12)
        if (s_mpeg[i].h264 >= 0) {
            sr_h264_destroy(s_mpeg[i].h264);
            s_mpeg[i].h264 = -1;
        }
#endif
        sr_atrac_decoder_destroy(s_mpeg[i].pmfAtrac);
        s_mpeg[i].pmfAtrac = NULL;
    }
    s_mpegInit = 0;
    return 0;
}

uint32_t mpeg_query_mem_size(uint32_t outAddr) {
    if (outAddr) MEM_W32(outAddr, MPEG_MEMSIZE_0105);
    return 0;
}
uint32_t mpeg_ringbuffer_query_mem_size(uint32_t packets) {
    return packets * (104u + 2048u);     /* PPSSPP __MpegRingbufferQueryMemSize */
}

uint32_t mpeg_ringbuffer_construct(uint32_t ring, uint32_t numPackets, uint32_t data, uint32_t size,
                                   uint32_t cbAddr, uint32_t cbArg) {
    if (!ring) return 0x80020003u;
    if ((int32_t)size < 0) return SCE_MPEG_ERROR_NO_MEMORY;
    if (mpeg_ringbuffer_query_mem_size(numPackets) > size && numPackets < 0x00100000u)
        return SCE_MPEG_ERROR_NO_MEMORY;
    rb_set(ring, RB_packets, numPackets);
    rb_set(ring, RB_packetsRead, 0);
    rb_set(ring, RB_packetsWritePos, 0);
    rb_set(ring, RB_packetsAvail, 0);
    rb_set(ring, RB_packetSize, 2048);
    rb_set(ring, RB_data, data);
    rb_set(ring, RB_callback_addr, cbAddr);
    rb_set(ring, RB_callback_args, cbArg);
    rb_set(ring, RB_dataUpperBound, data + numPackets * 2048u);
    rb_set(ring, RB_mpeg, 0);
    return 0;
}

uint32_t mpeg_create(uint32_t mpegAddr, uint32_t dataPtr, uint32_t size, uint32_t ringAddr,
                     uint32_t frameWidth, uint32_t mode, uint32_t ddrTop) {
    (void)mode; (void)ddrTop;
    if (!mpegAddr) return (uint32_t)-1;
    if (size < MPEG_MEMSIZE_0105) return SCE_MPEG_ERROR_NO_MEMORY;

    if (ringAddr) {
        uint32_t psize = rb_get(ringAddr, RB_packetSize);
        if (psize == 0) rb_set(ringAddr, RB_packetsAvail, 0);
        else rb_set(ringAddr, RB_packetsAvail,
                    rb_get(ringAddr, RB_packets) - (rb_get(ringAddr, RB_dataUpperBound) - rb_get(ringAddr, RB_data)) / psize);
        rb_set(ringAddr, RB_mpeg, mpegAddr);
    }

    /* Generate handle = dataPtr + 0x30, write it at *mpegAddr, and lay down the fake struct. */
    uint32_t h = dataPtr + 0x30;
    MEM_W32(mpegAddr, h);
    const char *lib = "LIBMPEG\0"; for (int i = 0; i < 8; i++) MEM_W8(h + (uint32_t)i, (uint8_t)lib[i]);
    const char *v = "001\0"; for (int i = 0; i < 4; i++) MEM_W8(h + 8 + (uint32_t)i, (uint8_t)v[i]);
    MEM_W32(h + 12, (uint32_t)-1);
    if (ringAddr) { MEM_W32(h + 16, ringAddr); MEM_W32(h + 20, rb_get(ringAddr, RB_dataUpperBound)); }

    Mpeg *ctx = 0; for (int i = 0; i < 8; i++) if (!s_mpeg[i].used) { ctx = &s_mpeg[i]; break; }
    if (!ctx) return SCE_MPEG_ERROR_NO_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->used = 1; ctx->handle = h; ctx->ringAddr = ringAddr;
    ctx->defaultFrameWidth = (int)frameWidth; ctx->pixelMode = 3; ctx->isAnalyzed = 0;
    ctx->h264 = -1;
    return 0;
}

/* sceMpegAvcDecodeMode(mpeg, mode*): mode[1] is the output pixel format (GE 5650..8888). */
uint32_t mpeg_avc_decode_mode(uint32_t mpegAddr, uint32_t modeAddr) {
    if (!modeAddr || modeAddr > UINT32_MAX - 7u ||
        !sr_inrange(modeAddr) || !sr_inrange(modeAddr + 7u))
        return (uint32_t)-1;
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    int pixelMode = (int)MEM_R32(modeAddr + 4);
    if (pixelMode >= 0 && pixelMode <= 3) ctx->pixelMode = pixelMode;
    else fprintf(stderr, "sceMpegAvcDecodeMode: unknown pixel mode %d\n", pixelMode);
    if (getenv("SR_MPEGLOG"))
        fprintf(stderr, "MpegAvcDecodeMode pixelMode=%d\n", pixelMode);
    return 0;
}

uint32_t mpeg_delete(uint32_t mpegAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
#if defined(SR_SDL3VK) || defined(SR_D3D12)
    if (ctx->h264 >= 0) { sr_h264_destroy(ctx->h264); ctx->h264 = -1; }
#endif
    if (getenv("SR_MPEGLOG"))
        fprintf(stderr,
                "MpegDelete videoFrames=%d audioFrames=%d videoPts=%lld "
                "audioPts=%lld maxDrift=%lld\n",
                ctx->h264Frames, ctx->pmfAudioFrames,
                (long long)ctx->videoPts, (long long)ctx->audioPts,
                (long long)ctx->maxAvDrift);
    sr_atrac_decoder_destroy(ctx->pmfAtrac);
    ctx->pmfAtrac = NULL;
    ctx->used = 0;
    return 0;
}

uint32_t mpeg_query_stream_offset(uint32_t mpegAddr, uint32_t bufferAddr, uint32_t offsetAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx || !bufferAddr || !offsetAddr) return (uint32_t)-1;
    analyze(bufferAddr, ctx);
    ctx->isAnalyzed = 1;
    ctx->headerAddr = bufferAddr;
    if (ctx->magic != PSMF_MAGIC) { MEM_W32(offsetAddr, 0); return SCE_MPEG_ERROR_INVALID_VALUE; }
    if (ctx->version < 0) { MEM_W32(offsetAddr, 0); return SCE_MPEG_ERROR_BAD_VERSION; }
    if ((ctx->offset & 2047) != 0 || ctx->offset == 0) { MEM_W32(offsetAddr, 0); return SCE_MPEG_ERROR_INVALID_VALUE; }
    MEM_W32(offsetAddr, ctx->offset);
    return 0;
}

uint32_t mpeg_query_stream_size(uint32_t bufferAddr, uint32_t sizeAddr) {
    if (!bufferAddr || !sizeAddr) return (uint32_t)-1;
    Mpeg tmp; memset(&tmp, 0, sizeof(tmp));
    analyze(bufferAddr, &tmp);
    if (tmp.magic != PSMF_MAGIC) { MEM_W32(sizeAddr, 0); return SCE_MPEG_ERROR_INVALID_VALUE; }
    if ((tmp.offset & 2047) != 0) { MEM_W32(sizeAddr, 0); return SCE_MPEG_ERROR_INVALID_VALUE; }
    MEM_W32(sizeAddr, tmp.streamSize);
    return 0;
}

uint32_t mpeg_regist_stream(uint32_t mpegAddr, uint32_t streamType, uint32_t streamNum) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (streamType == MPEG_AVC_STREAM) ctx->avcRegistered = 1;
    else if (streamType == MPEG_ATRAC_STREAM || streamType == MPEG_AUDIO_STREAM) ctx->atracRegistered = 1;
    uint32_t sid = s_streamIdGen++;
    for (int i = 0; i < 8; i++) if (!ctx->streams[i].used) {
        ctx->streams[i].used = 1; ctx->streams[i].type = (int)streamType;
        ctx->streams[i].num = (int)streamNum; ctx->streams[i].sid = sid; ctx->streams[i].needsReset = 1;
        break;
    }
    return sid;
}
/* sceMpegMallocAvcEsBuf: PPSSPP keeps a couple of flags rather than really allocating; returns a
 * 1-based ES-buffer index (non-zero) the game treats as a valid handle, or 0 if none free. The
 * earlier h_ok returned 0, so the game read the movie's ES-buffer alloc as failed and tore down. */
uint32_t mpeg_malloc_avc_es_buf(uint32_t mpegAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    for (int i = 0; i < 2; i++) if (!ctx->esBuffers[i]) { ctx->esBuffers[i] = 1; return (uint32_t)(i + 1); }
    return 0;
}
uint32_t mpeg_free_avc_es_buf(uint32_t mpegAddr, uint32_t esBuf) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (esBuf >= 1 && esBuf <= 2) ctx->esBuffers[esBuf - 1] = 0;
    return 0;
}
/* sceMpegInitAu(mpeg, esBuffer, auAddr): initialise the SceMpegAu at auAddr. AVC buffers get
 * esSize=2048/dts=0; the others (Atrac) get esSize=2112/dts=-1. esBuffer is zeroed (PPSSPP abuses
 * it for the stream id later). pts/dts are stored 32-bit-word-swapped (au_write_pts). */
/* sceMpegQueryAtracEsSize(mpeg, esSizeAddr, outSizeAddr): ES packet size 2112, decoded output
 * size 8192 (PPSSPP MPEG_ATRAC_ES_SIZE / MPEG_ATRAC_ES_OUTPUT_SIZE). */
uint32_t mpeg_query_atrac_es_size(uint32_t mpegAddr, uint32_t esSizeAddr, uint32_t outSizeAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (esSizeAddr) MEM_W32(esSizeAddr, MPEG_ATRAC_ES_SIZE);
    if (outSizeAddr) MEM_W32(outSizeAddr, 8192);
    return 0;
}
uint32_t mpeg_init_au(uint32_t mpegAddr, uint32_t esBuffer, uint32_t auAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    int isAvc = (esBuffer >= 1 && esBuffer <= 2 && ctx->esBuffers[esBuffer - 1]);
    au_write_pts(auAddr, 0, isAvc ? 0 : 0);                 /* pts = 0 */
    au_write_pts(auAddr, 8, isAvc ? 0 : -1);                /* dts: AVC 0, Atrac UNKNOWN(-1) */
    MEM_W32(auAddr + 16, 0);                                /* esBuffer */
    MEM_W32(auAddr + 20, isAvc ? MPEG_AVC_ES_SIZE : MPEG_ATRAC_ES_SIZE);
    return 0;
}
uint32_t mpeg_unregist_stream(uint32_t mpegAddr, uint32_t sid) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (ctx) for (int i = 0; i < 8; i++) if (ctx->streams[i].used && ctx->streams[i].sid == sid) ctx->streams[i].used = 0;
    return 0;
}

uint32_t mpeg_ringbuffer_available_size(uint32_t ring) {
    if (!ring) return 0;
    return rb_get(ring, RB_packets) - rb_get(ring, RB_packetsAvail);
}

unsigned long g_mpeg_put = 0, g_mpeg_getavc = 0, g_mpeg_avcdec = 0, g_mpeg_nodata = 0;
/* RingbufferPut(ring, numPackets, available): call the game's fill callback, then feed the bytes
 * it wrote into PPSSPP's MediaEngine bridge. */
uint32_t mpeg_ringbuffer_put(CpuState *s, uint32_t ring, uint32_t numPackets, uint32_t available) {
    g_mpeg_put++;
    if (!ring) return 0;
    if ((int32_t)numPackets <= 0) return 0;
    uint32_t avail = rb_get(ring, RB_packetsAvail);
    uint32_t total = rb_get(ring, RB_packets);
    uint32_t mpegAddr = rb_get(ring, RB_mpeg);
    Mpeg *ctx = mpegAddr ? mpeg_find(mpegAddr) : 0;
    uint32_t addWanted = numPackets;
    if (addWanted > available) addWanted = available;
    if (avail + addWanted > total) addWanted = total - avail;
    if (ctx && ctx->totalPackets) {
        uint32_t remaining = ctx->fedPackets < ctx->totalPackets ?
                             ctx->totalPackets - ctx->fedPackets : 0;
        if (addWanted > remaining) addWanted = remaining;
    }
    if (addWanted == 0) return 0;

    uint32_t cb = rb_get(ring, RB_callback_addr);
    uint32_t cbArg = rb_get(ring, RB_callback_args);
    uint32_t packetSize = rb_get(ring, RB_packetSize);
    if (!packetSize) packetSize = 2048;
    if (cb && !sr_lookup(cb)) {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "mpeg: ring fill callback 0x%08x is not recompiled code "
                    "(ring=0x%08x data=0x%08x cbArg=0x%08x mpeg=0x%08x) -- runtime-loaded code?\n",
                    cb, ring, rb_get(ring, RB_data), cbArg, mpegAddr);
        }
    }

    uint32_t addedTotal = 0;
    uint32_t writePos = total ? (rb_get(ring, RB_packetsWritePos) % total) : 0;
    while (addedTotal < addWanted) {
        uint32_t chunk = addWanted - addedTotal;
        if (total && chunk > total - writePos) chunk = total - writePos;
        if (chunk == 0) break;

        uint32_t dst = rb_get(ring, RB_data) + writePos * packetSize;
        uint32_t got = cb ? call_guest3(s, cb, dst, chunk, cbArg) : chunk;
        if ((int32_t)got < 0) {
            if (addedTotal == 0) return got;
            break;
        }
        if (got > chunk) got = chunk;
        if (got == 0) break;

        if (ctx && packetSize >= 9 && ctx->lastTimestamp > ctx->firstTimestamp) {
            int64_t duration = ctx->lastTimestamp - ctx->firstTimestamp;
            uint32_t firstPacket = ctx->fedPackets + addedTotal;
            for (uint32_t i = 0; i < got; i++) {
                int64_t scr = mpeg_scr(dst + i * packetSize);
                if (!ctx->videoTailBuffered && scr >= duration - videoTimestampStep) {
                    ctx->videoTailBuffered = 1;
                    if (getenv("SR_MPEGLOG"))
                        fprintf(stderr, "MpegVideoTail packet=%u/%u scr=%lld duration=%lld\n",
                                firstPacket + i + 1, ctx->totalPackets,
                                (long long)scr, (long long)duration);
                }
            }
        }

#if defined(SR_SDL3VK) || defined(SR_D3D12)
        if (ctx && got) {
            if (!ctx->h264Init) { ctx->h264Init = 1; ctx->h264 = sr_h264_create(); }
            if (ctx->h264 >= 0)
                sr_h264_feed(ctx->h264, (const uint8_t *)SR_HOST(dst), got * packetSize);
        }
#endif

        addedTotal += got;
        writePos = total ? (writePos + got) % total : 0;
        if (got < chunk) break;
    }

    if (addedTotal) {
        rb_set(ring, RB_packetsAvail, avail + addedTotal);
        rb_set(ring, RB_packetsRead, rb_get(ring, RB_packetsRead) + addedTotal);
        rb_set(ring, RB_packetsWritePos, writePos);
        if (ctx) ctx->fedPackets += addedTotal;
    }
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || addedTotal != addWanted)
            fprintf(stderr, "MpegRingbufferPut ring=0x%x want=%u/%u cb=0x%x -> %u avail=%u/%u\n",
                    ring, numPackets, available, cb, addedTotal, rb_get(ring, RB_packetsAvail), total);
    }
    return addedTotal;
}

static Mpeg *au_stream(uint32_t mpegAddr, uint32_t sid, int *needsReset, int *num) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return 0;
    for (int i = 0; i < 8; i++) if (ctx->streams[i].used && ctx->streams[i].sid == sid) {
        if (needsReset) *needsReset = ctx->streams[i].needsReset;
        if (num) *num = ctx->streams[i].num;
        ctx->streams[i].needsReset = 0;
        return ctx;
    }
    return ctx;   /* stream not found still returns ctx; caller checks */
}

uint32_t mpeg_get_avc_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t attrAddr) {
    g_mpeg_getavc++;
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    uint32_t ring = ctx->ringAddr;
    if (!ring) return (uint32_t)-1;
    if (ctx->videoEnd) {
        g_mpeg_nodata++;
        int64_t pts = ctx->videoPts + ctx->firstTimestamp;
        au_write_pts(auAddr, 0, pts);
        au_write_pts(auAddr, 8, pts - videoTimestampStep);
        rb_set(ring, RB_packetsAvail, 0);
        if (attrAddr) MEM_W32(attrAddr, 1);
        return SCE_MPEG_ERROR_NO_DATA;
    }
    if (rb_get(ring, RB_packetsRead) == 0 ||
        (rb_get(ring, RB_packetsAvail) == 0 && !ctx->videoTailBuffered)) {
        g_mpeg_nodata++;
        au_write_pts(auAddr, 0, -1); au_write_pts(auAddr, 8, -1);
        return SCE_MPEG_ERROR_NO_DATA;
    }
    int needsReset = 0, num = 0;
    au_stream(mpegAddr, sid, &needsReset, &num);
    int64_t pts = ctx->videoPts + ctx->firstTimestamp;
    au_write_pts(auAddr, 0, pts);
    au_write_pts(auAddr, 8, pts - videoTimestampStep);
    MEM_W32(auAddr + 16, (uint32_t)num);            /* esBuffer abused as stream num */
    if (attrAddr) MEM_W32(attrAddr, 1);
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegGetAvcAu #%d pts=%lld avail=%u end=%d\n",
                    n, (long long)pts, rb_get(ring, RB_packetsAvail), ctx->videoEnd);
    }
    return 0;
}

uint32_t mpeg_get_atrac_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t attrAddr) {
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    uint32_t ring = ctx->ringAddr;
    if (!ring) return (uint32_t)-1;
    int audio_available = 0;
#if defined(SR_SDL3VK) || defined(SR_D3D12)
    if (ctx->h264Init && ctx->h264 >= 0)
        audio_available = sr_h264_audio_available(ctx->h264);
#endif
    if (rb_get(ring, RB_packetsAvail) == 0 && !audio_available)
        return SCE_MPEG_ERROR_NO_DATA;
    if (ctx->pmfAudioFrames &&
        ctx->audioPts > ctx->videoPts + audioTimestampStep)
        return SCE_MPEG_ERROR_NO_DATA;
    if (ctx->h264Init && ctx->h264 >= 0 && !audio_available)
        return SCE_MPEG_ERROR_NO_DATA;
    int needsReset = 0, num = 0;
    au_stream(mpegAddr, sid, &needsReset, &num);
    int64_t pts = ctx->audioPts + ctx->firstTimestamp;
    au_write_pts(auAddr, 0, pts);
    au_write_pts(auAddr, 8, pts);
    MEM_W32(auAddr + 20, MPEG_ATRAC_ES_SIZE);
    if (attrAddr) MEM_W32(attrAddr, 0);
    return 0;   /* audio AU available; the audio ring drains with the video at EOF */
}

static uint32_t video_buffer_bytes(uint32_t frameWidth, int pixelMode) {
    uint32_t fw = frameWidth ? frameWidth : 512;
    if (fw > 2048) fw = 512;
    return fw * 272u * (pixelMode == 3 ? 4u : 2u);
}

static void clear_video_buffer(uint32_t ptr, uint32_t frameWidth, int pixelMode) {
    if (!ptr) return;
    uint32_t bytes = video_buffer_bytes(frameWidth, pixelMode);
    if (bytes && bytes - 1u <= UINT32_MAX - ptr &&
        sr_inrange(ptr) && sr_inrange(ptr + bytes - 1u))
        memset(SR_HOST(ptr), 0, bytes);
    else
        for (uint32_t i = 0; i < bytes; i++) MEM_W8(ptr + i, 0);
}

static void consume_stream_for_video_pts(Mpeg *ctx) {
    if (!ctx->ringAddr || !ctx->totalPackets || ctx->lastTimestamp <= ctx->firstTimestamp) return;
    uint64_t duration = (uint64_t)(ctx->lastTimestamp - ctx->firstTimestamp);
    uint64_t elapsed = ctx->videoPts > 0 ? (uint64_t)ctx->videoPts : 0;
    if (elapsed > duration) elapsed = duration;
    uint32_t target = elapsed == duration ? ctx->totalPackets :
                      (uint32_t)(((uint64_t)ctx->totalPackets * elapsed) / duration);
    if (target > ctx->consumedPackets) {
        uint32_t take = target - ctx->consumedPackets;
        uint32_t avail = rb_get(ctx->ringAddr, RB_packetsAvail);
        if (take > avail) take = avail;
        rb_set(ctx->ringAddr, RB_packetsAvail, avail - take);
        ctx->consumedPackets += take;
    }
    int audio_pending = 0;
#if defined(SR_SDL3VK) || defined(SR_D3D12)
    if (ctx->h264Init && ctx->h264 >= 0)
        audio_pending = sr_h264_audio_available(ctx->h264);
#endif
    int packets_complete =
        ctx->fedPackets >= ctx->totalPackets &&
        ctx->consumedPackets >= ctx->totalPackets;
    if (elapsed == duration && !audio_pending &&
        (ctx->videoTailBuffered || packets_complete)) {
        ctx->videoEnd = 1;
    }
}

/* AvcDecode(mpeg, auAddr, frameWidth, bufferAddr, initAddr): decode one AVC frame into
 * *bufferAddr. Windows GPU builds decode through Media Foundation (h264_mf.c); otherwise the
 * timestamp-only model runs and leaves the frame blank. */
uint32_t mpeg_avc_decode(uint32_t mpegAddr, uint32_t auAddr, uint32_t frameWidth, uint32_t bufferAddr, uint32_t initAddr) {
    (void)auAddr;
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    g_mpeg_avcdec++;
    if (frameWidth == 0 || frameWidth > 2048)
        frameWidth = ctx->defaultFrameWidth ? (uint32_t)ctx->defaultFrameWidth : 512u;
    uint32_t buffer = bufferAddr ? MEM_R32(bufferAddr) : 0;

    int64_t duration = ctx->lastTimestamp - ctx->firstTimestamp;
    ctx->videoPts += videoTimestampStep;
    if (duration > 0 && ctx->videoPts > duration)
        ctx->videoPts = duration;
    consume_stream_for_video_pts(ctx);
    /* Report "a frame was produced" (1) every decode; the game keeps feeding/decoding until it has
     * read the whole file, then stops on its own. */
    int gotFrame = 0;
    uint32_t outputBytes =
        video_buffer_bytes(frameWidth, ctx->pixelMode);
    if (buffer) sr_gpu_vram_prepare_write(buffer, outputBytes);
#if defined(SR_SDL3VK) || defined(SR_D3D12)
    if (ctx->h264Init && ctx->h264 >= 0 && buffer) {
        int eos = ctx->totalPackets && ctx->fedPackets >= ctx->totalPackets;
        gotFrame = sr_h264_frame(ctx->h264, eos, (uint8_t *)SR_HOST(buffer),
                                 (int)frameWidth, ctx->pixelMode);
        if (gotFrame > 0) ctx->h264Frames++;
    }
#endif
    /* No decoder (or it hasn't produced its first frame yet): clear the destination instead of
     * leaving stale contents, which otherwise appears as moving black bands over uninitialised
     * movie frames. Once frames flow, a miss keeps the previous frame (no black flicker). */
    if (gotFrame <= 0 && !ctx->h264Frames)
        clear_video_buffer(buffer, frameWidth, ctx->pixelMode);
    {
        if (buffer) sr_gpu_vram_dirty(buffer, outputBytes);
    }
    if (initAddr) MEM_W32(initAddr, 1);
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr, "MpegAvcDecode #%d buf=0x%x fw=%u mode=%d pts=%lld packets=%u/%u dec=%d frames=%d end=%d\n",
                    n, buffer, frameWidth, ctx->pixelMode, (long long)ctx->videoPts,
                    ctx->consumedPackets, ctx->totalPackets, gotFrame, ctx->h264Frames, ctx->videoEnd);
    }
    return 0;
}
uint32_t mpeg_atrac_decode(uint32_t mpegAddr, uint32_t auAddr, uint32_t bufferAddr, uint32_t init) {
    (void)init;
    Mpeg *ctx = mpeg_find(mpegAddr);
    if (!ctx) return (uint32_t)-1;
    if (!bufferAddr ||
        bufferAddr > UINT32_MAX - (MPEG_ATRAC_ES_OUTPUT_SIZE - 1u) ||
        !sr_inrange(bufferAddr) ||
        !sr_inrange(bufferAddr + MPEG_ATRAC_ES_OUTPUT_SIZE - 1u))
        return (uint32_t)-1;

    memset(SR_HOST(bufferAddr), 0, MPEG_ATRAC_ES_OUTPUT_SIZE);
    int encoded_bytes = 0;
    int channels = 2;
    int64_t encoded_pts = -1;
    static uint8_t encoded[0x2000];
    static int16_t pcm[MPEG_ATRAC_SAMPLES_PER_FRAME * 2];
#if defined(SR_SDL3VK) || defined(SR_D3D12)
    if (ctx->h264Init && ctx->h264 >= 0)
        encoded_bytes =
            sr_h264_audio_frame(
                ctx->h264, encoded, sizeof(encoded), &channels, &encoded_pts);
#endif
    int samples = 0;
    if (encoded_bytes > 0) {
        if (!ctx->pmfAtrac || ctx->pmfAtracChannels != channels ||
            ctx->pmfAtracBlock != encoded_bytes) {
            sr_atrac_decoder_destroy(ctx->pmfAtrac);
            ctx->pmfAtrac =
                sr_atrac_decoder_create(
                    MPEG_ATRAC_CODEC_PLUS, channels, encoded_bytes);
            ctx->pmfAtracChannels = channels;
            ctx->pmfAtracBlock = encoded_bytes;
        }
        int consumed = ctx->pmfAtrac
            ? sr_atrac_decoder_decode(
                  ctx->pmfAtrac, encoded, encoded_bytes, pcm, 2,
                  MPEG_ATRAC_SAMPLES_PER_FRAME, &samples)
            : -1;
        if (consumed < 0) {
            fprintf(stderr,
                    "sceMpegAtracDecode: ATRAC3+ decode failed (%d bytes, code %d)\n",
                    encoded_bytes, consumed);
            samples = 0;
        } else {
            uint32_t bytes = (uint32_t)samples * 4u;
            if (bytes > MPEG_ATRAC_ES_OUTPUT_SIZE)
                bytes = MPEG_ATRAC_ES_OUTPUT_SIZE;
            memcpy(SR_HOST(bufferAddr), pcm, bytes);
            ctx->pmfAudioFrames++;
        }
    }

    if (encoded_bytes > 0) {
        if (encoded_pts >= ctx->firstTimestamp)
            ctx->audioPts =
                encoded_pts - ctx->firstTimestamp + audioTimestampStep;
        else
            ctx->audioPts += audioTimestampStep;
    }
    int64_t drift = ctx->videoPts - ctx->audioPts;
    int64_t abs_drift = drift < 0 ? -drift : drift;
    if (ctx->h264Frames && ctx->pmfAudioFrames && abs_drift > ctx->maxAvDrift)
        ctx->maxAvDrift = abs_drift;
    if (auAddr)
        au_write_pts(auAddr, 0, ctx->audioPts + ctx->firstTimestamp);
    if (getenv("SR_MPEGLOG")) {
        static int n = 0;
        if (n++ < 32 || (n & 0xFF) == 0)
            fprintf(stderr,
                    "MpegAtracDecode #%d bytes=%d samples=%d pesPts=%lld pts=%lld "
                    "videoPts=%lld drift=%lld frames=%d\n",
                    n, encoded_bytes, samples, (long long)encoded_pts,
                    (long long)ctx->audioPts,
                    (long long)ctx->videoPts, (long long)drift,
                    ctx->pmfAudioFrames);
    }
    return 0;
}
uint32_t mpeg_avc_decode_stop(uint32_t mpegAddr, uint32_t frameWidth, uint32_t bufferAddr, uint32_t statusAddr) {
    (void)frameWidth; (void)bufferAddr;
    if (statusAddr) MEM_W32(statusAddr, 0);   /* no frames left */
    (void)mpegAddr; return 0;
}

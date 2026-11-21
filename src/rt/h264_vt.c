/* PSP PSMF H.264 decoding via the hardware VideoToolbox decoder. */
#include "h264_psmf.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEC 4

typedef struct {
    int used, failed, drained, formatDirty, hasVcl;
    SrPsmfDemux demux;
    uint8_t *sps, *pps, *au;
    size_t spsLen, ppsLen, auLen, auCap;
    CMVideoFormatDescriptionRef format;
    VTDecompressionSessionRef session;
    CVPixelBufferRef frame;
    OSStatus callbackStatus;
    uint64_t submitted, returned;
} Dec;

static Dec s_dec[MAX_DEC];

static int fail(Dec *d, const char *what, OSStatus status) {
    fprintf(stderr, "h264: VideoToolbox %s failed (%d)\n", what, (int)status);
    d->failed = 1;
    return -1;
}

static void output_callback(
    void *refcon, void *source, OSStatus status, VTDecodeInfoFlags flags,
    CVImageBufferRef image, CMTime pts, CMTime duration) {
    (void)source;
    (void)pts;
    (void)duration;
    Dec *d = (Dec *)refcon;
    d->callbackStatus = status;
    if (status != noErr) return;
    if ((flags & kVTDecodeInfo_FrameDropped) || !image || d->frame) {
        d->callbackStatus = kVTVideoDecoderMalfunctionErr;
        return;
    }
    d->frame = CVPixelBufferRetain(image);
}

static void close_session(Dec *d) {
    if (d->session) {
        VTDecompressionSessionInvalidate(d->session);
        CFRelease(d->session);
        d->session = NULL;
    }
    if (d->format) {
        CFRelease(d->format);
        d->format = NULL;
    }
}

static int configure_session(Dec *d) {
    if (!d->formatDirty && d->session) return 1;
    if (!d->spsLen || !d->ppsLen)
        return fail(d, "missing H.264 parameter sets", -1);
    const uint8_t *sets[] = {d->sps, d->pps};
    size_t sizes[] = {d->spsLen, d->ppsLen};
    CMVideoFormatDescriptionRef format = NULL;
    OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
        kCFAllocatorDefault, 2, sets, sizes, 4, &format);
    if (status != noErr) return fail(d, "format creation", status);
    if (d->session && VTDecompressionSessionCanAcceptFormatDescription(d->session, format)) {
        CFRelease(d->format);
        d->format = format;
        d->formatDirty = 0;
        return 1;
    }
    close_session(d);
    d->format = format;

    const void *specKeys[] = {kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder};
    const void *specValues[] = {kCFBooleanTrue};
    CFDictionaryRef spec = CFDictionaryCreate(
        kCFAllocatorDefault, specKeys, specValues, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int32_t pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef pixelNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixelFormat);
    const void *pixelKeys[] = {kCVPixelBufferPixelFormatTypeKey};
    const void *pixelValues[] = {pixelNumber};
    CFDictionaryRef attrs = pixelNumber ? CFDictionaryCreate(
        kCFAllocatorDefault, pixelKeys, pixelValues, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks) : NULL;
    if (!spec || !pixelNumber || !attrs) {
        if (spec) CFRelease(spec);
        if (pixelNumber) CFRelease(pixelNumber);
        if (attrs) CFRelease(attrs);
        return fail(d, "decoder configuration allocation", -1);
    }
    VTDecompressionOutputCallbackRecord callback = {output_callback, d};
    status = VTDecompressionSessionCreate(
        kCFAllocatorDefault, d->format, spec, attrs, &callback, &d->session);
    CFRelease(spec);
    CFRelease(attrs);
    CFRelease(pixelNumber);
    if (status != noErr || !d->session)
        return fail(d, "hardware decoder creation", status);
    CFTypeRef hardware = NULL;
    status = VTSessionCopyProperty(
        d->session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
        kCFAllocatorDefault, &hardware);
    int isHardware = status == noErr && hardware &&
                     CFGetTypeID(hardware) == CFBooleanGetTypeID() &&
                     CFBooleanGetValue((CFBooleanRef)hardware);
    if (hardware) CFRelease(hardware);
    if (!isHardware)
        return fail(d, "hardware decoder verification", status != noErr ? status : -1);
    d->formatDirty = 0;
    if (getenv("SR_MPEGLOG")) {
        CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(d->format);
        fprintf(stderr, "h264: VideoToolbox hardware decoder active (%dx%d)\n",
                dimensions.width, dimensions.height);
    }
    return 1;
}

static int set_parameter(Dec *d, uint8_t **dst, size_t *dstLen,
                         const uint8_t *src, size_t len) {
    if (*dstLen == len && *dst && memcmp(*dst, src, len) == 0) return 1;
    uint8_t *p = (uint8_t *)malloc(len);
    if (!p) return fail(d, "parameter set allocation", -1);
    memcpy(p, src, len);
    free(*dst);
    *dst = p;
    *dstLen = len;
    d->formatDirty = 1;
    return 1;
}

static int append_nal(Dec *d, const uint8_t *src, uint32_t len) {
    if (d->auLen > SIZE_MAX - 4u - len)
        return fail(d, "access unit size", -1);
    size_t need = d->auLen + 4u + len;
    if (need > d->auCap) {
        size_t cap = d->auCap ? d->auCap : 65536u;
        while (cap < need) {
            if (cap > SIZE_MAX / 2u) { cap = need; break; }
            cap *= 2u;
        }
        uint8_t *p = (uint8_t *)realloc(d->au, cap);
        if (!p) return fail(d, "access unit allocation", -1);
        d->au = p;
        d->auCap = cap;
    }
    uint8_t *p = d->au + d->auLen;
    p[0] = (uint8_t)(len >> 24);
    p[1] = (uint8_t)(len >> 16);
    p[2] = (uint8_t)(len >> 8);
    p[3] = (uint8_t)len;
    memcpy(p + 4, src, len);
    d->auLen = need;
    return 1;
}

static int submit_access_unit(Dec *d) {
    if (configure_session(d) < 0) return -1;
    CMBlockBufferRef block = NULL;
    CMSampleBufferRef sample = NULL;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault, NULL, d->auLen, kCFAllocatorDefault,
        NULL, 0, d->auLen, 0, &block);
    if (status == noErr)
        status = CMBlockBufferReplaceDataBytes(d->au, block, 0, d->auLen);
    CMSampleTimingInfo timing = {
        CMTimeMake(3003, 90000),
        CMTimeMake((int64_t)d->submitted * 3003, 90000),
        kCMTimeInvalid
    };
    size_t size = d->auLen;
    if (status == noErr)
        status = CMSampleBufferCreateReady(
            kCFAllocatorDefault, block, d->format, 1, 1, &timing, 1, &size, &sample);
    if (status == noErr) {
        d->callbackStatus = noErr;
        status = VTDecompressionSessionDecodeFrame(d->session, sample, 0, NULL, NULL);
        if (status == noErr) status = d->callbackStatus;
    }
    if (sample) CFRelease(sample);
    if (block) CFRelease(block);
    d->auLen = 0;
    d->hasVcl = 0;
    if (status != noErr) return fail(d, "frame decode", status);
    if (!d->frame) return fail(d, "frame callback", -1);
    d->submitted++;
    return 1;
}

/* Gather complete Annex-B NAL units into one AVCC access unit. */
static int feed_one(Dec *d, int eos) {
    SrPsmfDemux *s = &d->demux;
    uint32_t begin, header, end, nextHeader;
    if (!sr_h264_find_nal(s->es, s->esLen, s->esPos, &begin, &header)) {
        if (eos && d->hasVcl) return submit_access_unit(d);
        return 0;
    }
    if (!sr_h264_find_nal(s->es, s->esLen, header + 1u, &end, &nextHeader)) {
        if (!eos) return 0;
        end = s->esLen;
    }
    uint8_t type = s->es[header] & 31u;
    int vcl = type == 1 || type == 5;
    if (vcl && end <= header + 1u)
        return fail(d, "truncated H.264 slice", -1);
    if (type >= 2 && type <= 4)
        return fail(d, "unsupported H.264 partitioned slice", -1);
    if (d->hasVcl &&
        (type == 9 || type == 7 || type == 8 || type == 6 ||
         (vcl && (s->es[header + 1] & 0x80))))
        return submit_access_unit(d);

    int result = 1;
    if (type == 7)
        result = set_parameter(d, &d->sps, &d->spsLen, s->es + header, end - header);
    else if (type == 8)
        result = set_parameter(d, &d->pps, &d->ppsLen, s->es + header, end - header);
    else if (type != 9)
        result = append_nal(d, s->es + header, end - header);
    if (result < 0) return -1;
    if (vcl) d->hasVcl = 1;
    s->esPos = end;
    sr_psmf_compact_video(s);
    return 1;
}

static uint8_t clamp8(int x) {
    return x < 0 ? 0 : x > 255 ? 255 : (uint8_t)x;
}

static int convert_frame(Dec *d, uint8_t *dst, int frameWidth, int pixelMode) {
    CVPixelBufferRef frame = d->frame;
    if (!dst) return 1;
    if (frameWidth <= 0 || pixelMode < 0 || pixelMode > 3)
        return fail(d, "invalid destination format", -1);
    if (CVPixelBufferGetPixelFormatType(frame) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        CVPixelBufferGetPlaneCount(frame) != 2)
        return fail(d, "unexpected decoded pixel format", -1);
    CVReturn status = CVPixelBufferLockBaseAddress(frame, kCVPixelBufferLock_ReadOnly);
    if (status != kCVReturnSuccess) return fail(d, "pixel buffer lock", status);
    size_t width = CVPixelBufferGetWidth(frame);
    size_t height = CVPixelBufferGetHeight(frame);
    size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(frame, 0);
    size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(frame, 1);
    const uint8_t *yp = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(frame, 0);
    const uint8_t *uvp = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(frame, 1);
    if (width > (size_t)frameWidth) width = (size_t)frameWidth;
    if (height > 272u) height = 272u;
    int valid = yp && uvp && yStride >= width &&
                uvStride >= ((width + 1u) & ~(size_t)1u) &&
                CVPixelBufferGetHeightOfPlane(frame, 0) >= height &&
                CVPixelBufferGetHeightOfPlane(frame, 1) >= (height + 1u) / 2u;
    if (valid) for (size_t y = 0; y < height; y++) {
        const uint8_t *yr = yp + y * yStride;
        const uint8_t *uv = uvp + (y / 2u) * uvStride;
        uint8_t *row = dst + y * (size_t)frameWidth * (pixelMode == 3 ? 4u : 2u);
        for (size_t x = 0; x < width; x++) {
            int c = yr[x] - 16, u = uv[x & ~(size_t)1u] - 128;
            int v = uv[(x & ~(size_t)1u) + 1u] - 128;
            int r = clamp8((298 * c + 409 * v + 128) >> 8);
            int g = clamp8((298 * c - 100 * u - 208 * v + 128) >> 8);
            int b = clamp8((298 * c + 516 * u + 128) >> 8);
            if (pixelMode == 3) {
                row[x * 4] = (uint8_t)r;
                row[x * 4 + 1] = (uint8_t)g;
                row[x * 4 + 2] = (uint8_t)b;
                row[x * 4 + 3] = 255;
            } else {
                uint16_t pixel = pixelMode == 0 ?
                    (uint16_t)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)) :
                    pixelMode == 1 ?
                    (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000) :
                    (uint16_t)((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | 0xF000);
                memcpy(row + x * 2u, &pixel, sizeof(pixel));
            }
        }
    }
    CVReturn unlockStatus = CVPixelBufferUnlockBaseAddress(frame, kCVPixelBufferLock_ReadOnly);
    if (!valid) return fail(d, "invalid decoded pixel planes", -1);
    if (unlockStatus != kCVReturnSuccess) return fail(d, "pixel buffer unlock", unlockStatus);
    return 1;
}

int sr_h264_create(void) {
    if (getenv("SR_NOH264")) return -1;
    for (int i = 0; i < MAX_DEC; i++) {
        if (!s_dec[i].used) {
            memset(&s_dec[i], 0, sizeof(s_dec[i]));
            s_dec[i].used = 1;
            return i;
        }
    }
    fprintf(stderr, "h264: no free VideoToolbox decoder slots\n");
    return -1;
}

void sr_h264_destroy(int id) {
    if (id < 0 || id >= MAX_DEC || !s_dec[id].used) return;
    Dec *d = &s_dec[id];
    close_session(d);
    if (d->frame) CVPixelBufferRelease(d->frame);
    sr_psmf_destroy(&d->demux);
    free(d->sps);
    free(d->pps);
    free(d->au);
    memset(d, 0, sizeof(*d));
}

void sr_h264_feed(int id, const uint8_t *data, uint32_t len) {
    if (id < 0 || id >= MAX_DEC || !s_dec[id].used || !data || !len) return;
    Dec *d = &s_dec[id];
    if (d->failed) return;
    if (d->drained) {
        fail(d, "input after end of stream", -1);
    } else if (!sr_psmf_feed(&d->demux, data, len)) {
        fail(d, "invalid PSMF stream or demux allocation", -1);
    }
}

int sr_h264_audio_available(int id) {
    if (id < 0 || id >= MAX_DEC || !s_dec[id].used) return 0;
    return sr_psmf_audio_available(&s_dec[id].demux);
}

int sr_h264_audio_frame(int id, uint8_t *dst, int capacity, int *channels, int64_t *pts) {
    if (id < 0 || id >= MAX_DEC || !s_dec[id].used) return -1;
    return sr_psmf_audio_frame(&s_dec[id].demux, dst, capacity, channels, pts);
}

int sr_h264_frame(int id, int eos, uint8_t *dst, int frameWidth, int pixelMode) {
    if (id < 0 || id >= MAX_DEC || !s_dec[id].used) return -1;
    Dec *d = &s_dec[id];
    if (d->failed) return -1;
    for (int guard = 0; guard < 4096; guard++) {
        if (d->frame) {
            int result = convert_frame(d, dst, frameWidth, pixelMode);
            CVPixelBufferRelease(d->frame);
            d->frame = NULL;
            if (result < 0) return -1;
            d->returned++;
            return 1;
        }
        int result = feed_one(d, eos);
        if (result < 0) return -1;
        if (result > 0) continue;
        if (eos && !d->drained) {
            if (!sr_psmf_finish(&d->demux))
                return fail(d, "truncated PSMF packet at end of stream", -1);
            d->drained = 1;
            if (d->session) {
                OSStatus status = VTDecompressionSessionWaitForAsynchronousFrames(d->session);
                if (status != noErr) return fail(d, "end-of-stream drain", status);
                if (d->frame) continue;
            }
        }
        return 0;
    }
    return fail(d, "access-unit progress limit", -1);
}

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <pthread.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sr_h264_create(void);
void sr_h264_destroy(int id);
void sr_h264_feed(int id, const uint8_t *data, uint32_t len);
int sr_h264_frame(int id, int eos, uint8_t *dst, int width, int mode);

typedef struct {
    uint8_t stream[256 * 1024];
    size_t len;
    int count, failed;
    pthread_mutex_t lock;
} Fixture;

static int append(Fixture *f, const void *data, size_t len) {
    if (len > sizeof(f->stream) - f->len) return 0;
    memcpy(f->stream + f->len, data, len);
    f->len += len;
    return 1;
}

static int nal(Fixture *f, const uint8_t *data, size_t len) {
    const uint8_t start[] = {0, 0, 0, 1};
    return append(f, start, sizeof(start)) && append(f, data, len);
}

static void encoded(void *ref, void *source, OSStatus status,
                    VTEncodeInfoFlags flags, CMSampleBufferRef sample) {
    (void)source;
    Fixture *f = (Fixture *)ref;
    pthread_mutex_lock(&f->lock);
    if (status != noErr || (flags & kVTEncodeInfo_FrameDropped) || !sample) {
        f->failed = 1;
        pthread_mutex_unlock(&f->lock);
        return;
    }
    size_t pesStart = f->len;
    const uint8_t pesHeader[] = {0, 0, 1, 0xe0, 0, 0, 0x80, 0, 0};
    int ok = append(f, pesHeader, sizeof(pesHeader));
    if (f->count < 2) {
        const uint8_t aud[] = {0x09, 0xf0};
        ok = ok && nal(f, aud, sizeof(aud));
    }
    if (f->count == 0) {
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
        for (size_t i = 0; i < 2 && ok; i++) {
            const uint8_t *p = NULL;
            size_t len = 0;
            if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    format, i, &p, &len, NULL, NULL) != noErr)
                ok = 0;
            else
                ok = nal(f, p, len);
        }
    }
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    size_t size = block ? CMBlockBufferGetDataLength(block) : 0;
    uint8_t *bytes = size ? (uint8_t *)malloc(size) : NULL;
    if (!bytes || CMBlockBufferCopyDataBytes(block, 0, size, bytes) != noErr) ok = 0;
    for (size_t pos = 0; ok && pos < size;) {
        if (size - pos < 4) { ok = 0; break; }
        uint32_t len = ((uint32_t)bytes[pos] << 24) | ((uint32_t)bytes[pos + 1] << 16) |
                       ((uint32_t)bytes[pos + 2] << 8) | bytes[pos + 3];
        pos += 4;
        if (!len || len > size - pos) { ok = 0; break; }
        ok = nal(f, bytes + pos, len);
        pos += len;
    }
    free(bytes);
    size_t pesLen = f->len - pesStart - 6;
    if (pesLen > 65535) ok = 0;
    if (ok) {
        f->stream[pesStart + 4] = (uint8_t)(pesLen >> 8);
        f->stream[pesStart + 5] = (uint8_t)pesLen;
        f->count++;
    } else {
        f->failed = 1;
    }
    pthread_mutex_unlock(&f->lock);
}

static void make_fixture(Fixture *f) {
    assert(!pthread_mutex_init(&f->lock, NULL));
    VTCompressionSessionRef session = NULL;
    assert(VTCompressionSessionCreate(
        NULL, 32, 32, kCMVideoCodecType_H264, NULL, NULL, NULL,
        encoded, f, &session) == noErr);
    assert(VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel,
                                kVTProfileLevel_H264_Baseline_AutoLevel) == noErr);
    assert(VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering,
                                kCFBooleanFalse) == noErr);
    assert(VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowTemporalCompression,
                                kCFBooleanFalse) == noErr);
    for (int i = 0; i < 4; i++) {
        CVPixelBufferRef image = NULL;
        assert(CVPixelBufferCreate(NULL, 32, 32,
            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, NULL, &image) == kCVReturnSuccess);
        assert(CVPixelBufferLockBaseAddress(image, 0) == kCVReturnSuccess);
        for (size_t p = 0; p < 2; p++) {
            uint8_t *base = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(image, p);
            size_t stride = CVPixelBufferGetBytesPerRowOfPlane(image, p);
            size_t height = CVPixelBufferGetHeightOfPlane(image, p);
            memset(base, p == 0 ? 40 + i * 40 : 128, stride * height);
        }
        assert(CVPixelBufferUnlockBaseAddress(image, 0) == kCVReturnSuccess);
        assert(VTCompressionSessionEncodeFrame(
            session, image, CMTimeMake(i, 30), CMTimeMake(1, 30),
            NULL, NULL, NULL) == noErr);
        CVPixelBufferRelease(image);
    }
    assert(VTCompressionSessionCompleteFrames(session, kCMTimeInvalid) == noErr);
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    assert(!f->failed && f->count == 4 && f->len);
    assert(!pthread_mutex_destroy(&f->lock));
}

static void check_pixel(const uint8_t *out, int mode, int expected) {
    int r, g, b;
    if (mode == 3) {
        r = out[0]; g = out[1]; b = out[2];
        assert(out[3] == 255);
    } else {
        uint16_t p;
        memcpy(&p, out, sizeof(p));
        if (mode == 0) {
            r = (p & 31) << 3; g = ((p >> 5) & 63) << 2; b = ((p >> 11) & 31) << 3;
        } else if (mode == 1) {
            assert(p & 0x8000);
            r = (p & 31) << 3; g = ((p >> 5) & 31) << 3; b = ((p >> 10) & 31) << 3;
        } else {
            assert((p & 0xf000) == 0xf000);
            r = (p & 15) << 4; g = ((p >> 4) & 15) << 4; b = ((p >> 8) & 15) << 4;
        }
    }
    assert(abs(r - expected) <= 20);
    assert(abs(g - expected) <= 20);
    assert(abs(b - expected) <= 20);
}

int main(void) {
    Fixture *f = (Fixture *)calloc(1, sizeof(*f));
    assert(f);
    make_fixture(f);
    int id = sr_h264_create();
    assert(id >= 0);
    for (size_t pos = 0; pos < f->len;) {
        size_t len = f->len - pos > 7 ? 7 : f->len - pos;
        sr_h264_feed(id, f->stream + pos, (uint32_t)len);
        pos += len;
    }
    uint8_t output[48 * 272 * 4];
    for (int i = 0; i < 4; i++) {
        memset(output, 0xaa, sizeof(output));
        assert(sr_h264_frame(id, i == 3, output, 48, i) == 1);
        check_pixel(output, i, (298 * (40 + i * 40 - 16) + 128) >> 8);
        size_t bytes = i == 3 ? 4u : 2u;
        assert(output[32 * bytes] == 0xaa);
        assert(output[32 * 48 * bytes] == 0xaa);
    }
    assert(sr_h264_frame(id, 1, output, 48, 3) == 0);
    assert(sr_h264_frame(id, 1, output, 48, 3) == 0);
    sr_h264_destroy(id);
    id = sr_h264_create();
    assert(id >= 0);
    sr_h264_feed(id, f->stream, (uint32_t)f->len);
    assert(sr_h264_frame(id, 0, output, 48, 4) == -1);
    assert(sr_h264_frame(id, 0, output, 48, 3) == -1);
    sr_h264_destroy(id);
    id = sr_h264_create();
    assert(id >= 0);
    const uint8_t missing_parameters[] = {
        0, 0, 1, 0xe0, 0, 9, 0x80, 0, 0, 0, 0, 1, 0x65, 0x80, 0x80
    };
    sr_h264_feed(id, missing_parameters, sizeof(missing_parameters));
    assert(sr_h264_frame(id, 1, output, 48, 3) == -1);
    assert(sr_h264_frame(id, 1, output, 48, 3) == -1);
    sr_h264_destroy(id);
    id = sr_h264_create();
    assert(id >= 0);
    const uint8_t truncated[] = {0, 0, 1, 0xe0, 0, 8, 0x80, 0, 0};
    sr_h264_feed(id, truncated, sizeof(truncated));
    assert(sr_h264_frame(id, 1, output, 48, 3) == -1);
    sr_h264_destroy(id);
    free(f);
    puts("VideoToolbox hardware decode tests passed");
    return 0;
}

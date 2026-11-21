#include <stdint.h>
#include <stdio.h>

static void unavailable(void) {
    static int reported;
    if (reported) return;
    reported = 1;
    fprintf(
        stderr,
        "video: H.264 movie decoding is unavailable on this platform; "
        "gameplay continues without decoded movie frames\n");
}

int sr_h264_create(void) {
    unavailable();
    return -1;
}

void sr_h264_destroy(int id) {
    (void)id;
}

void sr_h264_feed(int id, const uint8_t *data, uint32_t len) {
    (void)id;
    (void)data;
    (void)len;
}

int sr_h264_audio_available(int id) {
    (void)id;
    return 0;
}

int sr_h264_audio_frame(
    int id, uint8_t *dst, int capacity,
    int *channels, int64_t *pts) {
    (void)id;
    (void)dst;
    (void)capacity;
    if (channels) *channels = 0;
    if (pts) *pts = -1;
    return 0;
}

int sr_h264_frame(
    int id, int eos, uint8_t *dst,
    int frame_width, int pixel_mode) {
    (void)id;
    (void)eos;
    (void)dst;
    (void)frame_width;
    (void)pixel_mode;
    return 0;
}

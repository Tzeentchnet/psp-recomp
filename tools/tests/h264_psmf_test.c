#include "h264_psmf.h"
#include "mpeg_audio_clock.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t pes(uint8_t *out, uint8_t stream, const uint8_t *data,
                  size_t len, int with_pts) {
    size_t header = with_pts ? 14u : 9u;
    size_t body = header - 6u + len;
    out[0] = 0; out[1] = 0; out[2] = 1; out[3] = stream;
    out[4] = (uint8_t)(body >> 8); out[5] = (uint8_t)body;
    out[6] = 0x80; out[7] = with_pts ? 0x80 : 0; out[8] = with_pts ? 5 : 0;
    if (with_pts) {
        /* 90,000 ticks */
        out[9] = 0x21; out[10] = 0; out[11] = 5; out[12] = 0xbf; out[13] = 0x21;
    }
    memcpy(out + header, data, len);
    return header + len;
}

int main(void) {
    int64_t clock = sr_mpeg_audio_next_pts(0, 85820, 90000, 4180, 1);
    assert(clock == 0);
    clock = sr_mpeg_audio_next_pts(clock, 85820, 90000, 4180, 0);
    assert(clock == 4180);
    clock = sr_mpeg_audio_next_pts(clock, 85820, 90000, 4180, 0);
    assert(clock == 8360);
    clock = sr_mpeg_audio_next_pts(clock, 85820, 90000, 4180, 0);
    assert(clock == 12540);
    clock = sr_mpeg_audio_next_pts(clock, 102538, 90000, 4180, 0);
    assert(clock == 16720);
    clock = sr_mpeg_audio_next_pts(clock, -1, 90000, 4180, 0);
    assert(clock == 20900);
    clock = sr_mpeg_audio_next_pts(clock, 200000, 90000, 4180, 0);
    assert(clock == 114180);

    SrPsmfDemux d = {0};
    uint8_t stream[512] = {0x42, 0x43};
    const uint8_t video1[] = {0, 0, 0, 1, 0x67, 1, 2};
    const uint8_t video2[] = {0, 0, 1, 0x65, 0x80};
    const uint8_t audio1[] = {0xb0, 0, 0, 0, 0, 0x0f, 0xd0, 0x24, 0, 1, 2};
    const uint8_t audio2[] = {
        0xb0, 0, 0, 0, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
        0x0f, 0xd0, 0x20, 0, 9, 9, 9, 9, 20, 21, 22, 23, 24, 25, 26, 27
    };
    size_t len = 2;
    len += pes(stream + len, 0xe0, video1, sizeof(video1), 0);
    len += pes(stream + len, 0xbd, audio1, sizeof(audio1), 1);
    len += pes(stream + len, 0xe0, video2, sizeof(video2), 0);
    len += pes(stream + len, 0xbd, audio2, sizeof(audio2), 0);
    for (size_t i = 0; i < len; i++)
        assert(sr_psmf_feed(&d, stream + i, 1));
    assert(d.esLen == sizeof(video1) + sizeof(video2));
    assert(!memcmp(d.es, video1, sizeof(video1)));
    assert(!memcmp(d.es + sizeof(video1), video2, sizeof(video2)));
    uint32_t offset, header;
    assert(sr_h264_find_nal(d.es, d.esLen, 0, &offset, &header));
    assert(offset == 0 && header == 4);
    assert(sr_h264_find_nal(d.es, d.esLen, header + 1, &offset, &header));
    assert(offset == sizeof(video1) && header == sizeof(video1) + 3);
    assert(!sr_h264_find_nal(d.es, d.esLen, header + 1, &offset, &header));

    uint8_t output[32] = {0};
    int channels = 0;
    int64_t pts = 0;
    assert(sr_psmf_audio_available(&d));
    assert(sr_psmf_audio_frame(&d, output, 7, &channels, &pts) == -1);
    assert(sr_psmf_audio_frame(&d, output, sizeof(output), &channels, &pts) == 8);
    const uint8_t first[] = {5, 6, 7, 8, 9, 10, 11, 12};
    assert(!memcmp(output, first, sizeof(first)));
    assert(channels == 1 && pts == 90000);
    assert(sr_psmf_audio_frame(&d, output, sizeof(output), &channels, &pts) == 8);
    const uint8_t second[] = {20, 21, 22, 23, 24, 25, 26, 27};
    assert(!memcmp(output, second, sizeof(second)));
    assert(channels == 2 && pts == -1);
    assert(!sr_psmf_audio_available(&d));
    assert(sr_psmf_finish(&d));
    sr_psmf_destroy(&d);

    uint8_t large_audio[5 + 16 * 2048] = {0xb0};
    uint8_t large_pes[sizeof(large_audio) + 14];
    for (size_t i = 5; i < sizeof(large_audio); i += 16) {
        large_audio[i] = 0x0f;
        large_audio[i + 1] = 0xd0;
        large_audio[i + 2] = 0x20;
    }
    size_t large_len = pes(large_pes, 0xbd, large_audio, sizeof(large_audio), 1);
    for (int packet = 0; packet < 33; packet++) {
        assert(sr_psmf_feed(&d, large_pes, (uint32_t)large_len));
        for (int frame = 0; frame < 2048; frame++) {
            assert(sr_psmf_audio_frame(&d, output, sizeof(output), &channels, &pts) == 8);
            assert(channels == 2 && pts == 90000);
        }
    }
    assert(d.audioPos < (1u << 20) && d.psPos < (1u << 20));
    assert(!sr_psmf_audio_available(&d));
    sr_psmf_destroy(&d);

    uint8_t malformed[] = {0, 0, 1, 0xe0, 0, 3, 0x80, 0, 1};
    assert(!sr_psmf_feed(&d, malformed, sizeof(malformed)));
    sr_psmf_destroy(&d);
    uint8_t zero_length[] = {0, 0, 1, 0xe0, 0, 0};
    assert(!sr_psmf_feed(&d, zero_length, sizeof(zero_length)));
    sr_psmf_destroy(&d);
    uint8_t truncated[] = {0, 0, 1, 0xe0, 0, 8, 0x80, 0, 0};
    assert(sr_psmf_feed(&d, truncated, sizeof(truncated)));
    assert(!sr_psmf_finish(&d));
    sr_psmf_destroy(&d);
    puts("PSMF demux tests passed");
    return 0;
}

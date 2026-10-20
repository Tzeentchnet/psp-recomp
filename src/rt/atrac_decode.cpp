#include "atrac_decode.h"

#include "at3_decoders.h"

#include <new>

struct SrAtracDecoder {
    int codec;
    int channels;
    int block_align;
    ATRAC3Context *atrac3;
    ATRAC3PContext *atrac3plus;
    alignas(32) float left[4096];
    alignas(32) float right[4096];
};

static int16_t clamp_sample(float sample) {
    if (sample >= 1.0f) return 32767;
    if (sample <= -1.0f) return -32767;
    return (int16_t)(sample * 32767.0f);
}

extern "C" SrAtracDecoder *sr_atrac_decoder_create(
    int codec, int channels, int block_align) {
    if ((codec != 0x1000 && codec != 0x1001) ||
        (channels != 1 && channels != 2) || block_align <= 0)
        return nullptr;

    SrAtracDecoder *decoder = new (std::nothrow) SrAtracDecoder{};
    if (!decoder) return nullptr;
    decoder->codec = codec;
    decoder->channels = channels;
    decoder->block_align = block_align;

    if (codec == 0x1000) {
        decoder->atrac3plus = atrac3p_alloc(channels, &decoder->block_align);
    } else {
        static const struct {
            int bytes;
            int channels;
            int joint_stereo;
        } formats[] = {
            {0x00c0, 1, 0}, {0x0098, 1, 0}, {0x0180, 2, 0},
            {0x0130, 2, 0}, {0x00c0, 2, 1},
        };
        int joint_stereo = 0;
        for (const auto &format : formats) {
            if (format.bytes == block_align && format.channels == channels) {
                joint_stereo = format.joint_stereo;
                break;
            }
        }
        uint8_t extra[14]{};
        extra[0] = 1;
        extra[3] = (uint8_t)(channels << 3);
        extra[6] = (uint8_t)joint_stereo;
        extra[8] = (uint8_t)joint_stereo;
        extra[10] = 1;
        decoder->atrac3 = atrac3_alloc(
            channels, &decoder->block_align, extra, sizeof(extra));
    }

    if (!decoder->atrac3 && !decoder->atrac3plus) {
        delete decoder;
        return nullptr;
    }
    return decoder;
}

extern "C" void sr_atrac_decoder_destroy(SrAtracDecoder *decoder) {
    if (!decoder) return;
    if (decoder->atrac3) atrac3_free(decoder->atrac3);
    if (decoder->atrac3plus) atrac3p_free(decoder->atrac3plus);
    delete decoder;
}

extern "C" void sr_atrac_decoder_reset(SrAtracDecoder *decoder) {
    if (!decoder) return;
    if (decoder->atrac3) atrac3_flush_buffers(decoder->atrac3);
    if (decoder->atrac3plus) atrac3p_flush_buffers(decoder->atrac3plus);
}

extern "C" int sr_atrac_decoder_decode(
    SrAtracDecoder *decoder, const uint8_t *input, int input_bytes,
    int16_t *output, int output_channels, int output_capacity,
    int *output_samples) {
    if (output_samples) *output_samples = 0;
    if (!decoder || !input || !output ||
        (output_channels != 1 && output_channels != 2) || output_capacity <= 0)
        return -1;

    float *decoded[2] = {decoder->left, decoder->right};
    int samples = 0;
    int consumed = decoder->atrac3plus
        ? atrac3p_decode_frame(
              decoder->atrac3plus, decoded, &samples, input, input_bytes)
        : atrac3_decode_frame(
              decoder->atrac3, decoded, &samples, input, input_bytes);
    if (consumed < 0) return consumed;
    if (samples > output_capacity) samples = output_capacity;

    const float *right = decoder->channels == 2 ? decoder->right : decoder->left;
    for (int i = 0; i < samples; i++) {
        output[i * output_channels] = clamp_sample(decoder->left[i]);
        if (output_channels == 2)
            output[i * 2 + 1] = clamp_sample(right[i]);
    }
    if (output_samples) *output_samples = samples;
    return consumed;
}

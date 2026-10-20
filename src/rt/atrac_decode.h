#ifndef PSP_RECOMP_ATRAC_DECODE_H
#define PSP_RECOMP_ATRAC_DECODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SrAtracDecoder SrAtracDecoder;

SrAtracDecoder *sr_atrac_decoder_create(int codec, int channels, int block_align);
void sr_atrac_decoder_destroy(SrAtracDecoder *decoder);
void sr_atrac_decoder_reset(SrAtracDecoder *decoder);
int sr_atrac_decoder_decode(SrAtracDecoder *decoder, const uint8_t *input,
                            int input_bytes, int16_t *output, int output_channels,
                            int output_capacity, int *output_samples);

#ifdef __cplusplus
}
#endif

#endif

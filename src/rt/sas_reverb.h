#ifndef PSP_RECOMP_SAS_REVERB_H
#define PSP_RECOMP_SAS_REVERB_H

#include <stddef.h>
#include <stdint.h>

#define SR_SAS_REVERB_WORKSPACE_SAMPLES 0x20000

typedef struct SrSasReverb {
    int16_t workspace[SR_SAS_REVERB_WORKSPACE_SAMPLES];
    int preset;
    int position;
} SrSasReverb;

void sr_sas_reverb_init(SrSasReverb *reverb);
void sr_sas_reverb_set_preset(SrSasReverb *reverb, int preset);
void sr_sas_reverb_process(SrSasReverb *reverb, int16_t *output,
                           const int16_t *input, size_t input_frames,
                           int volume_left, int volume_right);

#endif

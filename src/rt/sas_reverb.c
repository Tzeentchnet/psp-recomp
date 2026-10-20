/* PSP SAS reverb, modelled on PPSSPP's PSX SPU reverb implementation. */

#include "sas_reverb.h"

#include <string.h>

typedef struct SasReverbPreset {
    int32_t size;
    int16_t d_apf1, d_apf2, v_iir;
    int16_t v_comb1, v_comb2, v_comb3, v_comb4, v_wall;
    int16_t v_apf1, v_apf2;
    int16_t m_lsame, m_rsame, m_lcomb1, m_rcomb1, m_lcomb2, m_rcomb2;
    int16_t d_lsame, d_rsame, m_ldiff, m_rdiff;
    int16_t m_lcomb3, m_rcomb3, m_lcomb4, m_rcomb4;
    int16_t d_ldiff, d_rdiff, m_lapf1, m_rapf1, m_lapf2, m_rapf2;
} SasReverbPreset;

static const SasReverbPreset s_presets[9] = {
    {
        0x26C0,
        0x007D, 0x005B, 0x6D80,
        0x54B8, (int16_t)0xBED0, 0x0000, 0x0000, (int16_t)0xBA80,
        0x5800, 0x5300,
        0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
        0x0334, 0x01B5, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C,
    },
    {
        0x1F40,
        0x0033, 0x0025, 0x70F0,
        0x4FA8, (int16_t)0xBCE0, 0x4410, (int16_t)0xC0F0, (int16_t)0x9C00,
        0x5280, 0x4EC0,
        0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266,
        0x031C, 0x025D, 0x025C, 0x018E,
        0x022F, 0x0135, 0x01D2, 0x00B7,
        0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026,
    },
    {
        0x4840,
        0x00B1, 0x007F, 0x70F0,
        0x4FA8, (int16_t)0xBCE0, 0x4510, (int16_t)0xBEF0, (int16_t)0xB4C0,
        0x5280, 0x4EC0,
        0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616,
        0x076C, 0x05ED, 0x05EC, 0x042E,
        0x050F, 0x0305, 0x0462, 0x02B7,
        0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080,
    },
    {
        0x6FE0,
        0x00E3, 0x00A9, 0x6F60,
        0x4FA8, (int16_t)0xBCE0, 0x4510, (int16_t)0xBEF0, (int16_t)0xA680,
        0x5680, 0x52C0,
        0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973,
        0x0B59, 0x08DA, 0x08D9, 0x05E9,
        0x07EC, 0x04B0, 0x06EF, 0x03D2,
        0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA,
    },
    {
        0xADE0,
        0x01A5, 0x0139, 0x6000,
        0x5000, 0x4C00, (int16_t)0xB800, (int16_t)0xBC00, (int16_t)0xC000,
        0x6000, 0x5C00,
        0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1,
        0x11C0, 0x0DC3, 0x0DC0, 0x09C1,
        0x0BC4, 0x07C1, 0x0A00, 0x06CD,
        0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A,
    },
    {
        0xF6C0,
        0x033D, 0x0231, 0x7E00,
        0x5000, (int16_t)0xB400, (int16_t)0xB000, 0x4C00, (int16_t)0xB000,
        0x6000, 0x5400,
        0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2,
        0x1A32, 0x15EF, 0x15EE, 0x1055,
        0x1334, 0x0F2D, 0x11F6, 0x0C5D,
        0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232,
    },
    {
        0x18040,
        0x0001, 0x0001, 0x7FFF,
        0x7FFF, 0x0000, 0x0000, 0x0000, (int16_t)0xC080,
        0x0000, 0x0000,
        0x1FFF, 0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000,
        0x1005, 0x0005, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002,
    },
    {
        0x18040,
        0x0001, 0x0001, 0x7FFF,
        0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000,
        0x1FFF, 0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000,
        0x1005, 0x0005, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002,
    },
    {
        0x3C00,
        0x0017, 0x0013, 0x70F0,
        0x4FA8, (int16_t)0xBCE0, 0x4510, (int16_t)0xBEF0, (int16_t)0x8500,
        0x5F80, 0x54C0,
        0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7,
        0x0358, 0x026A, 0x01D6, 0x011E,
        0x012D, 0x00B1, 0x011F, 0x0059,
        0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014,
    },
};

static int16_t clamp_s16(int32_t value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return (int16_t)value;
}

static int workspace_index(const SrSasReverb *reverb, int offset, int used_size) {
    int index = reverb->position + offset;
    int base = SR_SAS_REVERB_WORKSPACE_SAMPLES - used_size;
    if (index >= SR_SAS_REVERB_WORKSPACE_SAMPLES) index -= used_size;
    if (index < base) index += used_size;
    return index;
}

static int16_t workspace_read(const SrSasReverb *reverb, int offset, int used_size) {
    return reverb->workspace[workspace_index(reverb, offset, used_size)];
}

static void workspace_write(SrSasReverb *reverb, int offset, int used_size,
                            int32_t value) {
    reverb->workspace[workspace_index(reverb, offset, used_size)] = clamp_s16(value);
}

void sr_sas_reverb_init(SrSasReverb *reverb) {
    memset(reverb, 0, sizeof(*reverb));
    reverb->preset = -1;
}

void sr_sas_reverb_set_preset(SrSasReverb *reverb, int preset) {
    reverb->preset = preset;
    memset(reverb->workspace, 0, sizeof(reverb->workspace));
    reverb->position = preset >= 0
        ? SR_SAS_REVERB_WORKSPACE_SAMPLES - s_presets[preset].size
        : 0;
}

void sr_sas_reverb_process(SrSasReverb *reverb, int16_t *output,
                           const int16_t *input, size_t input_frames,
                           int volume_left, int volume_right) {
    if (reverb->preset < 0) {
        for (size_t i = 0; i < input_frames; i++) {
            int16_t left = clamp_s16((int32_t)input[i * 2] * volume_left >> 15);
            int16_t right = clamp_s16((int32_t)input[i * 2 + 1] * volume_right >> 15);
            output[i * 4] = left;
            output[i * 4 + 1] = right;
            output[i * 4 + 2] = left;
            output[i * 4 + 3] = right;
        }
        return;
    }

    const SasReverbPreset *p = &s_presets[reverb->preset];
    for (size_t i = 0; i < input_frames; i++) {
        int32_t lin = input[i * 2] >> 1;
        int32_t rin = input[i * 2 + 1] >> 1;

        workspace_write(
            reverb, p->m_lsame, p->size,
            lin + (workspace_read(reverb, p->d_lsame, p->size) * p->v_wall >> 15) -
                (workspace_read(reverb, p->m_lsame - 1, p->size) * p->v_iir >> 15) +
                workspace_read(reverb, p->m_lsame - 1, p->size));
        workspace_write(
            reverb, p->m_rsame, p->size,
            rin + (workspace_read(reverb, p->d_rsame, p->size) * p->v_wall >> 15) -
                (workspace_read(reverb, p->m_rsame - 1, p->size) * p->v_iir >> 15) +
                workspace_read(reverb, p->m_rsame - 1, p->size));
        workspace_write(
            reverb, p->m_ldiff, p->size,
            lin + (workspace_read(reverb, p->d_rdiff, p->size) * p->v_wall >> 15) -
                (workspace_read(reverb, p->m_ldiff - 1, p->size) * p->v_iir >> 15) +
                workspace_read(reverb, p->m_ldiff - 1, p->size));
        workspace_write(
            reverb, p->m_rdiff, p->size,
            rin + (workspace_read(reverb, p->d_ldiff, p->size) * p->v_wall >> 15) -
                (workspace_read(reverb, p->m_rdiff - 1, p->size) * p->v_iir >> 15) +
                workspace_read(reverb, p->m_rdiff - 1, p->size));

        int32_t left =
            (p->v_comb1 * workspace_read(reverb, p->m_lcomb1, p->size) +
             p->v_comb2 * workspace_read(reverb, p->m_lcomb2, p->size) +
             p->v_comb3 * workspace_read(reverb, p->m_lcomb3, p->size) +
             p->v_comb4 * workspace_read(reverb, p->m_lcomb4, p->size)) >> 15;
        int32_t right =
            (p->v_comb1 * workspace_read(reverb, p->m_rcomb1, p->size) +
             p->v_comb2 * workspace_read(reverb, p->m_rcomb2, p->size) +
             p->v_comb3 * workspace_read(reverb, p->m_rcomb3, p->size) +
             p->v_comb4 * workspace_read(reverb, p->m_rcomb4, p->size)) >> 15;

        workspace_write(
            reverb, p->m_lapf1, p->size,
            left - (p->v_apf1 *
                    workspace_read(reverb, p->m_lapf1 - p->d_apf1, p->size) >> 15));
        left = workspace_read(reverb, p->m_lapf1 - p->d_apf1, p->size) +
            (workspace_read(reverb, p->m_lapf1, p->size) * p->v_apf1 >> 15);
        workspace_write(
            reverb, p->m_rapf1, p->size,
            right - (p->v_apf1 *
                     workspace_read(reverb, p->m_rapf1 - p->d_apf1, p->size) >> 15));
        right = workspace_read(reverb, p->m_rapf1 - p->d_apf1, p->size) +
            (workspace_read(reverb, p->m_rapf1, p->size) * p->v_apf1 >> 15);

        workspace_write(
            reverb, p->m_lapf2, p->size,
            left - (p->v_apf2 *
                    workspace_read(reverb, p->m_lapf2 - p->d_apf2, p->size) >> 15));
        left = workspace_read(reverb, p->m_lapf2 - p->d_apf2, p->size) +
            (workspace_read(reverb, p->m_lapf2, p->size) * p->v_apf2 >> 15);
        workspace_write(
            reverb, p->m_rapf2, p->size,
            right - (p->v_apf2 *
                     workspace_read(reverb, p->m_rapf2 - p->d_apf2, p->size) >> 15));
        right = workspace_read(reverb, p->m_rapf2 - p->d_apf2, p->size) +
            (workspace_read(reverb, p->m_rapf2, p->size) * p->v_apf2 >> 15);

        output[i * 4] = clamp_s16(left * volume_left >> 15);
        output[i * 4 + 1] = clamp_s16(right * volume_right >> 15);
        output[i * 4 + 2] = 0;
        output[i * 4 + 3] = 0;

        reverb->position++;
        if (reverb->position >= SR_SAS_REVERB_WORKSPACE_SAMPLES)
            reverb->position -= p->size;
    }
}

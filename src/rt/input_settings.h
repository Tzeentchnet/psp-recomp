#ifndef PSP_RECOMP_INPUT_SETTINGS_H
#define PSP_RECOMP_INPUT_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SR_INPUT_SETTINGS_ERROR = -1,
    SR_INPUT_SETTINGS_CLOSED = 0,
    SR_INPUT_SETTINGS_SAVED = 1,
    SR_INPUT_SETTINGS_LAUNCH = 2,
};

int sr_input_settings_show(const char *title, int allow_launch);

#ifdef __cplusplus
}
#endif

#endif

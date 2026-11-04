#ifndef PSP_RECOMP_INPUT_SCRIPT_H
#define PSP_RECOMP_INPUT_SCRIPT_H

#include <stddef.h>
#include <stdint.h>

enum { SR_ANALOG_SCRIPT_MAX_EVENTS = 256 };

typedef struct SrAnalogScriptEvent {
    uint32_t first_present;
    uint32_t end_present;
    uint8_t lx;
    uint8_t ly;
} SrAnalogScriptEvent;

typedef struct SrAnalogScript {
    SrAnalogScriptEvent events[SR_ANALOG_SCRIPT_MAX_EVENTS];
    uint32_t count;
} SrAnalogScript;

int sr_analog_script_parse(
    SrAnalogScript *script,
    const char *text,
    char *error,
    size_t error_size);
int sr_analog_script_load(
    SrAnalogScript *script,
    const char *path,
    char *error,
    size_t error_size);
int sr_analog_script_sample(
    const SrAnalogScript *script,
    uint32_t present,
    uint8_t *lx,
    uint8_t *ly);

#endif

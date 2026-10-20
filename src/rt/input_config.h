#ifndef PSP_RECOMP_INPUT_CONFIG_H
#define PSP_RECOMP_INPUT_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_INPUT_ACTIONS 12
#define SR_INPUT_ANALOG_DIRECTIONS 4
#define SR_INPUT_MAX_BINDINGS 4

enum SrInputAction {
    SR_INPUT_SELECT,
    SR_INPUT_START,
    SR_INPUT_UP,
    SR_INPUT_RIGHT,
    SR_INPUT_DOWN,
    SR_INPUT_LEFT,
    SR_INPUT_L,
    SR_INPUT_R,
    SR_INPUT_TRIANGLE,
    SR_INPUT_CIRCLE,
    SR_INPUT_CROSS,
    SR_INPUT_SQUARE,
};

enum SrInputAnalogDirection {
    SR_INPUT_ANALOG_UP,
    SR_INPUT_ANALOG_RIGHT,
    SR_INPUT_ANALOG_DOWN,
    SR_INPUT_ANALOG_LEFT,
};

enum SrInputKey {
    SR_KEY_NONE = 0,
    SR_KEY_ENTER = 256,
    SR_KEY_LEFT_SHIFT,
    SR_KEY_RIGHT_SHIFT,
    SR_KEY_UP,
    SR_KEY_RIGHT,
    SR_KEY_DOWN,
    SR_KEY_LEFT,
    SR_KEY_SPACE,
    SR_KEY_TAB,
    SR_KEY_BACKSPACE,
    SR_KEY_LEFT_CTRL,
    SR_KEY_RIGHT_CTRL,
};

enum SrInputPad {
    SR_PAD_NONE = 0,
    SR_PAD_SOUTH,
    SR_PAD_EAST,
    SR_PAD_WEST,
    SR_PAD_NORTH,
    SR_PAD_START,
    SR_PAD_BACK,
    SR_PAD_LEFT_SHOULDER,
    SR_PAD_RIGHT_SHOULDER,
    SR_PAD_LEFT_TRIGGER,
    SR_PAD_RIGHT_TRIGGER,
    SR_PAD_DPAD_UP,
    SR_PAD_DPAD_RIGHT,
    SR_PAD_DPAD_DOWN,
    SR_PAD_DPAD_LEFT,
    SR_PAD_LEFT_STICK,
    SR_PAD_RIGHT_STICK,
};

typedef struct SrInputBinding {
    int values[SR_INPUT_MAX_BINDINGS];
    int count;
} SrInputBinding;

typedef struct SrInputConfig {
    SrInputBinding keyboard[SR_INPUT_ACTIONS];
    SrInputBinding gamepad[SR_INPUT_ACTIONS];
    SrInputBinding analog_keyboard[SR_INPUT_ANALOG_DIRECTIONS];
    double deadzone;
    double curve;
    int invert_y;
} SrInputConfig;

typedef int (*SrInputDownFn)(void *context, int value);

void sr_input_config_defaults(SrInputConfig *config);
int sr_input_config_parse(
    SrInputConfig *config, const char *text, char *error, size_t error_size);
uint32_t sr_input_config_buttons(
    const SrInputConfig *config,
    SrInputDownFn key_down, void *key_context,
    SrInputDownFn pad_down, void *pad_context);
void sr_input_config_keyboard_axis(
    const SrInputConfig *config, SrInputDownFn key_down, void *context,
    int *x, int *y);
void sr_input_config_analog(
    const SrInputConfig *config, int raw_x, int raw_y,
    uint8_t *lx, uint8_t *ly);

int sr_input_init(void);
uint32_t sr_input_buttons(
    SrInputDownFn key_down, void *key_context,
    SrInputDownFn pad_down, void *pad_context);
void sr_input_keyboard_axis(
    SrInputDownFn key_down, void *context, int *x, int *y);
void sr_input_analog(int raw_x, int raw_y, uint8_t *lx, uint8_t *ly);
const char *sr_input_config_path(void);

#ifdef __cplusplus
}
#endif

#endif

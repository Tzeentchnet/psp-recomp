#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "input_config.h"
#include "settings_path.h"

#include <ctype.h>
#include <errno.h>
#include <io.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static const char *const s_action_names[SR_INPUT_ACTIONS] = {
    "select", "start", "up", "right", "down", "left",
    "l", "r", "triangle", "circle", "cross", "square",
};

static const uint32_t s_action_bits[SR_INPUT_ACTIONS] = {
    0x0001, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
    0x0100, 0x0200, 0x1000, 0x2000, 0x4000, 0x8000,
};

enum {
    SETTING_ANALOG_DEADZONE =
        SR_INPUT_ACTIONS * 3 + SR_INPUT_ANALOG_DIRECTIONS,
    SETTING_ANALOG_CURVE,
    SETTING_ANALOG_INVERT_Y,
    SETTING_TWIN_STICK_ENABLED,
    SETTING_MOVEMENT_DEADZONE,
    SETTING_MOVEMENT_CURVE,
    SETTING_MOVEMENT_INVERT_Y,
    SETTING_AIM_DEADZONE,
    SETTING_AIM_CURVE,
    SETTING_AIM_INVERT_X,
    SETTING_AIM_INVERT_Y,
};

#define PSP_CTRL_UP 0x0010u
#define PSP_CTRL_RIGHT 0x0020u
#define PSP_CTRL_DOWN 0x0040u
#define PSP_CTRL_LEFT 0x0080u
#define TWIN_STICK_DPAD_PRESS 32
#define TWIN_STICK_DPAD_RELEASE 24

static const char *const s_analog_names[SR_INPUT_ANALOG_DIRECTIONS] = {
    "analog_up", "analog_right", "analog_down", "analog_left",
};

typedef struct NameValue {
    const char *name;
    int value;
} NameValue;

static const NameValue s_key_names[] = {
    {"enter", SR_KEY_ENTER},
    {"left_shift", SR_KEY_LEFT_SHIFT},
    {"right_shift", SR_KEY_RIGHT_SHIFT},
    {"up", SR_KEY_UP},
    {"right", SR_KEY_RIGHT},
    {"down", SR_KEY_DOWN},
    {"left", SR_KEY_LEFT},
    {"space", SR_KEY_SPACE},
    {"tab", SR_KEY_TAB},
    {"backspace", SR_KEY_BACKSPACE},
    {"left_ctrl", SR_KEY_LEFT_CTRL},
    {"right_ctrl", SR_KEY_RIGHT_CTRL},
};

static const NameValue s_pad_names[] = {
    {"south", SR_PAD_SOUTH},
    {"east", SR_PAD_EAST},
    {"west", SR_PAD_WEST},
    {"north", SR_PAD_NORTH},
    {"start", SR_PAD_START},
    {"back", SR_PAD_BACK},
    {"left_shoulder", SR_PAD_LEFT_SHOULDER},
    {"right_shoulder", SR_PAD_RIGHT_SHOULDER},
    {"left_trigger", SR_PAD_LEFT_TRIGGER},
    {"right_trigger", SR_PAD_RIGHT_TRIGGER},
    {"dpad_up", SR_PAD_DPAD_UP},
    {"dpad_right", SR_PAD_DPAD_RIGHT},
    {"dpad_down", SR_PAD_DPAD_DOWN},
    {"dpad_left", SR_PAD_DPAD_LEFT},
    {"left_stick", SR_PAD_LEFT_STICK},
    {"right_stick", SR_PAD_RIGHT_STICK},
};

static const NameValue s_mouse_names[] = {
    {"left", SR_MOUSE_LEFT},
    {"right", SR_MOUSE_RIGHT},
    {"middle", SR_MOUSE_MIDDLE},
    {"x1", SR_MOUSE_X1},
    {"x2", SR_MOUSE_X2},
};

static SrInputConfig s_config;
static SrInputTwinStickState s_twin_stick_state;
static int s_initialized;
static char s_config_path[1024];
static HANDLE s_reload_thread;
static HANDLE s_reload_stop_event;
static volatile LONG s_reload_requested;

static int text_equal(const char *left, const char *right) {
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
            return 0;
        left++;
        right++;
    }
    return !*left && !*right;
}

static char *trim(char *text) {
    while (*text && isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = 0;
    return text;
}

static void set_error(
    char *error, size_t error_size, int line, const char *message,
    const char *detail) {
    if (!error || !error_size) return;
    if (detail)
        snprintf(error, error_size, "line %d: %s: %s", line, message, detail);
    else
        snprintf(error, error_size, "line %d: %s", line, message);
}

static void binding_set(
    SrInputBinding *binding, int a, int b, int c, int d) {
    int values[SR_INPUT_MAX_BINDINGS] = {a, b, c, d};
    binding->count = 0;
    for (int i = 0; i < SR_INPUT_MAX_BINDINGS; i++)
        if (values[i]) binding->values[binding->count++] = values[i];
}

void sr_input_config_defaults(SrInputConfig *config) {
    memset(config, 0, sizeof(*config));
    binding_set(&config->keyboard[SR_INPUT_SELECT],
                SR_KEY_LEFT_SHIFT, SR_KEY_RIGHT_SHIFT, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_START], SR_KEY_ENTER, 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_UP], SR_KEY_UP, 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_RIGHT], SR_KEY_RIGHT, 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_DOWN], SR_KEY_DOWN, 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_LEFT], SR_KEY_LEFT, 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_L], 'Q', 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_R], 'E', 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_TRIANGLE], 'R', 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_CIRCLE], 'G', 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_CROSS], SR_KEY_SPACE, 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_SQUARE], 'F', 0, 0, 0);

    binding_set(&config->mouse[SR_INPUT_L], SR_MOUSE_X2, 0, 0, 0);
    binding_set(&config->mouse[SR_INPUT_R], SR_MOUSE_RIGHT, 0, 0, 0);
    binding_set(&config->mouse[SR_INPUT_TRIANGLE], SR_MOUSE_MIDDLE, 0, 0, 0);
    binding_set(&config->mouse[SR_INPUT_CIRCLE], SR_MOUSE_X1, 0, 0, 0);
    binding_set(&config->mouse[SR_INPUT_SQUARE], SR_MOUSE_LEFT, 0, 0, 0);

    binding_set(&config->gamepad[SR_INPUT_SELECT], SR_PAD_BACK, 0, 0, 0);
    binding_set(&config->gamepad[SR_INPUT_START], SR_PAD_START, 0, 0, 0);
    binding_set(&config->gamepad[SR_INPUT_UP], SR_PAD_DPAD_UP, 0, 0, 0);
    binding_set(&config->gamepad[SR_INPUT_RIGHT], SR_PAD_DPAD_RIGHT, 0, 0, 0);
    binding_set(&config->gamepad[SR_INPUT_DOWN], SR_PAD_DPAD_DOWN, 0, 0, 0);
    binding_set(&config->gamepad[SR_INPUT_LEFT], SR_PAD_DPAD_LEFT, 0, 0, 0);
    binding_set(&config->gamepad[SR_INPUT_L],
                SR_PAD_LEFT_SHOULDER, SR_PAD_LEFT_TRIGGER, 0, 0);
    binding_set(&config->gamepad[SR_INPUT_R],
                SR_PAD_RIGHT_SHOULDER, SR_PAD_RIGHT_TRIGGER, 0, 0);
    binding_set(&config->gamepad[SR_INPUT_TRIANGLE], SR_PAD_NORTH, 0, 0, 0);
    binding_set(&config->gamepad[SR_INPUT_CIRCLE], SR_PAD_EAST, 0, 0, 0);
    binding_set(&config->gamepad[SR_INPUT_CROSS], SR_PAD_SOUTH, 0, 0, 0);
    binding_set(&config->gamepad[SR_INPUT_SQUARE], SR_PAD_WEST, 0, 0, 0);

    binding_set(&config->analog_keyboard[SR_INPUT_ANALOG_UP], 'W', 0, 0, 0);
    binding_set(&config->analog_keyboard[SR_INPUT_ANALOG_RIGHT], 'D', 0, 0, 0);
    binding_set(&config->analog_keyboard[SR_INPUT_ANALOG_DOWN], 'S', 0, 0, 0);
    binding_set(&config->analog_keyboard[SR_INPUT_ANALOG_LEFT], 'A', 0, 0, 0);
    config->deadzone = 0.24;
    config->curve = 1.0;
    config->invert_y = 0;
    config->twin_stick_enabled = 0;
    config->movement_deadzone = 0.24;
    config->movement_curve = 1.0;
    config->movement_invert_y = 0;
    config->aim_deadzone = 0.24;
    config->aim_curve = 1.0;
    config->aim_invert_x = 0;
    config->aim_invert_y = 0;
}

void sr_input_config_apply_twin_stick_defaults(SrInputConfig *config) {
    if (!config) return;
    binding_set(
        &config->gamepad[SR_INPUT_L],
        SR_PAD_LEFT_SHOULDER, 0, 0, 0);
    binding_set(
        &config->gamepad[SR_INPUT_R],
        SR_PAD_LEFT_TRIGGER, 0, 0, 0);
    binding_set(
        &config->gamepad[SR_INPUT_SQUARE],
        SR_PAD_WEST, SR_PAD_RIGHT_TRIGGER, 0, 0);
    config->twin_stick_enabled = 1;
    config->movement_deadzone = 0.24;
    config->movement_curve = 1.0;
    config->movement_invert_y = 0;
    config->aim_deadzone = 0.24;
    config->aim_curve = 1.0;
    config->aim_invert_x = 0;
    config->aim_invert_y = 1;
}

static void legacy_defaults(SrInputConfig *config) {
    sr_input_config_defaults(config);
    binding_set(&config->keyboard[SR_INPUT_R], 'W', 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_TRIANGLE], 'S', 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_CIRCLE], 'Z', 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_CROSS], 'X', 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_SQUARE], 'A', 0, 0, 0);
    binding_set(
        &config->analog_keyboard[SR_INPUT_ANALOG_UP], SR_KEY_UP, 0, 0, 0);
    binding_set(
        &config->analog_keyboard[SR_INPUT_ANALOG_RIGHT], SR_KEY_RIGHT, 0, 0, 0);
    binding_set(
        &config->analog_keyboard[SR_INPUT_ANALOG_DOWN], SR_KEY_DOWN, 0, 0, 0);
    binding_set(
        &config->analog_keyboard[SR_INPUT_ANALOG_LEFT], SR_KEY_LEFT, 0, 0, 0);
}

static int find_name(
    const NameValue *names, size_t count, const char *text) {
    for (size_t i = 0; i < count; i++)
        if (text_equal(text, names[i].name)) return names[i].value;
    return -1;
}

static int parse_key_token(const char *text) {
    if (text_equal(text, "none")) return SR_KEY_NONE;
    if (text[0] && !text[1] && isalnum((unsigned char)text[0]))
        return toupper((unsigned char)text[0]);
    return find_name(
        s_key_names, sizeof(s_key_names) / sizeof(s_key_names[0]), text);
}

static int parse_pad_token(const char *text) {
    if (text_equal(text, "none")) return SR_PAD_NONE;
    return find_name(
        s_pad_names, sizeof(s_pad_names) / sizeof(s_pad_names[0]), text);
}

static int parse_mouse_token(const char *text) {
    if (text_equal(text, "none")) return SR_MOUSE_NONE;
    return find_name(
        s_mouse_names, sizeof(s_mouse_names) / sizeof(s_mouse_names[0]), text);
}

static int parse_binding(
    SrInputBinding *binding, char *value, int binding_type,
    int line, char *error, size_t error_size) {
    binding->count = 0;
    char *cursor = value;
    while (*cursor) {
        char *comma = strchr(cursor, ',');
        if (comma) *comma = 0;
        char *token = trim(cursor);
        int parsed =
            binding_type == 1 ? parse_pad_token(token) :
            binding_type == 2 ? parse_mouse_token(token) :
            parse_key_token(token);
        if (parsed < 0) {
            set_error(error, error_size, line, "unknown binding", token);
            return 0;
        }
        if (parsed == 0) {
            if (binding->count || (comma && *trim(comma + 1))) {
                set_error(
                    error, error_size, line,
                    "'none' must be the only binding", NULL);
                return 0;
            }
        } else {
            for (int i = 0; i < binding->count; i++) {
                if (binding->values[i] == parsed) {
                    set_error(error, error_size, line, "duplicate binding", token);
                    return 0;
                }
            }
            if (binding->count >= SR_INPUT_MAX_BINDINGS) {
                set_error(error, error_size, line, "too many bindings", NULL);
                return 0;
            }
            binding->values[binding->count++] = parsed;
        }
        if (!comma) break;
        cursor = comma + 1;
    }
    return 1;
}

static int action_index(const char *name) {
    for (int i = 0; i < SR_INPUT_ACTIONS; i++)
        if (text_equal(name, s_action_names[i])) return i;
    return -1;
}

static int analog_index(const char *name) {
    for (int i = 0; i < SR_INPUT_ANALOG_DIRECTIONS; i++)
        if (text_equal(name, s_analog_names[i])) return i;
    return -1;
}

int sr_input_config_parse(
    SrInputConfig *config, const char *text, char *error, size_t error_size) {
    uint64_t seen = 0;
    int line_number = 0;
    const char *cursor = text;
    if (error && error_size) error[0] = 0;
    while (*cursor) {
        char line[512];
        size_t length = 0;
        while (cursor[length] && cursor[length] != '\n' && cursor[length] != '\r')
            length++;
        line_number++;
        if (length >= sizeof(line)) {
            set_error(error, error_size, line_number, "line is too long", NULL);
            return 0;
        }
        memcpy(line, cursor, length);
        line[length] = 0;
        cursor += length;
        while (*cursor == '\r' || *cursor == '\n') cursor++;

        char *entry = trim(line);
        if (!*entry || *entry == '#' || *entry == ';') continue;
        char *equals = strchr(entry, '=');
        if (!equals) {
            set_error(error, error_size, line_number, "missing '='", NULL);
            return 0;
        }
        *equals = 0;
        char *name = trim(entry);
        char *value = trim(equals + 1);
        if (!*name || !*value) {
            set_error(error, error_size, line_number, "empty name or value", NULL);
            return 0;
        }

        int setting = -1;
        SrInputBinding *binding = NULL;
        int binding_type = 0;
        if (strncmp(name, "keyboard.", 9) == 0) {
            const char *suffix = name + 9;
            int action = action_index(suffix);
            int analog = analog_index(suffix);
            if (action >= 0) {
                setting = action;
                binding = &config->keyboard[action];
            } else if (analog >= 0) {
                setting = SR_INPUT_ACTIONS * 3 + analog;
                binding = &config->analog_keyboard[analog];
            }
        } else if (strncmp(name, "gamepad.", 8) == 0) {
            int action = action_index(name + 8);
            if (action >= 0) {
                setting = SR_INPUT_ACTIONS + action;
                binding = &config->gamepad[action];
                binding_type = 1;
            }
        } else if (strncmp(name, "mouse.", 6) == 0) {
            int action = action_index(name + 6);
            if (action >= 0) {
                setting = SR_INPUT_ACTIONS * 2 + action;
                binding = &config->mouse[action];
                binding_type = 2;
            }
        } else if (text_equal(name, "analog.deadzone")) {
            setting = SETTING_ANALOG_DEADZONE;
        } else if (text_equal(name, "analog.curve")) {
            setting = SETTING_ANALOG_CURVE;
        } else if (text_equal(name, "analog.invert_y")) {
            setting = SETTING_ANALOG_INVERT_Y;
        } else if (text_equal(name, "twin_stick.enabled")) {
            setting = SETTING_TWIN_STICK_ENABLED;
        } else if (text_equal(name, "movement.deadzone")) {
            setting = SETTING_MOVEMENT_DEADZONE;
        } else if (text_equal(name, "movement.curve")) {
            setting = SETTING_MOVEMENT_CURVE;
        } else if (text_equal(name, "movement.invert_y")) {
            setting = SETTING_MOVEMENT_INVERT_Y;
        } else if (text_equal(name, "aim.deadzone")) {
            setting = SETTING_AIM_DEADZONE;
        } else if (text_equal(name, "aim.curve")) {
            setting = SETTING_AIM_CURVE;
        } else if (text_equal(name, "aim.invert_x")) {
            setting = SETTING_AIM_INVERT_X;
        } else if (text_equal(name, "aim.invert_y")) {
            setting = SETTING_AIM_INVERT_Y;
        }
        if (setting < 0) {
            set_error(error, error_size, line_number, "unknown setting", name);
            return 0;
        }
        if (seen & (1ull << setting)) {
            set_error(error, error_size, line_number, "duplicate setting", name);
            return 0;
        }
        seen |= 1ull << setting;

        if (binding) {
            if (!parse_binding(
                    binding, value, binding_type,
                    line_number, error, error_size))
                return 0;
        } else if (
            text_equal(name, "analog.invert_y") ||
            text_equal(name, "twin_stick.enabled") ||
            text_equal(name, "movement.invert_y") ||
            text_equal(name, "aim.invert_x") ||
            text_equal(name, "aim.invert_y")) {
            int parsed;
            if (text_equal(value, "true") || strcmp(value, "1") == 0)
                parsed = 1;
            else if (text_equal(value, "false") || strcmp(value, "0") == 0)
                parsed = 0;
            else {
                set_error(
                    error, error_size, line_number,
                    "expected true or false", value);
                return 0;
            }
            if (text_equal(name, "analog.invert_y"))
                config->invert_y = parsed;
            else if (text_equal(name, "twin_stick.enabled"))
                config->twin_stick_enabled = parsed;
            else if (text_equal(name, "movement.invert_y"))
                config->movement_invert_y = parsed;
            else if (text_equal(name, "aim.invert_x"))
                config->aim_invert_x = parsed;
            else
                config->aim_invert_y = parsed;
        } else {
            char *end = NULL;
            errno = 0;
            double parsed = strtod(value, &end);
            if (errno || !end || *trim(end) || !isfinite(parsed)) {
                set_error(error, error_size, line_number, "invalid number", value);
                return 0;
            }
            if (
                text_equal(name, "analog.deadzone") ||
                text_equal(name, "movement.deadzone") ||
                text_equal(name, "aim.deadzone")) {
                if (parsed < 0.0 || parsed > 0.95) {
                    set_error(
                        error, error_size, line_number,
                        "deadzone must be between 0 and 0.95", value);
                    return 0;
                }
                if (text_equal(name, "analog.deadzone"))
                    config->deadzone = parsed;
                else if (text_equal(name, "movement.deadzone"))
                    config->movement_deadzone = parsed;
                else
                    config->aim_deadzone = parsed;
            } else {
                if (parsed < 0.1 || parsed > 4.0) {
                    set_error(
                        error, error_size, line_number,
                        "curve must be between 0.1 and 4", value);
                    return 0;
                }
                if (text_equal(name, "analog.curve"))
                    config->curve = parsed;
                else if (text_equal(name, "movement.curve"))
                    config->movement_curve = parsed;
                else
                    config->aim_curve = parsed;
            }
        }
    }
    return 1;
}

static int binding_down(
    const SrInputBinding *binding, SrInputDownFn down, void *context) {
    if (!down) return 0;
    for (int i = 0; i < binding->count; i++)
        if (down(context, binding->values[i])) return 1;
    return 0;
}

uint32_t sr_input_config_buttons(
    const SrInputConfig *config,
    SrInputDownFn key_down, void *key_context,
    SrInputDownFn mouse_down, void *mouse_context,
    SrInputDownFn pad_down, void *pad_context) {
    uint32_t buttons = 0;
    for (int i = 0; i < SR_INPUT_ACTIONS; i++) {
        if (binding_down(&config->keyboard[i], key_down, key_context) ||
            binding_down(&config->mouse[i], mouse_down, mouse_context) ||
            binding_down(&config->gamepad[i], pad_down, pad_context))
            buttons |= s_action_bits[i];
    }
    return buttons;
}

void sr_input_config_keyboard_axis(
    const SrInputConfig *config, SrInputDownFn key_down, void *context,
    int *x, int *y) {
    int left = binding_down(
        &config->analog_keyboard[SR_INPUT_ANALOG_LEFT], key_down, context);
    int right = binding_down(
        &config->analog_keyboard[SR_INPUT_ANALOG_RIGHT], key_down, context);
    int up = binding_down(
        &config->analog_keyboard[SR_INPUT_ANALOG_UP], key_down, context);
    int down = binding_down(
        &config->analog_keyboard[SR_INPUT_ANALOG_DOWN], key_down, context);
    if (x) *x = right == left ? 0 : right ? 32767 : -32768;
    if (y) *y = down == up ? 0 : down ? 32767 : -32768;
}

static void transform_analog(
    double deadzone, double curve, int invert_x, int invert_y,
    int raw_x, int raw_y, uint8_t *lx, uint8_t *ly) {
    double x = raw_x < 0 ? raw_x / 32768.0 : raw_x / 32767.0;
    double y = raw_y < 0 ? raw_y / 32768.0 : raw_y / 32767.0;
    if (invert_x) x = -x;
    if (invert_y) y = -y;
    double magnitude = sqrt(x * x + y * y);
    if (magnitude <= deadzone || magnitude == 0.0) {
        if (lx) *lx = 128;
        if (ly) *ly = 128;
        return;
    }
    double direction_magnitude = magnitude;
    if (magnitude > 1.0) magnitude = 1.0;
    double scaled = (magnitude - deadzone) / (1.0 - deadzone);
    if (scaled < 0.0) scaled = 0.0;
    if (scaled > 1.0) scaled = 1.0;
    scaled = pow(scaled, curve);
    double factor = scaled / direction_magnitude;
    int output_x = (int)(127.5 + x * factor * 127.5 + 0.5);
    int output_y = (int)(127.5 + y * factor * 127.5 + 0.5);
    if (output_x < 0) output_x = 0;
    if (output_x > 255) output_x = 255;
    if (output_y < 0) output_y = 0;
    if (output_y > 255) output_y = 255;
    if (lx) *lx = (uint8_t)output_x;
    if (ly) *ly = (uint8_t)output_y;
}

void sr_input_config_analog(
    const SrInputConfig *config, int raw_x, int raw_y,
    uint8_t *lx, uint8_t *ly) {
    transform_analog(
        config->deadzone, config->curve, 0, config->invert_y,
        raw_x, raw_y, lx, ly);
}

static uint32_t dpad_axis(
    uint32_t previous, int value, uint32_t negative, uint32_t positive) {
    if (value <= -TWIN_STICK_DPAD_PRESS) return negative;
    if (value >= TWIN_STICK_DPAD_PRESS) return positive;
    if ((previous & negative) && value <= -TWIN_STICK_DPAD_RELEASE)
        return negative;
    if ((previous & positive) && value >= TWIN_STICK_DPAD_RELEASE)
        return positive;
    return 0;
}

static float twin_stick_axis(uint8_t value) {
    int centered = (int)value - 128;
    return centered < 0 ? centered / 128.0f : centered / 127.0f;
}

uint32_t sr_input_config_gamepad_sticks(
    const SrInputConfig *config,
    SrInputTwinStickState *state,
    uint32_t buttons,
    const SrInputSticks *sticks,
    uint8_t *lx,
    uint8_t *ly) {
    if (!config->twin_stick_enabled) {
        memset(state, 0, sizeof(*state));
        sr_input_config_analog(
            config, sticks->movement_x, sticks->movement_y, lx, ly);
        return buttons;
    }

    uint8_t movement_x;
    uint8_t movement_y;
    transform_analog(
        config->movement_deadzone,
        config->movement_curve,
        0,
        config->movement_invert_y,
        sticks->movement_x,
        sticks->movement_y,
        &movement_x,
        &movement_y);
    transform_analog(
        config->aim_deadzone,
        config->aim_curve,
        config->aim_invert_x,
        config->aim_invert_y,
        sticks->aim_x,
        sticks->aim_y,
        lx,
        ly);
    state->aim_x = twin_stick_axis(lx ? *lx : 128);
    state->aim_y = twin_stick_axis(ly ? *ly : 128);
    state->aim_active =
        state->aim_x * state->aim_x + state->aim_y * state->aim_y >
        0.0001f;

    uint32_t previous = state->dpad_buttons;
    state->dpad_buttons =
        dpad_axis(
            previous, (int)movement_x - 128,
            PSP_CTRL_LEFT, PSP_CTRL_RIGHT) |
        dpad_axis(
            previous, (int)movement_y - 128,
            PSP_CTRL_UP, PSP_CTRL_DOWN);
    return buttons | state->dpad_buttons;
}

const char *sr_input_action_name(int action) {
    if (action < 0 || action >= SR_INPUT_ACTIONS) return NULL;
    return s_action_names[action];
}

static const char *binding_value_name(
    const NameValue *names,
    size_t count,
    int value,
    char single_character[2]) {
    for (size_t i = 0; i < count; i++)
        if (names[i].value == value) return names[i].name;
    if (value > 0 && value < 128 && isalnum((unsigned char)value)) {
        single_character[0] = (char)value;
        single_character[1] = 0;
        return single_character;
    }
    return NULL;
}

static int format_binding(
    const SrInputBinding *binding,
    const NameValue *names,
    size_t name_count,
    int allow_single_character,
    char *text,
    size_t text_size) {
    size_t used = 0;
    if (!text || !text_size) return 0;
    text[0] = 0;
    if (!binding->count) {
        if (text_size < sizeof("none")) return 0;
        strcpy(text, "none");
        return 1;
    }
    for (int i = 0; i < binding->count; i++) {
        char single_character[2];
        const char *name = binding_value_name(
            names,
            name_count,
            binding->values[i],
            single_character);
        size_t length;
        if (!name || (!allow_single_character && name == single_character))
            return 0;
        length = strlen(name);
        if (used + (i ? 2u : 0u) + length + 1u > text_size) return 0;
        if (i) {
            memcpy(text + used, ", ", 2);
            used += 2;
        }
        memcpy(text + used, name, length);
        used += length;
        text[used] = 0;
    }
    return 1;
}

int sr_input_config_format_gamepad_binding(
    const SrInputConfig *config,
    int action,
    char *text,
    size_t text_size) {
    if (!config || action < 0 || action >= SR_INPUT_ACTIONS) return 0;
    return format_binding(
        &config->gamepad[action],
        s_pad_names,
        sizeof(s_pad_names) / sizeof(s_pad_names[0]),
        0,
        text,
        text_size);
}

static int write_binding(
    FILE *file,
    const char *prefix,
    const char *name,
    const SrInputBinding *binding,
    const NameValue *names,
    size_t name_count,
    int allow_single_character) {
    char text[128];
    if (!format_binding(
            binding,
            names,
            name_count,
            allow_single_character,
            text,
            sizeof(text))) {
        return 0;
    }
    return fprintf(file, "%s.%s = %s\n", prefix, name, text) > 0;
}

static int write_config(FILE *file, const SrInputConfig *config) {
    if (fprintf(file, "# KZL input config v4\n") <= 0) return 0;
    for (int i = 0; i < SR_INPUT_ACTIONS; i++) {
        if (!write_binding(
                file,
                "keyboard",
                s_action_names[i],
                &config->keyboard[i],
                s_key_names,
                sizeof(s_key_names) / sizeof(s_key_names[0]),
                1)) {
            return 0;
        }
    }
    for (int i = 0; i < SR_INPUT_ANALOG_DIRECTIONS; i++) {
        if (!write_binding(
                file,
                "keyboard",
                s_analog_names[i],
                &config->analog_keyboard[i],
                s_key_names,
                sizeof(s_key_names) / sizeof(s_key_names[0]),
                1)) {
            return 0;
        }
    }
    for (int i = 0; i < SR_INPUT_ACTIONS; i++) {
        if (!write_binding(
                file,
                "mouse",
                s_action_names[i],
                &config->mouse[i],
                s_mouse_names,
                sizeof(s_mouse_names) / sizeof(s_mouse_names[0]),
                0)) {
            return 0;
        }
    }
    for (int i = 0; i < SR_INPUT_ACTIONS; i++) {
        if (!write_binding(
                file,
                "gamepad",
                s_action_names[i],
                &config->gamepad[i],
                s_pad_names,
                sizeof(s_pad_names) / sizeof(s_pad_names[0]),
                0)) {
            return 0;
        }
    }
    return fprintf(
               file,
               "analog.deadzone = %.6g\n"
               "analog.curve = %.6g\n"
               "analog.invert_y = %s\n"
               "# Experimental gamepad-only twin-stick controls (default off)\n"
               "twin_stick.enabled = %s\n"
               "movement.deadzone = %.6g\n"
               "movement.curve = %.6g\n"
               "movement.invert_y = %s\n"
               "aim.deadzone = %.6g\n"
               "aim.curve = %.6g\n"
               "aim.invert_x = %s\n"
               "aim.invert_y = %s\n",
               config->deadzone,
               config->curve,
               config->invert_y ? "true" : "false",
               config->twin_stick_enabled ? "true" : "false",
               config->movement_deadzone,
               config->movement_curve,
               config->movement_invert_y ? "true" : "false",
               config->aim_deadzone,
               config->aim_curve,
               config->aim_invert_x ? "true" : "false",
               config->aim_invert_y ? "true" : "false") > 0;
}

static int write_default_config(const char *path) {
    SrInputConfig defaults;
    FILE *file = fopen(path, "wb");
    int ok;
    if (!file) return 0;
    sr_input_config_defaults(&defaults);
    ok = write_config(file, &defaults);
    if (fclose(file) != 0) ok = 0;
    return ok;
}

static char *read_config(const char *path, char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        snprintf(error, error_size, "cannot open %s", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        snprintf(error, error_size, "cannot seek %s", path);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || size > 65536) {
        fclose(file);
        snprintf(error, error_size, "input config is too large: %s", path);
        return NULL;
    }
    rewind(file);
    char *text = (char *)malloc((size_t)size + 1u);
    if (!text) {
        fclose(file);
        snprintf(error, error_size, "out of memory reading %s", path);
        return NULL;
    }
    if (fread(text, 1, (size_t)size, file) != (size_t)size || ferror(file)) {
        free(text);
        fclose(file);
        snprintf(error, error_size, "cannot read %s", path);
        return NULL;
    }
    text[size] = 0;
    fclose(file);
    return text;
}

static int read_file_attributes(
    const char *path, WIN32_FILE_ATTRIBUTE_DATA *attributes) {
    return GetFileAttributesExA(
        path, GetFileExInfoStandard, attributes) != 0;
}

static int same_file_version(
    const WIN32_FILE_ATTRIBUTE_DATA *left,
    const WIN32_FILE_ATTRIBUTE_DATA *right) {
    return CompareFileTime(
               &left->ftLastWriteTime,
               &right->ftLastWriteTime) == 0 &&
           left->nFileSizeHigh == right->nFileSizeHigh &&
           left->nFileSizeLow == right->nFileSizeLow;
}

static int binding_equal(
    const SrInputBinding *left, const SrInputBinding *right) {
    if (left->count != right->count) return 0;
    for (int i = 0; i < left->count; i++)
        if (left->values[i] != right->values[i]) return 0;
    return 1;
}

static int is_legacy_default_config(const SrInputConfig *config) {
    SrInputConfig legacy;
    legacy_defaults(&legacy);
    for (int i = 0; i < SR_INPUT_ACTIONS; i++) {
        if (!binding_equal(&config->keyboard[i], &legacy.keyboard[i]) ||
            !binding_equal(&config->gamepad[i], &legacy.gamepad[i])) {
            return 0;
        }
    }
    for (int i = 0; i < SR_INPUT_ANALOG_DIRECTIONS; i++) {
        if (!binding_equal(
                &config->analog_keyboard[i],
                &legacy.analog_keyboard[i])) {
            return 0;
        }
    }
    return config->deadzone == legacy.deadzone &&
           config->curve == legacy.curve &&
           config->invert_y == legacy.invert_y &&
           config->twin_stick_enabled == legacy.twin_stick_enabled &&
           config->movement_deadzone == legacy.movement_deadzone &&
           config->movement_curve == legacy.movement_curve &&
           config->movement_invert_y == legacy.movement_invert_y &&
           config->aim_deadzone == legacy.aim_deadzone &&
           config->aim_curve == legacy.aim_curve &&
           config->aim_invert_x == legacy.aim_invert_x &&
           config->aim_invert_y == legacy.aim_invert_y;
}

static int is_default_config(const SrInputConfig *config) {
    SrInputConfig defaults;
    sr_input_config_defaults(&defaults);
    for (int i = 0; i < SR_INPUT_ACTIONS; i++) {
        if (!binding_equal(&config->keyboard[i], &defaults.keyboard[i]) ||
            !binding_equal(&config->mouse[i], &defaults.mouse[i]) ||
            !binding_equal(&config->gamepad[i], &defaults.gamepad[i])) {
            return 0;
        }
    }
    for (int i = 0; i < SR_INPUT_ANALOG_DIRECTIONS; i++) {
        if (!binding_equal(
                &config->analog_keyboard[i],
                &defaults.analog_keyboard[i])) {
            return 0;
        }
    }
    return config->deadzone == defaults.deadzone &&
           config->curve == defaults.curve &&
           config->invert_y == defaults.invert_y &&
           config->twin_stick_enabled == defaults.twin_stick_enabled &&
           config->movement_deadzone == defaults.movement_deadzone &&
           config->movement_curve == defaults.movement_curve &&
           config->movement_invert_y == defaults.movement_invert_y &&
           config->aim_deadzone == defaults.aim_deadzone &&
           config->aim_curve == defaults.aim_curve &&
           config->aim_invert_x == defaults.aim_invert_x &&
           config->aim_invert_y == defaults.aim_invert_y;
}

int sr_input_init(void) {
    char error[512];
    if (s_initialized) return 1;
    sr_input_config_defaults(&s_config);
    const char *override = getenv("SR_INPUT_CONFIG");
    if (override && strcmp(override, "0") == 0) {
        s_config_path[0] = 0;
        s_initialized = 1;
        fprintf(stderr, "input: defaults (config disabled by SR_INPUT_CONFIG=0)\n");
        return 1;
    }

    int required = override && override[0];
    if (required) {
        if (strlen(override) >= sizeof(s_config_path)) {
            fprintf(stderr, "input: SR_INPUT_CONFIG path is too long\n");
            return 0;
        }
        strcpy(s_config_path, override);
    } else {
        if (!sr_settings_path(
                "input.ini", s_config_path, sizeof(s_config_path),
                error, sizeof(error))) {
            fprintf(stderr, "input: %s\n", error);
            return 0;
        }
        FILE *existing = fopen(s_config_path, "rb");
        if (!existing) {
            if (!write_default_config(s_config_path)) {
                fprintf(
                    stderr, "input: cannot write default %s; using defaults\n",
                    s_config_path);
                s_config_path[0] = 0;
            } else {
                fprintf(stderr, "input: wrote default config %s\n", s_config_path);
            }
            s_initialized = 1;
            return 1;
        }
        fclose(existing);
    }

    char *text = read_config(s_config_path, error, sizeof(error));
    if (!text) {
        fprintf(stderr, "input: %s\n", error);
        return 0;
    }
    SrInputConfig parsed;
    sr_input_config_defaults(&parsed);
    int legacy_version = strstr(text, "# KZL input config v1") != NULL;
    int previous_version = strstr(text, "# KZL input config v2") != NULL;
    int version_three = strstr(text, "# KZL input config v3") != NULL;
    int ok = sr_input_config_parse(&parsed, text, error, sizeof(error));
    free(text);
    if (!ok) {
        fprintf(stderr, "input: invalid %s: %s\n", s_config_path, error);
        return 0;
    }
    if (!required &&
        ((legacy_version && is_legacy_default_config(&parsed)) ||
         ((previous_version || version_three) &&
          is_default_config(&parsed)))) {
        sr_input_config_defaults(&parsed);
        if (!write_default_config(s_config_path)) {
            fprintf(
                stderr,
                "input: cannot write migrated %s; using migrated defaults\n",
                s_config_path);
        } else {
            fprintf(
                stderr, "input: migrated default config to v4 %s\n",
                s_config_path);
        }
    }
    s_config = parsed;
    s_initialized = 1;
    fprintf(
        stderr,
        "input: loaded %s (deadzone %.3f, curve %.3f, invert_y %d, "
        "twin_stick %d)\n",
        s_config_path,
        s_config.deadzone,
        s_config.curve,
        s_config.invert_y,
        s_config.twin_stick_enabled);
    return 1;
}

uint32_t sr_input_buttons(
    SrInputDownFn key_down, void *key_context,
    SrInputDownFn mouse_down, void *mouse_context,
    SrInputDownFn pad_down, void *pad_context) {
    return sr_input_config_buttons(
        &s_config, key_down, key_context,
        mouse_down, mouse_context, pad_down, pad_context);
}

void sr_input_keyboard_axis(
    SrInputDownFn key_down, void *context, int *x, int *y) {
    sr_input_config_keyboard_axis(&s_config, key_down, context, x, y);
}

void sr_input_analog(int raw_x, int raw_y, uint8_t *lx, uint8_t *ly) {
    sr_input_config_analog(&s_config, raw_x, raw_y, lx, ly);
}

uint32_t sr_input_gamepad_sticks(
    uint32_t buttons,
    const SrInputSticks *sticks,
    uint8_t *lx,
    uint8_t *ly) {
    uint8_t output_lx = 128;
    uint8_t output_ly = 128;
    uint32_t result = sr_input_config_gamepad_sticks(
        &s_config,
        &s_twin_stick_state,
        buttons,
        sticks,
        &output_lx,
        &output_ly);
    if (lx) *lx = output_lx;
    if (ly) *ly = output_ly;
    return result;
}

void sr_input_gamepad_disconnected(void) {
    memset(&s_twin_stick_state, 0, sizeof(s_twin_stick_state));
}

int sr_input_twin_stick_enabled(void) {
    return s_config.twin_stick_enabled;
}

int sr_input_twin_stick_aim(float *x, float *y) {
    if (!s_twin_stick_state.aim_active) return 0;
    if (x) *x = s_twin_stick_state.aim_x;
    if (y) *y = s_twin_stick_state.aim_y;
    return 1;
}

const SrInputConfig *sr_input_config_get(void) {
    return s_initialized ? &s_config : NULL;
}

int sr_input_config_save(
    const SrInputConfig *config,
    char *error,
    size_t error_size) {
    char temporary[1100];
    FILE *file;
    int length;
    int ok;

    if (error && error_size) error[0] = 0;
    if (!s_initialized || !config || !s_config_path[0]) {
        set_error(
            error,
            error_size,
            0,
            "input config cannot be saved",
            NULL);
        return 0;
    }
    length = snprintf(
        temporary, sizeof(temporary), "%s.tmp", s_config_path);
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        set_error(
            error,
            error_size,
            0,
            "temporary input config path is too long",
            NULL);
        return 0;
    }
    file = fopen(temporary, "wb");
    if (!file) {
        set_error(
            error,
            error_size,
            0,
            "cannot create temporary input config",
            temporary);
        return 0;
    }
    ok = write_config(file, config) &&
         fflush(file) == 0 &&
         _commit(_fileno(file)) == 0;
    if (fclose(file) != 0) ok = 0;
    if (!ok) {
        DeleteFileA(temporary);
        set_error(
            error,
            error_size,
            0,
            "cannot write temporary input config",
            temporary);
        return 0;
    }
    if (!MoveFileExA(
            temporary,
            s_config_path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        char detail[256];
        snprintf(
            detail,
            sizeof(detail),
            "%s (error %lu)",
            s_config_path,
            (unsigned long)GetLastError());
        DeleteFileA(temporary);
        set_error(
            error,
            error_size,
            0,
            "cannot publish input config",
            detail);
        return 0;
    }
    s_config = *config;
    memset(&s_twin_stick_state, 0, sizeof(s_twin_stick_state));
    fprintf(stderr, "input: saved %s\n", s_config_path);
    return 1;
}

int sr_input_config_reload(char *error, size_t error_size) {
    SrInputConfig parsed;
    char *text;
    int ok;

    if (error && error_size) error[0] = 0;
    if (!s_initialized || !s_config_path[0]) {
        set_error(
            error,
            error_size,
            0,
            "input config cannot be reloaded",
            NULL);
        return 0;
    }
    text = read_config(s_config_path, error, error_size);
    if (!text) return 0;
    sr_input_config_defaults(&parsed);
    ok = sr_input_config_parse(
        &parsed, text, error, error_size);
    free(text);
    if (!ok) return 0;
    s_config = parsed;
    memset(&s_twin_stick_state, 0, sizeof(s_twin_stick_state));
    return 1;
}

static DWORD WINAPI input_reload_thread(void *context) {
    WIN32_FILE_ATTRIBUTE_DATA observed;
    WIN32_FILE_ATTRIBUTE_DATA current;
    int observed_valid;
    (void)context;

    observed_valid =
        read_file_attributes(s_config_path, &observed);
    for (;;) {
        DWORD wait = WaitForSingleObject(
            s_reload_stop_event, 250u);
        int current_valid;
        int changed;
        if (wait == WAIT_OBJECT_0) return 0;
        if (wait == WAIT_FAILED) {
            fprintf(
                stderr,
                "input: hot reload watcher wait failed (error %lu)\n",
                (unsigned long)GetLastError());
            return 1;
        }
        current_valid =
            read_file_attributes(s_config_path, &current);
        changed =
            current_valid != observed_valid ||
            (current_valid &&
             !same_file_version(&current, &observed));
        if (!changed) continue;
        observed_valid = current_valid;
        if (current_valid) observed = current;
        InterlockedExchange(&s_reload_requested, 1);
    }
}

int sr_input_start_hot_reload(void) {
    const char *override;
    if (!s_initialized) {
        fprintf(
            stderr,
            "input: hot reload requires initialized config\n");
        return 0;
    }
    override = getenv("SR_INPUT_HOT_RELOAD");
    if (override && strcmp(override, "0") == 0) {
        fprintf(
            stderr,
            "input: hot reload disabled by SR_INPUT_HOT_RELOAD=0\n");
        return 1;
    }
    if (!s_config_path[0] || s_reload_thread) return 1;
    s_reload_stop_event = CreateEventA(
        NULL, TRUE, FALSE, NULL);
    if (!s_reload_stop_event) {
        fprintf(
            stderr,
            "input: cannot create hot reload stop event (error %lu)\n",
            (unsigned long)GetLastError());
        return 0;
    }
    s_reload_thread = CreateThread(
        NULL, 0, input_reload_thread, NULL, 0, NULL);
    if (!s_reload_thread) {
        fprintf(
            stderr,
            "input: cannot start hot reload watcher (error %lu)\n",
            (unsigned long)GetLastError());
        CloseHandle(s_reload_stop_event);
        s_reload_stop_event = NULL;
        return 0;
    }
    atexit(sr_input_stop_hot_reload);
    fprintf(stderr, "input: hot reload watcher active\n");
    return 1;
}

void sr_input_stop_hot_reload(void) {
    if (!s_reload_thread) return;
    SetEvent(s_reload_stop_event);
    WaitForSingleObject(s_reload_thread, INFINITE);
    CloseHandle(s_reload_thread);
    CloseHandle(s_reload_stop_event);
    s_reload_thread = NULL;
    s_reload_stop_event = NULL;
    InterlockedExchange(&s_reload_requested, 0);
}

void sr_input_poll_reload(void) {
    char error[512];

    if (!s_initialized ||
        !s_config_path[0] ||
        InterlockedExchange(&s_reload_requested, 0) == 0) {
        return;
    }
    if (!sr_input_config_reload(error, sizeof(error))) {
        fprintf(
            stderr,
            "input: hot reload rejected %s: %s "
            "(keeping previous settings)\n",
            s_config_path,
            error);
        return;
    }
    fprintf(
        stderr,
        "input: hot reloaded %s "
        "(twin_stick %d, aim invert_x %d, invert_y %d)\n",
        s_config_path,
        s_config.twin_stick_enabled,
        s_config.aim_invert_x,
        s_config.aim_invert_y);
}

const char *sr_input_config_path(void) {
    return s_config_path;
}

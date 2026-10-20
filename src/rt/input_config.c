#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "input_config.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

static const char *const s_action_names[SR_INPUT_ACTIONS] = {
    "select", "start", "up", "right", "down", "left",
    "l", "r", "triangle", "circle", "cross", "square",
};

static const uint32_t s_action_bits[SR_INPUT_ACTIONS] = {
    0x0001, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
    0x0100, 0x0200, 0x1000, 0x2000, 0x4000, 0x8000,
};

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

static SrInputConfig s_config;
static int s_initialized;
static char s_config_path[1024];

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
    binding_set(&config->keyboard[SR_INPUT_R], 'W', 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_TRIANGLE], 'S', 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_CIRCLE], 'Z', 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_CROSS], 'X', 0, 0, 0);
    binding_set(&config->keyboard[SR_INPUT_SQUARE], 'A', 0, 0, 0);

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

    binding_set(&config->analog_keyboard[SR_INPUT_ANALOG_UP], SR_KEY_UP, 0, 0, 0);
    binding_set(&config->analog_keyboard[SR_INPUT_ANALOG_RIGHT], SR_KEY_RIGHT, 0, 0, 0);
    binding_set(&config->analog_keyboard[SR_INPUT_ANALOG_DOWN], SR_KEY_DOWN, 0, 0, 0);
    binding_set(&config->analog_keyboard[SR_INPUT_ANALOG_LEFT], SR_KEY_LEFT, 0, 0, 0);
    config->deadzone = 0.24;
    config->curve = 1.0;
    config->invert_y = 0;
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

static int parse_binding(
    SrInputBinding *binding, char *value, int gamepad,
    int line, char *error, size_t error_size) {
    binding->count = 0;
    char *cursor = value;
    while (*cursor) {
        char *comma = strchr(cursor, ',');
        if (comma) *comma = 0;
        char *token = trim(cursor);
        int parsed = gamepad ? parse_pad_token(token) : parse_key_token(token);
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
        int gamepad = 0;
        if (strncmp(name, "keyboard.", 9) == 0) {
            const char *suffix = name + 9;
            int action = action_index(suffix);
            int analog = analog_index(suffix);
            if (action >= 0) {
                setting = action;
                binding = &config->keyboard[action];
            } else if (analog >= 0) {
                setting = SR_INPUT_ACTIONS * 2 + analog;
                binding = &config->analog_keyboard[analog];
            }
        } else if (strncmp(name, "gamepad.", 8) == 0) {
            int action = action_index(name + 8);
            if (action >= 0) {
                setting = SR_INPUT_ACTIONS + action;
                binding = &config->gamepad[action];
                gamepad = 1;
            }
        } else if (text_equal(name, "analog.deadzone")) {
            setting = SR_INPUT_ACTIONS * 2 + SR_INPUT_ANALOG_DIRECTIONS;
        } else if (text_equal(name, "analog.curve")) {
            setting = SR_INPUT_ACTIONS * 2 + SR_INPUT_ANALOG_DIRECTIONS + 1;
        } else if (text_equal(name, "analog.invert_y")) {
            setting = SR_INPUT_ACTIONS * 2 + SR_INPUT_ANALOG_DIRECTIONS + 2;
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
                    binding, value, gamepad, line_number, error, error_size))
                return 0;
        } else if (text_equal(name, "analog.invert_y")) {
            if (text_equal(value, "true") || strcmp(value, "1") == 0)
                config->invert_y = 1;
            else if (text_equal(value, "false") || strcmp(value, "0") == 0)
                config->invert_y = 0;
            else {
                set_error(
                    error, error_size, line_number,
                    "expected true or false", value);
                return 0;
            }
        } else {
            char *end = NULL;
            errno = 0;
            double parsed = strtod(value, &end);
            if (errno || !end || *trim(end) || !isfinite(parsed)) {
                set_error(error, error_size, line_number, "invalid number", value);
                return 0;
            }
            if (text_equal(name, "analog.deadzone")) {
                if (parsed < 0.0 || parsed > 0.95) {
                    set_error(
                        error, error_size, line_number,
                        "deadzone must be between 0 and 0.95", value);
                    return 0;
                }
                config->deadzone = parsed;
            } else {
                if (parsed < 0.1 || parsed > 4.0) {
                    set_error(
                        error, error_size, line_number,
                        "curve must be between 0.1 and 4", value);
                    return 0;
                }
                config->curve = parsed;
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
    SrInputDownFn pad_down, void *pad_context) {
    uint32_t buttons = 0;
    for (int i = 0; i < SR_INPUT_ACTIONS; i++) {
        if (binding_down(&config->keyboard[i], key_down, key_context) ||
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

void sr_input_config_analog(
    const SrInputConfig *config, int raw_x, int raw_y,
    uint8_t *lx, uint8_t *ly) {
    double x = raw_x < 0 ? raw_x / 32768.0 : raw_x / 32767.0;
    double y = raw_y < 0 ? raw_y / 32768.0 : raw_y / 32767.0;
    if (config->invert_y) y = -y;
    double magnitude = sqrt(x * x + y * y);
    if (magnitude <= config->deadzone || magnitude == 0.0) {
        if (lx) *lx = 128;
        if (ly) *ly = 128;
        return;
    }
    double direction_magnitude = magnitude;
    if (magnitude > 1.0) magnitude = 1.0;
    double scaled =
        (magnitude - config->deadzone) / (1.0 - config->deadzone);
    if (scaled < 0.0) scaled = 0.0;
    if (scaled > 1.0) scaled = 1.0;
    scaled = pow(scaled, config->curve);
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

static int write_default_config(const char *path) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    static const char text[] =
        "# KZL input config v1\n"
        "keyboard.select = left_shift, right_shift\n"
        "keyboard.start = enter\n"
        "keyboard.up = up\n"
        "keyboard.right = right\n"
        "keyboard.down = down\n"
        "keyboard.left = left\n"
        "keyboard.l = Q\n"
        "keyboard.r = W\n"
        "keyboard.triangle = S\n"
        "keyboard.circle = Z\n"
        "keyboard.cross = X\n"
        "keyboard.square = A\n"
        "keyboard.analog_up = up\n"
        "keyboard.analog_right = right\n"
        "keyboard.analog_down = down\n"
        "keyboard.analog_left = left\n"
        "gamepad.select = back\n"
        "gamepad.start = start\n"
        "gamepad.up = dpad_up\n"
        "gamepad.right = dpad_right\n"
        "gamepad.down = dpad_down\n"
        "gamepad.left = dpad_left\n"
        "gamepad.l = left_shoulder, left_trigger\n"
        "gamepad.r = right_shoulder, right_trigger\n"
        "gamepad.triangle = north\n"
        "gamepad.circle = east\n"
        "gamepad.cross = south\n"
        "gamepad.square = west\n"
        "analog.deadzone = 0.24\n"
        "analog.curve = 1.0\n"
        "analog.invert_y = false\n";
    int ok = fwrite(text, 1, sizeof(text) - 1, file) == sizeof(text) - 1;
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

int sr_input_init(void) {
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
        const char *local = getenv("LOCALAPPDATA");
        if (!local || !local[0]) {
            s_config_path[0] = 0;
            s_initialized = 1;
            fprintf(stderr, "input: LOCALAPPDATA is unset; using defaults\n");
            return 1;
        }
        int length = snprintf(
            s_config_path, sizeof(s_config_path),
            "%s\\KZLRecomp\\input.ini", local);
        if (length < 0 || (size_t)length >= sizeof(s_config_path)) {
            fprintf(stderr, "input: default config path is too long\n");
            return 0;
        }
#ifdef _WIN32
        char directory[1024];
        snprintf(directory, sizeof(directory), "%s\\KZLRecomp", local);
        if (!CreateDirectoryA(directory, NULL) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            fprintf(
                stderr, "input: cannot create %s (error %lu); using defaults\n",
                directory, (unsigned long)GetLastError());
            s_config_path[0] = 0;
            s_initialized = 1;
            return 1;
        }
#endif
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

    char error[512];
    char *text = read_config(s_config_path, error, sizeof(error));
    if (!text) {
        fprintf(stderr, "input: %s\n", error);
        return 0;
    }
    SrInputConfig parsed;
    sr_input_config_defaults(&parsed);
    int ok = sr_input_config_parse(&parsed, text, error, sizeof(error));
    free(text);
    if (!ok) {
        fprintf(stderr, "input: invalid %s: %s\n", s_config_path, error);
        return 0;
    }
    s_config = parsed;
    s_initialized = 1;
    fprintf(
        stderr, "input: loaded %s (deadzone %.3f, curve %.3f, invert_y %d)\n",
        s_config_path, s_config.deadzone, s_config.curve, s_config.invert_y);
    return 1;
}

uint32_t sr_input_buttons(
    SrInputDownFn key_down, void *key_context,
    SrInputDownFn pad_down, void *pad_context) {
    return sr_input_config_buttons(
        &s_config, key_down, key_context, pad_down, pad_context);
}

void sr_input_keyboard_axis(
    SrInputDownFn key_down, void *context, int *x, int *y) {
    sr_input_config_keyboard_axis(&s_config, key_down, context, x, y);
}

void sr_input_analog(int raw_x, int raw_y, uint8_t *lx, uint8_t *ly) {
    sr_input_config_analog(&s_config, raw_x, raw_y, lx, ly);
}

const char *sr_input_config_path(void) {
    return s_config_path;
}

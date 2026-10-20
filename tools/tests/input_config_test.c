#include "input_config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int failures;

static void check_u32(const char *name, uint32_t got, uint32_t expected) {
    if (got != expected) {
        fprintf(
            stderr, "FAIL: %s: got 0x%08x, expected 0x%08x\n",
            name, got, expected);
        failures++;
    }
}

static void check_true(const char *name, int value) {
    if (!value) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static int fake_down(void *context, int value) {
    const uint8_t *state = (const uint8_t *)context;
    return value >= 0 && value < 512 && state[value];
}

int main(void) {
    SrInputConfig config;
    uint8_t keys[512] = {0};
    uint8_t pad[512] = {0};
    char error[256];

    sr_input_config_defaults(&config);
    keys['X'] = 1;
    keys[SR_KEY_UP] = 1;
    check_u32(
        "default keyboard buttons",
        sr_input_config_buttons(
            &config, fake_down, keys, fake_down, pad),
        0x4010u);
    int x = 1, y = 1;
    sr_input_config_keyboard_axis(&config, fake_down, keys, &x, &y);
    check_u32("default keyboard analog x", (uint32_t)x, 0);
    check_u32("default keyboard analog y", (uint32_t)y, (uint32_t)-32768);

    memset(keys, 0, sizeof(keys));
    pad[SR_PAD_SOUTH] = 1;
    pad[SR_PAD_LEFT_TRIGGER] = 1;
    check_u32(
        "default gamepad buttons",
        sr_input_config_buttons(
            &config, fake_down, keys, fake_down, pad),
        0x4100u);

    uint8_t lx = 0, ly = 0;
    sr_input_config_analog(&config, 0, 0, &lx, &ly);
    check_u32("center analog x", lx, 128);
    check_u32("center analog y", ly, 128);
    sr_input_config_analog(&config, 7000, 0, &lx, &ly);
    check_u32("deadzone analog x", lx, 128);
    check_u32("deadzone analog y", ly, 128);
    sr_input_config_analog(&config, 32767, -32768, &lx, &ly);
    check_u32("clamped diagonal x", lx, 218);
    check_u32("clamped diagonal y", ly, 37);

    const char custom[] =
        "keyboard.cross = C\n"
        "keyboard.select = none\n"
        "keyboard.analog_right = D\n"
        "gamepad.cross = north\n"
        "gamepad.triangle = none\n"
        "analog.deadzone = 0.25\n"
        "analog.curve = 2.0\n"
        "analog.invert_y = true\n";
    sr_input_config_defaults(&config);
    check_true(
        "parse custom config",
        sr_input_config_parse(&config, custom, error, sizeof(error)));
    memset(keys, 0, sizeof(keys));
    memset(pad, 0, sizeof(pad));
    keys['C'] = 1;
    pad[SR_PAD_NORTH] = 1;
    check_u32(
        "custom cross bindings",
        sr_input_config_buttons(
            &config, fake_down, keys, fake_down, pad),
        0x4000u);
    keys['C'] = 0;
    keys['D'] = 1;
    sr_input_config_keyboard_axis(&config, fake_down, keys, &x, &y);
    check_u32("custom analog key x", (uint32_t)x, 32767);
    check_u32("custom analog key y", (uint32_t)y, 0);
    sr_input_config_analog(&config, 16384, 0, &lx, &ly);
    check_u32("quadratic curve x", lx, 142);
    check_u32("quadratic curve y", ly, 128);
    sr_input_config_analog(&config, 0, -32768, &lx, &ly);
    check_u32("inverted analog y", ly, 255);

    sr_input_config_defaults(&config);
    check_true(
        "reject duplicate setting",
        !sr_input_config_parse(
            &config,
            "keyboard.cross=X\nkeyboard.cross=Z\n",
            error, sizeof(error)));
    check_true("duplicate diagnostic", strstr(error, "duplicate setting") != NULL);
    sr_input_config_defaults(&config);
    check_true(
        "reject unknown binding",
        !sr_input_config_parse(
            &config, "gamepad.cross=banana\n", error, sizeof(error)));
    sr_input_config_defaults(&config);
    check_true(
        "reject deadzone range",
        !sr_input_config_parse(
            &config, "analog.deadzone=0.96\n", error, sizeof(error)));
    sr_input_config_defaults(&config);
    check_true(
        "reject nonfinite curve",
        !sr_input_config_parse(
            &config, "analog.curve=nan\n", error, sizeof(error)));
    sr_input_config_defaults(&config);
    check_true(
        "reject mixed none",
        !sr_input_config_parse(
            &config, "keyboard.cross=none,X\n", error, sizeof(error)));

    char temp_dir[MAX_PATH];
    char config_path[MAX_PATH];
    DWORD temp_length = GetTempPathA(sizeof(temp_dir), temp_dir);
    check_true(
        "temporary config directory",
        temp_length > 0 && temp_length < sizeof(temp_dir));
    snprintf(
        config_path, sizeof(config_path), "%skzl-input-%lu.ini",
        temp_dir, (unsigned long)GetCurrentProcessId());
    FILE *file = fopen(config_path, "wb");
    check_true("create temporary config", file != NULL);
    if (file) {
        check_true(
            "write temporary config",
            fwrite(custom, 1, sizeof(custom) - 1, file) == sizeof(custom) - 1);
        check_true("close temporary config", fclose(file) == 0);
        _putenv_s("SR_INPUT_CONFIG", config_path);
        check_true("initialize input config", sr_input_init());
        check_true(
            "loaded config path",
            strcmp(sr_input_config_path(), config_path) == 0);
        memset(keys, 0, sizeof(keys));
        memset(pad, 0, sizeof(pad));
        keys['C'] = 1;
        check_u32(
            "loaded keyboard binding",
            sr_input_buttons(fake_down, keys, fake_down, pad), 0x4000u);
        check_true("remove temporary config", remove(config_path) == 0);
    }

    if (failures) {
        fprintf(stderr, "input config tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("input config tests passed\n");
    return 0;
}

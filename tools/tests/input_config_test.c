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
    uint8_t mouse[512] = {0};
    uint8_t pad[512] = {0};
    char error[256];

    sr_input_config_defaults(&config);
    keys[SR_KEY_SPACE] = 1;
    keys[SR_KEY_UP] = 1;
    keys['W'] = 1;
    check_u32(
        "default keyboard buttons",
        sr_input_config_buttons(
            &config, fake_down, keys, fake_down, mouse, fake_down, pad),
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
            &config, fake_down, keys, fake_down, mouse, fake_down, pad),
        0x4100u);
    memset(pad, 0, sizeof(pad));
    mouse[SR_MOUSE_LEFT] = 1;
    mouse[SR_MOUSE_RIGHT] = 1;
    mouse[SR_MOUSE_MIDDLE] = 1;
    mouse[SR_MOUSE_X1] = 1;
    mouse[SR_MOUSE_X2] = 1;
    check_u32(
        "default mouse buttons",
        sr_input_config_buttons(
            &config, fake_down, keys, fake_down, mouse, fake_down, pad),
        0xb300u);

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
        "mouse.r = none\n"
        "mouse.cross = right\n"
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
    memset(mouse, 0, sizeof(mouse));
    memset(pad, 0, sizeof(pad));
    keys['C'] = 1;
    mouse[SR_MOUSE_RIGHT] = 1;
    pad[SR_PAD_NORTH] = 1;
    check_u32(
        "custom cross bindings",
        sr_input_config_buttons(
            &config, fake_down, keys, fake_down, mouse, fake_down, pad),
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
        "reject unknown mouse binding",
        !sr_input_config_parse(
            &config, "mouse.square=wheel_up\n", error, sizeof(error)));
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
    char config_dir[MAX_PATH];
    char config_path[MAX_PATH];
    DWORD temp_length = GetTempPathA(sizeof(temp_dir), temp_dir);
    check_true(
        "temporary config directory",
        temp_length > 0 && temp_length < sizeof(temp_dir));
    snprintf(
        config_dir, sizeof(config_dir), "%skzl-input-%lu",
        temp_dir, (unsigned long)GetCurrentProcessId());
    check_true(
        "create temporary input root",
        CreateDirectoryA(config_dir, NULL) ||
            GetLastError() == ERROR_ALREADY_EXISTS);
    strncat(config_dir, "\\KZLRecomp",
            sizeof(config_dir) - strlen(config_dir) - 1);
    check_true(
        "create temporary input config directory",
        CreateDirectoryA(config_dir, NULL) ||
            GetLastError() == ERROR_ALREADY_EXISTS);
    snprintf(
        config_path, sizeof(config_path), "%s\\input.ini", config_dir);
    FILE *file = fopen(config_path, "wb");
    check_true("create temporary config", file != NULL);
    if (file) {
        static const char legacy[] =
            "# KZL input config v1\n"
            "keyboard.r = W\n"
            "keyboard.triangle = S\n"
            "keyboard.circle = Z\n"
            "keyboard.cross = X\n"
            "keyboard.square = A\n"
            "keyboard.analog_up = up\n"
            "keyboard.analog_right = right\n"
            "keyboard.analog_down = down\n"
            "keyboard.analog_left = left\n";
        check_true(
            "write temporary config",
            fwrite(legacy, 1, sizeof(legacy) - 1, file) == sizeof(legacy) - 1);
        check_true("close temporary config", fclose(file) == 0);
        _putenv_s("SR_INPUT_CONFIG", "");
        _putenv_s("SR_SETTINGS_DIR", config_dir);
        check_true("initialize input config", sr_input_init());
        check_true(
            "loaded config path",
            strcmp(sr_input_config_path(), config_path) == 0);
        memset(keys, 0, sizeof(keys));
        memset(mouse, 0, sizeof(mouse));
        memset(pad, 0, sizeof(pad));
        keys['W'] = 1;
        mouse[SR_MOUSE_LEFT] = 1;
        pad[SR_PAD_SOUTH] = 1;
        check_u32(
            "loaded migrated mouse and gamepad bindings",
            sr_input_buttons(
                fake_down, keys, fake_down, mouse, fake_down, pad),
            0xc000u);
        sr_input_keyboard_axis(fake_down, keys, &x, &y);
        check_u32("migrated WASD analog x", (uint32_t)x, 0);
        check_u32(
            "migrated WASD analog y", (uint32_t)y, (uint32_t)-32768);
        file = fopen(config_path, "rb");
        check_true("open migrated config", file != NULL);
        if (file) {
            char migrated[4096];
            size_t length = fread(migrated, 1, sizeof(migrated) - 1, file);
            migrated[length] = 0;
            check_true("close migrated config", fclose(file) == 0);
            check_true(
                "migrated config version",
                strstr(migrated, "# KZL input config v2") != NULL);
            check_true(
                "migrated mouse fire binding",
                strstr(migrated, "mouse.square = left") != NULL);
        }
        check_true("remove temporary config", remove(config_path) == 0);
        check_true(
            "remove temporary config directory",
            RemoveDirectoryA(config_dir) != 0);
        {
            char local_root[MAX_PATH];
            snprintf(
                local_root, sizeof(local_root), "%skzl-input-%lu",
                temp_dir, (unsigned long)GetCurrentProcessId());
            check_true(
                "remove temporary input root",
                RemoveDirectoryA(local_root) != 0);
        }
    }

    if (failures) {
        fprintf(stderr, "input config tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("input config tests passed\n");
    return 0;
}

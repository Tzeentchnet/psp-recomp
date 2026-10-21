#include "input_config.h"

#include <math.h>
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

static void check_float_near(
    const char *name, float got, float expected, float tolerance) {
    if (fabsf(got - expected) > tolerance) {
        fprintf(
            stderr,
            "FAIL: %s: got %.6f, expected %.6f\n",
            name,
            got,
            expected);
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
    check_true("twin stick disabled by default", !config.twin_stick_enabled);

    SrInputSticks sticks = {32767, -32768, -32768, 32767};
    SrInputTwinStickState twin_state = {0xffffffffu};
    uint32_t stick_buttons = sr_input_config_gamepad_sticks(
        &config, &twin_state, 0x0100u, &sticks, &lx, &ly);
    check_u32("disabled twin stick buttons unchanged", stick_buttons, 0x0100u);
    check_u32("disabled twin stick state reset", twin_state.dpad_buttons, 0);
    check_true("disabled twin stick aim inactive", !twin_state.aim_active);
    check_u32("disabled twin stick uses movement x", lx, 218);
    check_u32("disabled twin stick uses movement y", ly, 37);

    sr_input_config_defaults(&config);
    sr_input_config_apply_twin_stick_defaults(&config);
    check_true("twin stick preset enables mode", config.twin_stick_enabled);
    check_true(
        "twin stick preset aim direction",
        !config.aim_invert_x && config.aim_invert_y);
    check_true(
        "format twin stick aim binding",
        sr_input_config_format_gamepad_binding(
            &config, SR_INPUT_R, error, sizeof(error)));
    check_true(
        "twin stick aim binding",
        strcmp(error, "left_trigger") == 0);
    check_true(
        "format twin stick fire binding",
        sr_input_config_format_gamepad_binding(
            &config, SR_INPUT_SQUARE, error, sizeof(error)));
    check_true(
        "twin stick fire binding",
        strcmp(error, "west, right_trigger") == 0);

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
        "analog.invert_y = true\n"
        "twin_stick.enabled = true\n"
        "movement.deadzone = 0.0\n"
        "movement.curve = 1.0\n"
        "movement.invert_y = false\n"
        "aim.deadzone = 0.0\n"
        "aim.curve = 1.0\n"
        "aim.invert_x = true\n"
        "aim.invert_y = true\n";
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

    memset(&twin_state, 0, sizeof(twin_state));
    sticks.movement_x = 32767;
    sticks.movement_y = -32768;
    sticks.aim_x = 32767;
    sticks.aim_y = -32768;
    stick_buttons = sr_input_config_gamepad_sticks(
        &config, &twin_state, 0x0100u, &sticks, &lx, &ly);
    check_u32(
        "twin stick synthesizes movement dpad",
        stick_buttons,
        0x0130u);
    check_u32("twin stick aim horizontal inversion", lx, 37);
    check_u32("twin stick aim inversion", ly, 218);
    check_true("twin stick aim side channel active", twin_state.aim_active);
    check_float_near("twin stick aim side channel x", twin_state.aim_x, -0.71f, 0.02f);
    check_float_near("twin stick aim side channel y", twin_state.aim_y, 0.71f, 0.02f);

    sticks.movement_x = 9000;
    sticks.movement_y = 0;
    sr_input_config_gamepad_sticks(
        &config, &twin_state, 0, &sticks, &lx, &ly);
    check_u32(
        "twin stick dpad press",
        twin_state.dpad_buttons,
        0x0020u);
    sticks.movement_x = 7000;
    sr_input_config_gamepad_sticks(
        &config, &twin_state, 0, &sticks, &lx, &ly);
    check_u32(
        "twin stick dpad hysteresis hold",
        twin_state.dpad_buttons,
        0x0020u);
    sticks.movement_x = 5000;
    sr_input_config_gamepad_sticks(
        &config, &twin_state, 0, &sticks, &lx, &ly);
    check_u32(
        "twin stick dpad hysteresis release",
        twin_state.dpad_buttons,
        0);
    sticks.aim_x = 0;
    sticks.aim_y = 0;
    sr_input_config_gamepad_sticks(
        &config, &twin_state, 0, &sticks, &lx, &ly);
    check_true("centered twin stick aim inactive", !twin_state.aim_active);

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
        "reject aim deadzone range",
        !sr_input_config_parse(
            &config, "aim.deadzone=1.0\n", error, sizeof(error)));
    sr_input_config_defaults(&config);
    check_true(
        "reject twin stick boolean",
        !sr_input_config_parse(
            &config, "twin_stick.enabled=maybe\n", error, sizeof(error)));
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
                strstr(migrated, "# KZL input config v4") != NULL);
            check_true(
                "migrated mouse fire binding",
                strstr(migrated, "mouse.square = left") != NULL);
            check_true(
                "migrated twin stick default",
                strstr(migrated, "twin_stick.enabled = false") != NULL);
            check_true(
                "migrated horizontal aim default",
                strstr(migrated, "aim.invert_x = false") != NULL);

            SrInputConfig saved = *sr_input_config_get();
            char binding[128];
            saved.gamepad[SR_INPUT_SQUARE].values[0] = SR_PAD_WEST;
            saved.gamepad[SR_INPUT_SQUARE].values[1] =
                SR_PAD_RIGHT_TRIGGER;
            saved.gamepad[SR_INPUT_SQUARE].count = 2;
            saved.aim_invert_x = 1;
            check_true(
                "format gamepad binding",
                sr_input_config_format_gamepad_binding(
                    &saved,
                    SR_INPUT_SQUARE,
                    binding,
                    sizeof(binding)));
            check_true(
                "formatted gamepad binding value",
                strcmp(binding, "west, right_trigger") == 0);
            check_true(
                "save input config",
                sr_input_config_save(&saved, error, sizeof(error)));
            file = fopen(config_path, "rb");
            check_true("open saved input config", file != NULL);
            if (file) {
                char saved_text[4096];
                size_t saved_length =
                    fread(saved_text, 1, sizeof(saved_text) - 1, file);
                saved_text[saved_length] = 0;
                check_true("close saved input config", fclose(file) == 0);
                check_true(
                    "saved trigger fire binding",
                    strstr(
                        saved_text,
                        "gamepad.square = west, right_trigger") != NULL);
                check_true(
                    "saved horizontal aim inversion",
                    strstr(saved_text, "aim.invert_x = true") != NULL);
            }

            file = fopen(config_path, "wb");
            check_true("open external reload config", file != NULL);
            if (file) {
                static const char reloaded[] =
                    "# external input edit\n"
                    "gamepad.square = right_trigger\n"
                    "twin_stick.enabled = true\n"
                    "aim.invert_x = false\n"
                    "aim.invert_y = true\n";
                check_true(
                    "write external reload config",
                    fwrite(
                        reloaded,
                        1,
                        sizeof(reloaded) - 1,
                        file) == sizeof(reloaded) - 1);
                check_true(
                    "close external reload config",
                    fclose(file) == 0);
            }
            check_true(
                "reload external input config",
                sr_input_config_reload(error, sizeof(error)));
            const SrInputConfig *reloaded_config =
                sr_input_config_get();
            check_true(
                "reloaded twin stick",
                reloaded_config->twin_stick_enabled);
            check_true(
                "reloaded aim inversion",
                !reloaded_config->aim_invert_x &&
                    reloaded_config->aim_invert_y);
            check_true(
                "format reloaded fire binding",
                sr_input_config_format_gamepad_binding(
                    reloaded_config,
                    SR_INPUT_SQUARE,
                    binding,
                    sizeof(binding)));
            check_true(
                "reloaded fire binding value",
                strcmp(binding, "right_trigger") == 0);

            file = fopen(config_path, "wb");
            check_true("open invalid reload config", file != NULL);
            if (file) {
                static const char invalid_reload[] =
                    "aim.invert_x = maybe\n";
                check_true(
                    "write invalid reload config",
                    fwrite(
                        invalid_reload,
                        1,
                        sizeof(invalid_reload) - 1,
                        file) == sizeof(invalid_reload) - 1);
                check_true(
                    "close invalid reload config",
                    fclose(file) == 0);
            }
            check_true(
                "reject invalid input reload",
                !sr_input_config_reload(error, sizeof(error)));
            check_true(
                "invalid reload diagnostic",
                strstr(error, "expected true or false") != NULL);
            check_true(
                "invalid reload preserves config",
                sr_input_config_get()->twin_stick_enabled &&
                    !sr_input_config_get()->aim_invert_x &&
                    sr_input_config_get()->aim_invert_y);

            check_true(
                "start hot reload watcher",
                sr_input_start_hot_reload());
            Sleep(50);
            file = fopen(config_path, "wb");
            check_true("open watched reload config", file != NULL);
            if (file) {
                static const char watched_reload[] =
                    "twin_stick.enabled = true\n"
                    "aim.invert_x = true\n"
                    "aim.invert_y = false\n";
                check_true(
                    "write watched reload config",
                    fwrite(
                        watched_reload,
                        1,
                        sizeof(watched_reload) - 1,
                        file) == sizeof(watched_reload) - 1);
                check_true(
                    "close watched reload config",
                    fclose(file) == 0);
            }
            Sleep(350);
            sr_input_poll_reload();
            check_true(
                "watched reload applied",
                sr_input_config_get()->twin_stick_enabled &&
                    sr_input_config_get()->aim_invert_x &&
                    !sr_input_config_get()->aim_invert_y);
            sr_input_stop_hot_reload();
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

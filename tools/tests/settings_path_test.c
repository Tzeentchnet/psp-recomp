#include "platform.h"
#include "settings_path.h"
#include "test_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check_true(const char *name, int value) {
    if (!value) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static void check_string(
    const char *name, const char *value, const char *expected) {
    if (strcmp(value, expected) != 0) {
        fprintf(
            stderr, "FAIL: %s: got \"%s\", expected \"%s\"\n",
            name, value, expected);
        failures++;
    }
}

int main(void) {
    char root[SR_TEST_PATH_MAX];
    char settings[SR_TEST_PATH_MAX];
    char resources[SR_TEST_PATH_MAX];
    char expected[SR_TEST_PATH_MAX];
    char path[SR_TEST_PATH_MAX];
    char data_root[SR_TEST_PATH_MAX];
    char error[512];
    FILE *file;

    if (!sr_test_temp_path(root, sizeof(root), "kzl-paths", ""))
        return 2;
    if (!sr_test_mkdir(root)) return 2;
    snprintf(settings, sizeof(settings), "%s/settings", root);
    snprintf(resources, sizeof(resources), "%s/resources", root);
    if (!sr_test_mkdir(settings) || !sr_test_mkdir(resources))
        return 2;

    check_true(
        "set settings override",
        sr_test_set_env("SR_SETTINGS_DIR", settings));
    check_true(
        "resolve settings override",
        sr_settings_path(
            "nested/config.ini", path, sizeof(path),
            error, sizeof(error)));
    snprintf(expected, sizeof(expected), "%s/nested/config.ini", settings);
    check_string("settings override path", path, expected);
    file = fopen(path, "wb");
    check_true("settings parent directory exists", file != NULL);
    if (file) {
        fputs("settings\n", file);
        fclose(file);
    }
    check_true(
        "reject settings traversal",
        !sr_settings_path(
            "../config.ini", path, sizeof(path),
            error, sizeof(error)));

    check_true(
        "set resource override",
        sr_test_set_env("SR_RESOURCE_DIR", resources));
    check_true(
        "resolve resource override",
        sr_resource_path(
            "font/ltn0.pgf", path, sizeof(path),
            error, sizeof(error)));
    snprintf(expected, sizeof(expected), "%s/font/ltn0.pgf", resources);
    check_string("resource override path", path, expected);
    check_true(
        "reject resource traversal",
        !sr_resource_path(
            "assets/../secret", path, sizeof(path),
            error, sizeof(error)));

#ifdef _WIN32
    check_true(
        "set local app data",
        sr_test_set_env("LOCALAPPDATA", root));
#else
    check_true("set home", sr_test_set_env("HOME", root));
    check_true(
        "set XDG data root",
        sr_test_set_env("XDG_DATA_HOME", root));
#endif
    check_true(
        "resolve application data root",
        sr_data_root(
            data_root, sizeof(data_root), error, sizeof(error)));
#ifdef __APPLE__
    snprintf(
        expected, sizeof(expected),
        "%s/Library/Application Support/KZLRecomp", root);
#else
    snprintf(expected, sizeof(expected), "%s/KZLRecomp", root);
#endif
    check_string("application data root", data_root, expected);
    check_true(
        "resolve nested application data",
        sr_data_path(
            "ms0/PSP/test.bin", path, sizeof(path),
            error, sizeof(error)));
    file = fopen(path, "wb");
    check_true("application data parent exists", file != NULL);
    if (file) {
        fputs("data\n", file);
        fclose(file);
    }

#ifdef __APPLE__
    check_true(
        "clear settings override",
        sr_test_set_env("SR_SETTINGS_DIR", NULL));
    check_true(
        "resolve default Mac settings",
        sr_settings_path(
            "graphics-test.ini", path, sizeof(path),
            error, sizeof(error)));
    snprintf(expected, sizeof(expected), "%s/graphics-test.ini", data_root);
    check_string("default Mac settings path", path, expected);
#endif

    remove(path);
    snprintf(path, sizeof(path), "%s/ms0/PSP/test.bin", data_root);
    remove(path);
    snprintf(path, sizeof(path), "%s/ms0/PSP", data_root);
    sr_test_rmdir(path);
    snprintf(path, sizeof(path), "%s/ms0", data_root);
    sr_test_rmdir(path);
    sr_test_rmdir(data_root);
#ifdef __APPLE__
    snprintf(path, sizeof(path), "%s/Library/Application Support", root);
    sr_test_rmdir(path);
    snprintf(path, sizeof(path), "%s/Library", root);
    sr_test_rmdir(path);
#endif
    snprintf(path, sizeof(path), "%s/nested/config.ini", settings);
    remove(path);
    snprintf(path, sizeof(path), "%s/nested", settings);
    sr_test_rmdir(path);
    sr_test_rmdir(settings);
    sr_test_rmdir(resources);
    sr_test_rmdir(root);

    sr_test_set_env("SR_SETTINGS_DIR", NULL);
    sr_test_set_env("SR_RESOURCE_DIR", NULL);

    if (failures) {
        fprintf(
            stderr, "settings path tests: %d failure(s)\n",
            failures);
        return 1;
    }
    printf("settings path tests passed\n");
    return 0;
}

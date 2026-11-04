#include "startup_check.h"
#include "test_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(const char *name, int value, int expected) {
    if (value != expected) {
        fprintf(stderr, "FAIL: %s: got %d, expected %d\n", name, value, expected);
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
    char path[SR_TEST_PATH_MAX];
    char config_dir[SR_TEST_PATH_MAX];
    char config_path[SR_TEST_PATH_MAX];
    char resolved[SR_TEST_PATH_MAX];
    char error[512];
    if (!sr_test_temp_path(path, sizeof(path), "kzl-startup", ".iso"))
        return 2;
    if (!sr_test_temp_path(
            config_dir, sizeof(config_dir), "kzl-startup-config", ""))
        return 2;
    if (!sr_test_mkdir(config_dir)) return 2;
    if (snprintf(
            config_path, sizeof(config_path), "%s%slauncher.ini",
            config_dir, SR_TEST_PATH_SEPARATOR) < 0) {
        return 2;
    }
    if (!sr_test_set_env("SR_SETTINGS_DIR", config_dir)) return 2;

    FILE *file = fopen(path, "wb");
    if (!file) return 2;
    fwrite("abc", 1, 3, file);
    fclose(file);
    if (!sr_test_set_env("PSP_ISO", path)) return 2;
    check("valid hash", sr_startup_validate(), 1);

    file = fopen(path, "wb");
    if (!file) return 2;
    fwrite("abcd", 1, 4, file);
    fclose(file);
    check("wrong size", sr_startup_validate(), 0);

    remove(path);
    check("missing file", sr_startup_validate(), 0);
    if (!sr_test_set_env("PSP_ISO", NULL)) return 2;
    check("missing setting", sr_startup_validate(), 0);

    file = fopen(path, "wb");
    if (!file) return 2;
    fwrite("abc", 1, 3, file);
    fclose(file);
    check(
        "persist valid ISO",
        sr_startup_save_iso_path(path, error, sizeof(error)),
        1);
    check(
        "resolve persisted ISO",
        sr_startup_resolve_iso_path(
            resolved, sizeof(resolved), error, sizeof(error)),
        1);
    check_string("persisted ISO path", resolved, path);
    check("validate persisted ISO", sr_startup_validate(), 1);

    if (!sr_test_set_env("PSP_ISO", "override.iso")) return 2;
    check(
        "resolve environment override",
        sr_startup_resolve_iso_path(
            resolved, sizeof(resolved), error, sizeof(error)),
        1);
    check_string("environment override priority", resolved, "override.iso");
    if (!sr_test_set_env("PSP_ISO", NULL)) return 2;

    file = fopen(config_path, "wb");
    if (!file) return 2;
    fwrite("invalid\n", 1, 8, file);
    fclose(file);
    check(
        "reject corrupt launcher config",
        sr_startup_resolve_iso_path(
            resolved, sizeof(resolved), error, sizeof(error)),
        0);

    remove(config_path);
    remove(path);
    sr_test_rmdir(config_dir);
    sr_test_set_env("SR_SETTINGS_DIR", NULL);

    if (failures) {
        fprintf(stderr, "startup check tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("startup check tests passed\n");
    return 0;
}

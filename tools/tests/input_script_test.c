#include "input_script.h"
#include "test_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check_true(const char *name, int value) {
    if (!value) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static void check_u32(const char *name, uint32_t got, uint32_t expected) {
    if (got != expected) {
        fprintf(
            stderr, "FAIL: %s: got %u, expected %u\n",
            name, got, expected);
        failures++;
    }
}

static void check_error(
    const char *name, const char *error, const char *expected) {
    if (!strstr(error, expected)) {
        fprintf(
            stderr, "FAIL: %s: '%s' does not contain '%s'\n",
            name, error, expected);
        failures++;
    }
}

int main(void) {
    static const char valid[] =
        "# psp-recomp analog script v1\n"
        "100 20 255 0\n"
        "120 10 128 255\n";
    SrAnalogScript script;
    char error[256];
    char path[SR_TEST_PATH_MAX];
    uint8_t lx = 7;
    uint8_t ly = 9;

    check_true(
        "parse valid script",
        sr_analog_script_parse(
            &script, valid, error, sizeof(error)));
    check_u32("event count", script.count, 2);
    check_true(
        "before first event is inactive",
        !sr_analog_script_sample(&script, 99, &lx, &ly));
    check_u32("inactive x unchanged", lx, 7);
    check_u32("inactive y unchanged", ly, 9);
    check_true(
        "first event begins inclusively",
        sr_analog_script_sample(&script, 100, &lx, &ly));
    check_u32("first event x", lx, 255);
    check_u32("first event y", ly, 0);
    check_true(
        "adjacent event begins at prior end",
        sr_analog_script_sample(&script, 120, &lx, &ly));
    check_u32("second event x", lx, 128);
    check_u32("second event y", ly, 255);
    check_true(
        "last event ends exclusively",
        !sr_analog_script_sample(&script, 130, &lx, &ly));

    check_true(
        "reject overlap",
        !sr_analog_script_parse(
            &script,
            "# psp-recomp analog script v1\n"
            "10 10 128 0\n"
            "19 5 255 128\n",
            error, sizeof(error)));
    check_error("overlap error", error, "non-overlapping");

    check_true(
        "reject zero duration",
        !sr_analog_script_parse(
            &script,
            "# psp-recomp analog script v1\n"
            "10 0 128 0\n",
            error, sizeof(error)));
    check_error("duration error", error, "duration must be positive");

    check_true(
        "reject analog range",
        !sr_analog_script_parse(
            &script,
            "# psp-recomp analog script v1\n"
            "10 1 256 0\n",
            error, sizeof(error)));
    check_error("analog range error", error, "0..255");

    check_true(
        "require header",
        !sr_analog_script_parse(
            &script, "10 1 128 0\n", error, sizeof(error)));
    check_error("header error", error, "missing");

    check_true(
        "make temporary path",
        sr_test_temp_path(
            path, sizeof(path), "psp-recomp-input-script", ".txt"));
    FILE *file = fopen(path, "wb");
    check_true("open temporary script", file != NULL);
    if (file) {
        check_true(
            "write temporary script",
            fwrite(valid, 1, sizeof(valid) - 1u, file) ==
                sizeof(valid) - 1u);
        check_true("close temporary script", fclose(file) == 0);
        check_true(
            "load script from file",
            sr_analog_script_load(
                &script, path, error, sizeof(error)));
        check_u32("loaded event count", script.count, 2);
        remove(path);
    }

    if (failures) {
        fprintf(stderr, "%d input script test(s) failed\n", failures);
        return 1;
    }
    puts("input script tests passed");
    return 0;
}

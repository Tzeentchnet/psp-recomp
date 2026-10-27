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

int main(void) {
    char path[SR_TEST_PATH_MAX];
    if (!sr_test_temp_path(path, sizeof(path), "kzl-startup", ".iso"))
        return 2;

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

    if (failures) {
        fprintf(stderr, "startup check tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("startup check tests passed\n");
    return 0;
}

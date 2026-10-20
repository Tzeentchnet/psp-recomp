#include "startup_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int failures;

static void check(const char *name, int value, int expected) {
    if (value != expected) {
        fprintf(stderr, "FAIL: %s: got %d, expected %d\n", name, value, expected);
        failures++;
    }
}

int main(void) {
    char temp[MAX_PATH];
    char path[MAX_PATH];
    DWORD length = GetTempPathA(sizeof(temp), temp);
    if (!length || length >= sizeof(temp)) return 2;
    snprintf(
        path, sizeof(path), "%skzl-startup-%lu.iso",
        temp, (unsigned long)GetCurrentProcessId());

    FILE *file = fopen(path, "wb");
    if (!file) return 2;
    fwrite("abc", 1, 3, file);
    fclose(file);
    _putenv_s("PSP_ISO", path);
    check("valid hash", sr_startup_validate(), 1);

    file = fopen(path, "wb");
    if (!file) return 2;
    fwrite("abcd", 1, 4, file);
    fclose(file);
    check("wrong size", sr_startup_validate(), 0);

    remove(path);
    check("missing file", sr_startup_validate(), 0);
    _putenv_s("PSP_ISO", "");
    check("missing setting", sr_startup_validate(), 0);

    if (failures) {
        fprintf(stderr, "startup check tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("startup check tests passed\n");
    return 0;
}

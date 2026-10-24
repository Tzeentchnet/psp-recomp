#include "texture_scaler.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check_true(const char *name, bool value) {
    if (!value) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

int main(void) {
    const uint32_t source[4] = {
        0xff0000ffu, 0xff00ff00u,
        0xffff0000u, 0x80ffffffu,
    };
    uint32_t copy[4] = {};
    uint32_t scaled[16] = {};
    uint32_t single[25] = {};
    const uint32_t translucent = 0x4080c0ffu;

    check_true(
        "1x copy",
        sr_texture_scale_rgba(source, 2, 2, 1, copy) &&
        memcmp(source, copy, sizeof(source)) == 0);
    check_true(
        "2x scale",
        sr_texture_scale_rgba(source, 2, 2, 2, scaled));
    check_true(
        "2x output is populated",
        scaled[0] != 0 && scaled[3] != 0 &&
        scaled[12] != 0 && scaled[15] != 0);
    for (uint32_t factor = 2; factor <= 5; factor++) {
        memset(single, 0, sizeof(single));
        bool valid =
            sr_texture_scale_rgba(
                &translucent, 1, 1, factor, single) != 0;
        for (uint32_t i = 0; valid && i < factor * factor; i++)
            valid = single[i] == translucent;
        check_true("single-pixel alpha", valid);
    }
    check_true(
        "reject invalid factor",
        !sr_texture_scale_rgba(source, 2, 2, 6, scaled));
    check_true(
        "reject null source",
        !sr_texture_scale_rgba(nullptr, 2, 2, 2, scaled));

    if (failures) {
        fprintf(stderr, "texture scaler tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("texture scaler tests passed\n");
    return 0;
}

#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void first(CpuState *state) {
    state->r[2] = 1;
}

static void middle(CpuState *state) {
    state->r[2] = 2;
}

static void last(CpuState *state) {
    state->r[2] = 3;
}

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "dispatch_table_test:%d: %s\n", __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

int main(void) {
    static const SrDispatchEntry entries[] = {
        {0x08804000u, first},
        {0x08812000u, middle},
        {0x089ffffcu, last},
    };
    CpuState state;
    memset(&state, 0, sizeof(state));

    CHECK(sr_lookup(0x08804000u) == NULL);
    sr_set_dispatch_table(entries, 3);
    CHECK(sr_lookup(0x08803ffcu) == NULL);
    CHECK(sr_lookup(0x08804000u) == first);
    CHECK(sr_lookup(0x08812000u) == middle);
    CHECK(sr_lookup(0x089ffffcu) == last);
    CHECK(sr_lookup(0x08a00000u) == NULL);
    sr_lookup(0x08812000u)(&state);
    CHECK(state.r[2] == 2);

    sr_set_dispatch_table(NULL, 0);
    CHECK(sr_lookup(0x08804000u) == NULL);

    const uint32_t large_count = 42600;
    SrDispatchEntry *large = malloc(sizeof(*large) * large_count);
    CHECK(large != NULL);
    for (uint32_t i = 0; i < large_count; i++) {
        large[i].address = 0x08800000u + i * 8;
        large[i].function = first;
    }
    sr_set_dispatch_table(large, large_count);
    for (uint32_t i = 0; i < large_count; i++) {
        CHECK(sr_lookup(large[i].address) == first);
        CHECK(sr_lookup(large[i].address + 4) == NULL);
    }
    sr_set_dispatch_table(NULL, 0);
    free(large);

    puts("dispatch table tests passed");
    return 0;
}

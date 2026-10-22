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

static void interpreted(CpuState *state) {
    state->r[2] = 4;
}

static void replacement(CpuState *state) {
    state->r[2] = 5;
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

    static const SrDispatchEntry module_entries[] = {
        {0x09af5800u, middle},
        {0x09af6000u, last},
    };
    static const SrDispatchTable module_table = {
        "module",
        module_entries,
        2,
    };
    CHECK(sr_activate_dispatch_table(&module_table));
    CHECK(sr_activate_dispatch_table(&module_table));
    CHECK(sr_lookup(0x08804000u) == first);
    CHECK(sr_lookup(0x09af5800u) == middle);

    static const SrDispatchEntry collision_entries[] = {
        {0x08812000u, last},
    };
    static const SrDispatchTable collision_table = {
        "collision",
        collision_entries,
        1,
    };
    CHECK(!sr_activate_dispatch_table(&collision_table));
    CHECK(sr_lookup(0x08812000u) == middle);

    CHECK(sr_deactivate_dispatch_table(&module_table));
    CHECK(!sr_deactivate_dispatch_table(&module_table));
    CHECK(sr_lookup(0x09af5800u) == NULL);
    CHECK(sr_lookup(0x08804000u) == first);

    static const SrDispatchEntry overlay_a_entries[] = {
        {0x09b43c00u, first},
    };
    static const SrDispatchEntry overlay_b_entries[] = {
        {0x09b43c00u, last},
    };
    static const SrDispatchTable overlay_a = {
        "overlay-a",
        overlay_a_entries,
        1,
    };
    static const SrDispatchTable overlay_b = {
        "overlay-b",
        overlay_b_entries,
        1,
    };
    CHECK(sr_activate_dispatch_table(&overlay_a));
    CHECK(!sr_activate_dispatch_table(&overlay_b));
    CHECK(sr_lookup(0x09b43c00u) == first);
    CHECK(sr_deactivate_dispatch_table(&overlay_a));
    CHECK(sr_activate_dispatch_table(&overlay_b));
    CHECK(sr_lookup(0x09b43c00u) == last);
    CHECK(sr_deactivate_dispatch_table(&overlay_b));

    sr_set_dispatch_table(NULL, 0);
    CHECK(sr_lookup(0x08804000u) == NULL);

    static const SrDispatchEntry interpreted_entries[] = {
        {0x09b43c00u, interpreted},
    };
    static const SrDispatchEntry replacement_entries[] = {
        {0x09b43c00u, replacement},
    };
    static const SrDispatchTable interpreted_table = {
        "interpreted",
        interpreted_entries,
        1,
    };
    static const SrDispatchTable replacement_table = {
        "replacement",
        replacement_entries,
        1,
    };
    CHECK(sr_activate_dispatch_table(&interpreted_table));
    CHECK(sr_lookup(0x09b43c00u) == interpreted);
    CHECK(sr_interpreter_module(0x09b43c00u) == NULL);
    sr_disable_native_function(interpreted);
    sr_disable_native_function(interpreted);
    CHECK(sr_lookup(0x09b43c00u) == NULL);
    CHECK(
        strcmp(
            sr_interpreter_module(0x09b43c00u),
            "interpreted") == 0);
    CHECK(sr_deactivate_dispatch_table(&interpreted_table));
    CHECK(sr_activate_dispatch_table(&replacement_table));
    CHECK(sr_lookup(0x09b43c00u) == replacement);
    CHECK(sr_interpreter_module(0x09b43c00u) == NULL);
    CHECK(sr_deactivate_dispatch_table(&replacement_table));

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

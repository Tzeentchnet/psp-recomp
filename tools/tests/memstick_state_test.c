#include "memstick_state.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf( \
                stderr, "memstick_state_test:%d: %s\n", \
                __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

static void set_state(const char *value) {
#if defined(_WIN32)
    _putenv_s("SR_MEMSTICK_STATE", value ? value : "");
#else
    if (value)
        setenv("SR_MEMSTICK_STATE", value, 1);
    else
        unsetenv("SR_MEMSTICK_STATE");
#endif
}

int main(void) {
    int inserted = -1;

    CHECK(sr_memstick_parse_state(NULL, &inserted));
    CHECK(inserted == 1);
    CHECK(sr_memstick_parse_state("inserted", &inserted));
    CHECK(inserted == 1);
    CHECK(sr_memstick_parse_state("removed", &inserted));
    CHECK(inserted == 0);
    CHECK(!sr_memstick_parse_state("missing", &inserted));
    CHECK(!sr_memstick_parse_state("inserted", NULL));

    set_state(NULL);
    CHECK(sr_memstick_inserted());
    CHECK(sr_memstick_driver_state() == SR_MEMSTICK_DEVICE_INSERTED);
    CHECK(sr_memstick_device_state() == SR_MEMSTICK_STATE_INSERTED);
    CHECK(sr_memstick_fat_state() == SR_MEMSTICK_FAT_ASSIGNED);

    set_state("removed");
    CHECK(!sr_memstick_inserted());
    CHECK(sr_memstick_driver_state() == SR_MEMSTICK_DRIVER_READY);
    CHECK(sr_memstick_device_state() == SR_MEMSTICK_STATE_REMOVED);
    CHECK(sr_memstick_fat_state() == SR_MEMSTICK_FAT_UNASSIGNED);

    set_state("inserted");
    CHECK(sr_memstick_inserted());

    puts("memstick state tests passed");
    return 0;
}

#include "memstick_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sr_memstick_parse_state(const char *value, int *inserted) {
    if (!inserted) return 0;
    if (!value || !value[0] || strcmp(value, "inserted") == 0) {
        *inserted = 1;
        return 1;
    }
    if (strcmp(value, "removed") == 0) {
        *inserted = 0;
        return 1;
    }
    return 0;
}

int sr_memstick_inserted(void) {
    const char *value = getenv("SR_MEMSTICK_STATE");
    int inserted;
    if (!sr_memstick_parse_state(value, &inserted)) {
        fprintf(
            stderr,
            "SR_MEMSTICK_STATE: expected inserted or removed, got %s\n",
            value ? value : "");
        _Exit(2);
    }
    return inserted;
}

uint32_t sr_memstick_driver_state(void) {
    return sr_memstick_inserted()
        ? SR_MEMSTICK_DEVICE_INSERTED : SR_MEMSTICK_DRIVER_READY;
}

uint32_t sr_memstick_device_state(void) {
    return sr_memstick_inserted()
        ? SR_MEMSTICK_STATE_INSERTED : SR_MEMSTICK_STATE_REMOVED;
}

uint32_t sr_memstick_fat_state(void) {
    return sr_memstick_inserted()
        ? SR_MEMSTICK_FAT_ASSIGNED : SR_MEMSTICK_FAT_UNASSIGNED;
}

#ifndef SR_MEMSTICK_STATE_H
#define SR_MEMSTICK_STATE_H

#include <stdint.h>

enum {
    SR_MEMSTICK_STATE_INSERTED = 1,
    SR_MEMSTICK_STATE_REMOVED = 2,
    SR_MEMSTICK_DRIVER_READY = 1,
    SR_MEMSTICK_DEVICE_INSERTED = 4,
    SR_MEMSTICK_FAT_UNASSIGNED = 0,
    SR_MEMSTICK_FAT_ASSIGNED = 1,
};

int sr_memstick_parse_state(const char *value, int *inserted);
int sr_memstick_inserted(void);
uint32_t sr_memstick_driver_state(void);
uint32_t sr_memstick_device_state(void);
uint32_t sr_memstick_fat_state(void);

#endif

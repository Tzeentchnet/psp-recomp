#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>

static const SrDispatchEntry *s_dispatch_entries;
static uint32_t s_dispatch_count;

void sr_set_dispatch_table(const SrDispatchEntry *entries, uint32_t count) {
    if (count != 0 && entries == NULL) {
        fprintf(stderr, "sr_set_dispatch_table: null table with %u entries\n", count);
        abort();
    }
    for (uint32_t i = 0; i < count; i++) {
        if (entries[i].function == NULL) {
            fprintf(
                stderr,
                "sr_set_dispatch_table: null function at entry %u (0x%08x)\n",
                i, entries[i].address);
            abort();
        }
        if ((entries[i].address & 3u) != 0) {
            fprintf(
                stderr,
                "sr_set_dispatch_table: unaligned address at entry %u (0x%08x)\n",
                i, entries[i].address);
            abort();
        }
        if (i != 0 && entries[i - 1].address >= entries[i].address) {
            fprintf(
                stderr,
                "sr_set_dispatch_table: entries %u and %u are not ordered\n",
                i - 1, i);
            abort();
        }
    }
    s_dispatch_entries = entries;
    s_dispatch_count = count;
}

RecompFn sr_lookup(uint32_t address) {
    uint32_t low = 0;
    uint32_t high = s_dispatch_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        uint32_t candidate = s_dispatch_entries[middle].address;
        if (candidate < address)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < s_dispatch_count && s_dispatch_entries[low].address == address)
        return s_dispatch_entries[low].function;
    return NULL;
}

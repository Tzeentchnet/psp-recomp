/* ISO9660 disc reader (src/rt/iso.c). */
#ifndef PSP_RECOMP_ISO_H
#define PSP_RECOMP_ISO_H

#include <stdint.h>

typedef struct IsoDirEntry {
    char name[256];
    uint32_t lba;
    uint32_t size;
    int is_dir;
} IsoDirEntry;

int iso_init(void);
/* Resolve a guest path ("disc0:/PSP_GAME/...") to its extent. Returns 0 on success. */
int iso_lookup(const char *guest_path, uint32_t *out_lba, uint32_t *out_size);
int iso_lookup_info(const char *guest_path, uint32_t *out_lba, uint32_t *out_size, int *out_is_dir);
/* List a directory. out_count receives the total even when capacity is smaller. */
int iso_list(const char *guest_path, IsoDirEntry *entries, int capacity, int *out_count);
/* Read bytes from (lba*2048 + offset). Returns bytes read. */
int iso_read(uint32_t lba, uint32_t offset, void *dst, uint32_t bytes);

#endif

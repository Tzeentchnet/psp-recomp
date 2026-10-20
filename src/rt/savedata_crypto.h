#ifndef PSP_RECOMP_SAVEDATA_CRYPTO_H
#define PSP_RECOMP_SAVEDATA_CRYPTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int sr_savedata_encrypt(
    int mode, uint8_t *data, uint32_t *size, uint32_t capacity,
    uint8_t hash[16], const uint8_t key[16]);
int sr_savedata_decrypt(
    int mode, uint8_t *data, uint32_t *size, uint32_t capacity,
    const uint8_t key[16], const uint8_t expected_hash[16]);
int sr_savedata_update_sfo_hash(
    uint8_t *sfo, uint32_t size, uint32_t capacity,
    uint32_t params_offset, int mode);

#ifdef __cplusplus
}
#endif

#endif

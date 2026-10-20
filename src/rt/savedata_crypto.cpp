#include "savedata_crypto.h"

#include "../../third_party/ppsspp_savedata/chnnlsv.h"

#include <cstring>

namespace {

uint32_t align16(uint32_t value) {
    return (value + 15u) & ~15u;
}

void ensure_kirk_initialized() {
    static bool initialized;
    if (!initialized) {
        kirk_init(chnnlsv_kirk_state());
        initialized = true;
    }
}

int build_hash(
    uint8_t output[16], const uint8_t *data, uint32_t aligned_length,
    int mode, const uint8_t *key) {
    PspChnnlsvContext1 context{};
    std::memset(output, 0, 16);
    if (sceSdMacInit(context, mode & 0xff) < 0)
        return -1;
    if (sceSdMacUpdate(context, data, (int)aligned_length) < 0)
        return -2;
    if (sceSdMacFinal(context, output, key) < 0) {
        std::memset(output, 1, 16);
        return 0;
    }
    return 0;
}

}  // namespace

extern "C" int sr_savedata_encrypt(
    int mode, uint8_t *data, uint32_t *size, uint32_t capacity,
    uint8_t hash[16], const uint8_t key[16]) {
    if (!data || !size || !hash)
        return -1;
    uint32_t plain_size = *size;
    uint32_t aligned = align16(plain_size);
    if (aligned > UINT32_MAX - 16u || capacity < aligned + 16u)
        return -2;
    ensure_kirk_initialized();
    std::memmove(data + 16, data, aligned);
    std::memset(hash, 0, 16);
    std::memset(data, 0, 16);

    PspChnnlsvContext2 cipher{};
    PspChnnlsvContext1 mac{};
    const uint8_t *mode_key = mode > 1 ? key : nullptr;
    if (sceSdCipherInit(cipher, mode, 1, data, mode_key) < 0)
        return -3;
    if (sceSdMacInit(mac, mode) < 0)
        return -4;
    if (sceSdMacUpdate(mac, data, 16) < 0)
        return -5;
    if (sceSdCipherUpdate(cipher, data + 16, (int)aligned) < 0)
        return -6;
    std::memset(data + 16 + plain_size, 0, aligned - plain_size);
    if (sceSdMacUpdate(mac, data + 16, (int)aligned) < 0)
        return -7;
    if (sceSdCipherFinal(cipher) < 0)
        return -8;
    if (sceSdMacFinal(mac, hash, mode_key) < 0)
        return -9;
    *size = plain_size + 16;
    return 0;
}

extern "C" int sr_savedata_decrypt(
    int mode, uint8_t *data, uint32_t *size, uint32_t capacity,
    const uint8_t key[16], const uint8_t expected_hash[16]) {
    if (!data || !size || *size <= 16)
        return -1;
    ensure_kirk_initialized();
    uint32_t encrypted_size = *size;
    uint32_t aligned_size = align16(encrypted_size);
    if (capacity < aligned_size)
        return -1;
    uint32_t plain_size = encrypted_size - 16;
    uint32_t aligned_plain_size = aligned_size - 16;
    std::memset(data + encrypted_size, 0, aligned_size - encrypted_size);
    const uint8_t *mode_key = mode > 1 ? key : nullptr;
    PspChnnlsvContext1 mac{};
    PspChnnlsvContext2 cipher{};
    if (sceSdMacInit(mac, mode) < 0)
        return -2;
    if (sceSdCipherInit(cipher, mode, 2, data, mode_key) < 0)
        return -3;
    if (sceSdMacUpdate(mac, data, 16) < 0)
        return -4;
    if (sceSdMacUpdate(mac, data + 16, (int)aligned_plain_size) < 0)
        return -5;
    if (sceSdCipherUpdate(cipher, data + 16, (int)aligned_plain_size) < 0)
        return -6;
    if (sceSdCipherFinal(cipher) < 0)
        return -7;
    if (expected_hash) {
        uint8_t actual_hash[16];
        if (sceSdMacFinal(mac, actual_hash, mode_key) < 0)
            return -8;
        if (std::memcmp(actual_hash, expected_hash, 16) != 0)
            return -9;
    }
    std::memmove(data, data + 16, plain_size);
    *size = plain_size;
    return 0;
}

extern "C" int sr_savedata_update_sfo_hash(
    uint8_t *sfo, uint32_t size, uint32_t capacity,
    uint32_t params_offset, int mode) {
    if (!sfo || params_offset > size || size - params_offset < 128)
        return -1;
    uint32_t aligned = align16(size);
    if (capacity < aligned)
        return -2;
    ensure_kirk_initialized();
    std::memset(sfo + size, 0, aligned - size);
    std::memset(sfo + params_offset, 0, 128);

    int first_mode = mode & 2 ? 4 : 2;
    int second_mode = mode & 2 ? 3 : 0;
    if (mode & 4) {
        first_mode = 6;
        second_mode = 5;
    }
    uint8_t hash[16];
    int result = build_hash(hash, sfo, aligned, first_mode, nullptr);
    if (result < 0)
        return result - 100;
    std::memcpy(sfo + params_offset + 0x20, hash, 16);
    sfo[params_offset] |= 0x01;

    if (mode & 6) {
        sfo[params_offset] |= (uint8_t)((mode & 6) << 4);
        result = build_hash(hash, sfo, aligned, second_mode, nullptr);
        if (result < 0)
            return result - 200;
        std::memcpy(sfo + params_offset + 0x70, hash, 16);
    }
    result = build_hash(hash, sfo, aligned, 1, nullptr);
    if (result < 0)
        return result - 300;
    std::memcpy(sfo + params_offset + 0x10, hash, 16);
    return 0;
}

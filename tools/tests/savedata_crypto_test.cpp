#include "savedata_crypto.h"

#include <cstdio>
#include <cstring>
#include <vector>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf( \
                stderr, "savedata_crypto_test:%d: %s\n", __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

int main() {
    static const uint8_t key[16] = {
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
        0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
    };
    std::vector<uint8_t> plain(1664);
    for (size_t i = 0; i < plain.size(); i++)
        plain[i] = (uint8_t)(i * 37u + 11u);
    std::vector<uint8_t> encrypted(plain.size() + 16);
    std::memcpy(encrypted.data(), plain.data(), plain.size());
    uint32_t encrypted_size = (uint32_t)plain.size();
    uint8_t hash[16];
    CHECK(sr_savedata_encrypt(
        5, encrypted.data(), &encrypted_size, (uint32_t)encrypted.size(),
        hash, key) == 0);
    CHECK(encrypted_size == 1680);
    CHECK(std::memcmp(encrypted.data(), plain.data(), plain.size()) != 0);

    std::vector<uint8_t> decrypted = encrypted;
    uint32_t decrypted_size = encrypted_size;
    CHECK(sr_savedata_decrypt(
        5, decrypted.data(), &decrypted_size, (uint32_t)decrypted.size(),
        key, hash) == 0);
    CHECK(decrypted_size == plain.size());
    CHECK(std::memcmp(decrypted.data(), plain.data(), plain.size()) == 0);

    uint8_t bad_hash[16];
    std::memcpy(bad_hash, hash, sizeof(bad_hash));
    bad_hash[0] ^= 1;
    decrypted = encrypted;
    decrypted_size = encrypted_size;
    CHECK(sr_savedata_decrypt(
        5, decrypted.data(), &decrypted_size, (uint32_t)decrypted.size(),
        key, bad_hash) != 0);

    std::vector<uint8_t> sfo(256);
    for (size_t i = 0; i < 96; i++)
        sfo[i] = (uint8_t)(i * 3u);
    std::vector<uint8_t> second = sfo;
    CHECK(sr_savedata_update_sfo_hash(
        sfo.data(), 192, (uint32_t)sfo.size(), 32, 5) == 0);
    CHECK(sr_savedata_update_sfo_hash(
        second.data(), 192, (uint32_t)second.size(), 32, 5) == 0);
    CHECK(sfo[32] == 0x41);
    CHECK(std::memcmp(sfo.data(), second.data(), 192) == 0);
    bool nonzero = false;
    for (size_t i = 0; i < 16; i++)
        nonzero |= sfo[32 + 0x10 + i] != 0;
    CHECK(nonzero);

    std::puts("savedata crypto tests passed");
    return 0;
}

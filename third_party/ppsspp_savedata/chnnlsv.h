// Standalone subset of PPSSPP Core/HLE/sceChnnlsv.h.
// PPSSPP is GPL-2.0-or-later; libkirk is GPL-3.0-or-later.

#pragma once

#include <cstdint>

#include "kirk_engine.h"

using s8 = int8_t;

struct PspChnnlsvContext1 {
    int32_t mode;
    uint8_t result[0x10];
    uint8_t key[0x10];
    int32_t keyLength;
};

struct PspChnnlsvContext2 {
    int32_t mode;
    int32_t unkn;
    uint8_t cryptedData[0x92];
};

using pspChnnlsvContext1 = PspChnnlsvContext1;
using pspChnnlsvContext2 = PspChnnlsvContext2;

int sceSdMacInit(PspChnnlsvContext1 &context, int mode);
int sceSdMacUpdate(
    PspChnnlsvContext1 &context, const uint8_t *data, int length);
int sceSdMacFinal(
    PspChnnlsvContext1 &context, uint8_t *hash, const uint8_t *key);
int sceSdCipherInit(
    PspChnnlsvContext2 &context, int mode, int operation,
    uint8_t *data, const uint8_t *key);
int sceSdCipherUpdate(
    PspChnnlsvContext2 &context, uint8_t *data, int aligned_length);
int sceSdCipherFinal(PspChnnlsvContext2 &context);
KirkState *chnnlsv_kirk_state();

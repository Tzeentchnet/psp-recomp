#include "kzl_twinstick.h"
#include "recomp.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_RAM_SIZE 0x01000000u
#define PROFILE_OWNER_GLOBAL 0x08ea04a0u
#define FUNCTION_BUTTONS_OFFSET 0x000003d4u
#define DPAD_MOVEMENT_OFFSET 0x000004b0u

static uint8_t s_ram[TEST_RAM_SIZE];
uint8_t *g_mem = s_ram;
int g_sr_gpu_vram_read_watch;
int g_sr_gpu_vram_write_watch;

static int failures;
static int out_of_range_access;
static int aim_active;
static float aim_x;
static float aim_y;

static const uint8_t s_indices[] = {
    3, 7, 8, 9, 13, 41, 42, 43, 44, 45, 46, 47, 48,
};

static const uint8_t s_injected[] = {
    19, 18, 17, 16, 18, 2, 3, 1, 0, 18, 19, 17, 16,
};

void sr_gpu_vram_read(uint32_t address, uint32_t bytes) {
    (void)address;
    (void)bytes;
}

void sr_gpu_vram_dirty(uint32_t address, uint32_t bytes) {
    (void)address;
    (void)bytes;
}

void sr_gpu_vram_prepare_write(uint32_t address, uint32_t bytes) {
    (void)address;
    (void)bytes;
}

void sr_oor(uint32_t address, uint32_t value, int store) {
    (void)address;
    (void)value;
    (void)store;
    out_of_range_access++;
}

int sr_input_twin_stick_aim(float *x, float *y) {
    if (!aim_active) return 0;
    if (x) *x = aim_x;
    if (y) *y = aim_y;
    return 1;
}

static void check_u32(const char *name, uint32_t got, uint32_t expected) {
    if (got != expected) {
        fprintf(
            stderr,
            "FAIL: %s: got 0x%08x, expected 0x%08x\n",
            name,
            got,
            expected);
        failures++;
    }
}

static void check_float(const char *name, float got, float expected) {
    if (fabsf(got - expected) > 0.0001f) {
        fprintf(
            stderr,
            "FAIL: %s: got %.6f, expected %.6f\n",
            name,
            got,
            expected);
        failures++;
    }
}

static void write_float(uint32_t address, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    sr_w32(address, bits);
}

static float read_float(uint32_t address) {
    uint32_t bits = sr_r32(address);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t mapping_address(uint32_t profile, int slot) {
    return profile + FUNCTION_BUTTONS_OFFSET +
           (uint32_t)s_indices[slot] * sizeof(uint32_t);
}

static void setup_profile(
    uint32_t owner,
    uint32_t profile,
    uint32_t originals[sizeof(s_indices)]) {
    memset(s_ram, 0, sizeof(s_ram));
    sr_w32(PROFILE_OWNER_GLOBAL, owner);
    sr_w32(owner + 0x0cu, profile);
    sr_w8(owner + 0x14u, 0x5au);
    for (int i = 0; i < (int)sizeof(s_indices); i++) {
        originals[i] = (uint32_t)(i % 10);
        sr_w32(mapping_address(profile, i), originals[i]);
    }
    sr_w8(profile + DPAD_MOVEMENT_OFFSET, 0);
}

int main(void) {
    const uint32_t owner = 0x08810000u;
    const uint32_t profile = 0x08820000u;
    const uint32_t controller = 0x08830000u;
    uint32_t originals[sizeof(s_indices)];
    CpuState state;

    setup_profile(owner, profile, originals);
    sr_kzl_twinstick_update(1);
    for (int i = 0; i < (int)sizeof(s_indices); i++)
        check_u32("injected mapping", sr_r32(mapping_address(profile, i)), s_injected[i]);
    check_u32(
        "enabled D-pad movement",
        sr_r8(profile + DPAD_MOVEMENT_OFFSET),
        1);
    check_u32("profile not marked dirty", sr_r8(owner + 0x14u), 0x5a);

    memset(&state, 0, sizeof(state));
    state.r[17] = controller;
    write_float(controller + 0xc0u, 1.0f);
    write_float(controller + 0xc4u, 0.0f);
    write_float(controller + 0xd0u, 0.0f);
    write_float(controller + 0xd4u, 1.0f);
    write_float(controller + 0xe0u, 0.0f);
    write_float(controller + 0xe4u, 0.0f);
    aim_active = 1;
    aim_x = 1.0f;
    aim_y = 0.0f;
    sr_kzl_twinstick_apply_aim(&state);
    check_float(
        "right-stick facing x",
        read_float(controller + 0x114u),
        1.0f);
    check_float(
        "right-stick facing y",
        read_float(controller + 0x118u),
        0.0f);
    aim_x = 0.0f;
    aim_y = -1.0f;
    sr_kzl_twinstick_apply_aim(&state);
    check_float(
        "up-stick facing x",
        read_float(controller + 0x114u),
        0.0f);
    check_float(
        "up-stick facing y",
        read_float(controller + 0x118u),
        -1.0f);
    write_float(controller + 0x114u, 0.25f);
    aim_active = 0;
    sr_kzl_twinstick_apply_aim(&state);
    check_float(
        "centered aim preserves original facing",
        read_float(controller + 0x114u),
        0.25f);

    sr_w32(mapping_address(profile, 0), 7);
    sr_kzl_twinstick_update(1);
    check_u32(
        "reapplied changed mapping",
        sr_r32(mapping_address(profile, 0)),
        s_injected[0]);
    sr_kzl_twinstick_update(0);
    originals[0] = 7;
    for (int i = 0; i < (int)sizeof(s_indices); i++)
        check_u32("restored mapping", sr_r32(mapping_address(profile, i)), originals[i]);
    check_u32(
        "restored D-pad movement",
        sr_r8(profile + DPAD_MOVEMENT_OFFSET),
        0);

    sr_w32(PROFILE_OWNER_GLOBAL, 0);
    sr_kzl_twinstick_update(1);
    sr_w32(PROFILE_OWNER_GLOBAL, owner);
    sr_kzl_twinstick_update(1);
    check_u32(
        "late profile activation",
        sr_r8(profile + DPAD_MOVEMENT_OFFSET),
        1);
    sr_kzl_twinstick_reset();
    check_u32(
        "reset restores profile",
        sr_r8(profile + DPAD_MOVEMENT_OFFSET),
        0);
    check_u32("no out-of-range access", (uint32_t)out_of_range_access, 0);

    if (failures) {
        fprintf(stderr, "KZL twin-stick tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("KZL twin-stick tests passed\n");
    return 0;
}

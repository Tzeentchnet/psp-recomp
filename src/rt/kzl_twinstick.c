#include "kzl_twinstick.h"
#include "input_config.h"
#include "recomp.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define KZL_PROFILE_OWNER_GLOBAL 0x08ea04a0u
#define KZL_PROFILE_DATA_OFFSET 0x0000000cu
#define KZL_PROFILE_DIRTY_OFFSET 0x00000014u
#define KZL_FUNCTION_BUTTONS_OFFSET 0x000003d4u
#define KZL_DPAD_MOVEMENT_OFFSET 0x000004b0u
#define KZL_SOLDIER_AIM_BASIS_OFFSET 0x000000c0u
#define KZL_SOLDIER_FACING_OFFSET 0x00000114u

static const uint8_t s_function_indices[] = {
    3, 7, 8, 9, 13, 41, 42, 43, 44, 45, 46, 47, 48,
};

static const uint8_t s_twin_stick_mappings[] = {
    19, 18, 17, 16, 18, 2, 3, 1, 0, 18, 19, 17, 16,
};

typedef struct KzlTwinStickSnapshot {
    uint32_t owner;
    uint32_t profile;
    uint32_t mappings[sizeof(s_function_indices)];
    uint8_t dpad_movement;
    int captured;
} KzlTwinStickSnapshot;

static KzlTwinStickSnapshot s_snapshot;
static uint32_t s_rejected_profile;
static uint32_t s_rejected_pointer;
static int s_aim_hook_logged;

static int valid_span(uint32_t address, uint32_t size) {
    if (!g_mem || !size || address > UINT32_MAX - (size - 1u)) return 0;
    return sr_inrange(address) && sr_inrange(address + size - 1u);
}

static int valid_object_pointer(uint32_t address, uint32_t size) {
    return address && !(address & 3u) && valid_span(address, size);
}

static float read_float(uint32_t address) {
    uint32_t bits = sr_r32(address);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void write_float(uint32_t address, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    sr_w32(address, bits);
}

static int normalize_2d(float *x, float *y) {
    float magnitude_squared = *x * *x + *y * *y;
    if (!isfinite(magnitude_squared) || magnitude_squared <= 1e-12f)
        return 0;
    float inverse_magnitude = 1.0f / sqrtf(magnitude_squared);
    *x *= inverse_magnitude;
    *y *= inverse_magnitude;
    return isfinite(*x) && isfinite(*y);
}

static void report_bad_pointer(uint32_t owner, uint32_t profile) {
    uint32_t pointer = profile ? profile : owner;
    if (pointer == s_rejected_pointer) return;
    fprintf(
        stderr,
        "twin-stick: refusing invalid player profile pointer 0x%08x\n",
        pointer);
    s_rejected_pointer = pointer;
}

static int resolve_profile(uint32_t *owner, uint32_t *profile) {
    uint32_t current_owner;
    uint32_t current_profile;
    if (!valid_span(KZL_PROFILE_OWNER_GLOBAL, sizeof(uint32_t))) return 0;
    current_owner = sr_r32(KZL_PROFILE_OWNER_GLOBAL);
    if (!current_owner) return 0;
    if (!valid_object_pointer(
            current_owner, KZL_PROFILE_DIRTY_OFFSET + sizeof(uint8_t))) {
        report_bad_pointer(current_owner, 0);
        return 0;
    }
    current_profile = sr_r32(current_owner + KZL_PROFILE_DATA_OFFSET);
    if (!valid_object_pointer(
            current_profile,
            KZL_DPAD_MOVEMENT_OFFSET + sizeof(uint8_t))) {
        report_bad_pointer(current_owner, current_profile);
        return 0;
    }
    s_rejected_pointer = 0;
    *owner = current_owner;
    *profile = current_profile;
    return 1;
}

static int valid_mapping(uint32_t value) {
    return value == UINT32_MAX || value <= 19u;
}

static uint32_t mapping_address(uint32_t profile, int slot) {
    return profile + KZL_FUNCTION_BUTTONS_OFFSET +
           (uint32_t)s_function_indices[slot] * sizeof(uint32_t);
}

static int read_profile_state(
    uint32_t profile,
    uint32_t mappings[sizeof(s_function_indices)],
    uint8_t *dpad_movement) {
    for (int i = 0; i < (int)sizeof(s_function_indices); i++) {
        mappings[i] = sr_r32(mapping_address(profile, i));
        if (!valid_mapping(mappings[i])) {
            if (s_rejected_profile != profile) {
                fprintf(
                    stderr,
                    "twin-stick: refusing unexpected mapping %u at profile "
                    "0x%08x\n",
                    mappings[i],
                    profile);
                s_rejected_profile = profile;
            }
            return 0;
        }
    }
    *dpad_movement = sr_r8(profile + KZL_DPAD_MOVEMENT_OFFSET);
    if (*dpad_movement > 1u) {
        if (s_rejected_profile != profile) {
            fprintf(
                stderr,
                "twin-stick: refusing unexpected D-pad mode %u at profile "
                "0x%08x\n",
                *dpad_movement,
                profile);
            s_rejected_profile = profile;
        }
        return 0;
    }
    s_rejected_profile = 0;
    return 1;
}

static void write_twin_stick_profile(
    uint32_t profile,
    const uint32_t mappings[sizeof(s_function_indices)],
    uint8_t dpad_movement) {
    for (int i = 0; i < (int)sizeof(s_function_indices); i++) {
        if (mappings[i] != s_twin_stick_mappings[i]) {
            sr_w32(
                mapping_address(profile, i),
                s_twin_stick_mappings[i]);
        }
    }
    if (dpad_movement != 1u)
        sr_w8(profile + KZL_DPAD_MOVEMENT_OFFSET, 1);
}

static void restore_snapshot(void) {
    if (!s_snapshot.captured) return;
    if (valid_object_pointer(
            s_snapshot.owner,
            KZL_PROFILE_DIRTY_OFFSET + sizeof(uint8_t)) &&
        sr_r32(s_snapshot.owner + KZL_PROFILE_DATA_OFFSET) ==
            s_snapshot.profile) {
        for (int i = 0; i < (int)sizeof(s_function_indices); i++) {
            uint32_t address = mapping_address(s_snapshot.profile, i);
            if (sr_r32(address) == s_twin_stick_mappings[i])
                sr_w32(address, s_snapshot.mappings[i]);
        }
        if (sr_r8(s_snapshot.profile + KZL_DPAD_MOVEMENT_OFFSET) == 1u) {
            sr_w8(
                s_snapshot.profile + KZL_DPAD_MOVEMENT_OFFSET,
                s_snapshot.dpad_movement);
        }
    }
    memset(&s_snapshot, 0, sizeof(s_snapshot));
}

void sr_kzl_twinstick_update(int active) {
    uint32_t owner;
    uint32_t profile;
    uint32_t mappings[sizeof(s_function_indices)];
    uint8_t dpad_movement;

    if (!active) {
        restore_snapshot();
        return;
    }
    if (!resolve_profile(&owner, &profile)) {
        restore_snapshot();
        return;
    }
    if (s_snapshot.captured && s_snapshot.profile != profile)
        restore_snapshot();
    if (!read_profile_state(profile, mappings, &dpad_movement)) return;

    if (!s_snapshot.captured) {
        s_snapshot.owner = owner;
        s_snapshot.profile = profile;
        memcpy(s_snapshot.mappings, mappings, sizeof(mappings));
        s_snapshot.dpad_movement = dpad_movement;
        s_snapshot.captured = 1;
        fprintf(
            stderr,
            "twin-stick: applied temporary controller profile at 0x%08x\n",
            profile);
    } else {
        for (int i = 0; i < (int)sizeof(s_function_indices); i++) {
            if (mappings[i] != s_twin_stick_mappings[i])
                s_snapshot.mappings[i] = mappings[i];
        }
        if (dpad_movement != 1u)
            s_snapshot.dpad_movement = dpad_movement;
    }
    write_twin_stick_profile(
        profile, mappings, dpad_movement);
}

void sr_kzl_twinstick_apply_aim(CpuState *s) {
    float aim_x;
    float aim_y;
    float right_x;
    float right_y;
    float forward_x;
    float forward_y;
    float facing_x;
    float facing_y;
    uint32_t controller;

    if (!s_snapshot.captured || !sr_input_twin_stick_aim(&aim_x, &aim_y))
        return;
    controller = s->r[17];
    if (!valid_object_pointer(
            controller, KZL_SOLDIER_FACING_OFFSET + 3u * sizeof(float))) {
        return;
    }

    right_x = read_float(controller + KZL_SOLDIER_AIM_BASIS_OFFSET);
    right_y = read_float(controller + KZL_SOLDIER_AIM_BASIS_OFFSET + 4u);
    forward_x =
        read_float(controller + KZL_SOLDIER_AIM_BASIS_OFFSET + 0x10u) +
        read_float(controller + KZL_SOLDIER_AIM_BASIS_OFFSET + 0x20u);
    forward_y =
        read_float(controller + KZL_SOLDIER_AIM_BASIS_OFFSET + 0x14u) +
        read_float(controller + KZL_SOLDIER_AIM_BASIS_OFFSET + 0x24u);
    if (!normalize_2d(&right_x, &right_y) ||
        !normalize_2d(&forward_x, &forward_y)) {
        return;
    }

    facing_x = aim_x * right_x + aim_y * forward_x;
    facing_y = aim_x * right_y + aim_y * forward_y;
    if (!normalize_2d(&facing_x, &facing_y)) return;
    write_float(controller + KZL_SOLDIER_FACING_OFFSET, facing_x);
    write_float(controller + KZL_SOLDIER_FACING_OFFSET + 4u, facing_y);
    write_float(controller + KZL_SOLDIER_FACING_OFFSET + 8u, 0.0f);
    if (!s_aim_hook_logged) {
        fprintf(stderr, "twin-stick: native on-foot aim vector active\n");
        s_aim_hook_logged = 1;
    }
}

void sr_kzl_twinstick_reset(void) {
    restore_snapshot();
    s_rejected_profile = 0;
    s_rejected_pointer = 0;
    s_aim_hook_logged = 0;
}

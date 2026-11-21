#include "graphics_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSP_DISPLAY_WIDTH 480u
#define PSP_DISPLAY_HEIGHT 272u
#define PSP_TEXTURE_ENVELOPE 512u

static int apple_generation(const char *device_name) {
    static const char prefix[] = "Apple M";
    char *end = NULL;
    unsigned long generation;
    if (!device_name ||
        strncmp(device_name, prefix, sizeof(prefix) - 1u) != 0) {
        return 0;
    }
    generation = strtoul(
        device_name + sizeof(prefix) - 1u, &end, 10);
    if (!end || end == device_name + sizeof(prefix) - 1u ||
        (*end && *end != ' ') || generation < 1u || generation > 5u) {
        return 0;
    }
    return (int)generation;
}

static int high_tier_variant(const char *device_name) {
    return device_name &&
        (strstr(device_name, " Pro") ||
         strstr(device_name, " Max") ||
         strstr(device_name, " Ultra"));
}

const char *sr_graphics_auto_profile_name(int profile) {
    switch (profile) {
        case SR_GRAPHICS_AUTO_PROFILE_MANUAL: return "manual";
        case SR_GRAPHICS_AUTO_PROFILE_REFERENCE: return "reference";
        case SR_GRAPHICS_AUTO_PROFILE_SAFE: return "safe";
        case SR_GRAPHICS_AUTO_PROFILE_BALANCED: return "balanced";
        case SR_GRAPHICS_AUTO_PROFILE_HIGH: return "high";
        default: return NULL;
    }
}

int sr_graphics_auto_select(
    const SrGraphicsAutoCaps *caps,
    SrGraphicsAutoSelection *selection) {
    uint32_t desired;
    uint32_t display_cap;
    uint32_t image_cap;
    int generation;
    int variant;
    if (!caps || !selection)
        return 0;
    memset(selection, 0, sizeof(*selection));
    selection->texture_scale = 1u;
    selection->anti_aliasing = SR_GRAPHICS_ANTI_ALIASING_OFF;

    if (!caps->hardware_renderer) {
        selection->profile = SR_GRAPHICS_AUTO_PROFILE_REFERENCE;
        selection->internal_resolution = 1u;
        snprintf(
            selection->reason, sizeof(selection->reason),
            "software reference renderer uses native PSP resolution");
        return 1;
    }

    generation = apple_generation(caps->device_name);
    variant = high_tier_variant(caps->device_name);
    desired = 2u;
    if (generation == 1 || generation == 2) {
        if (variant)
            desired = 3u;
    } else if (generation == 3 || generation == 4) {
        desired = variant ? 4u : 3u;
    } else if (generation == 5) {
        desired = 4u;
    }

    if (caps->drawable_width && caps->drawable_height) {
        uint32_t width_cap =
            caps->drawable_width / PSP_DISPLAY_WIDTH;
        uint32_t height_cap =
            caps->drawable_height / PSP_DISPLAY_HEIGHT;
        display_cap = width_cap < height_cap
            ? width_cap : height_cap;
        if (!display_cap)
            display_cap = 1u;
    } else {
        display_cap = 2u;
    }
    image_cap = caps->max_image_dimension_2d
        ? caps->max_image_dimension_2d / PSP_TEXTURE_ENVELOPE
        : 2u;
    if (!image_cap)
        image_cap = 1u;
    if (desired > display_cap)
        desired = display_cap;
    if (desired > image_cap)
        desired = image_cap;
    if (desired > SR_GRAPHICS_SCALE_MAX)
        desired = SR_GRAPHICS_SCALE_MAX;
    if (desired < SR_GRAPHICS_SCALE_MIN)
        desired = SR_GRAPHICS_SCALE_MIN;

    selection->internal_resolution = desired;
    selection->profile =
        desired >= 4u ? SR_GRAPHICS_AUTO_PROFILE_HIGH :
        desired >= 3u ? SR_GRAPHICS_AUTO_PROFILE_BALANCED :
        SR_GRAPHICS_AUTO_PROFILE_SAFE;
    if (generation == 5 && caps->smaa_supported)
        selection->anti_aliasing =
            SR_GRAPHICS_ANTI_ALIASING_SMAA;

    snprintf(
        selection->reason, sizeof(selection->reason),
        "%s device policy for %s, capped by %ux%u drawable and "
        "%u-pixel Vulkan image limit; xBRZ remains manual%s",
        generation ? "known Apple Silicon" : "conservative unknown-device",
        caps->device_name && caps->device_name[0]
            ? caps->device_name : "unnamed GPU",
        caps->drawable_width, caps->drawable_height,
        caps->max_image_dimension_2d,
        selection->anti_aliasing == SR_GRAPHICS_ANTI_ALIASING_SMAA
            ? "; SMAA enabled from measured M5 headroom"
            : "");
    return 1;
}

#ifndef GE_VK_TNL_CLASSES_H
#define GE_VK_TNL_CLASSES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    GE_VK_TNL_CLASS_DIRECT = 1u << 0,
    GE_VK_TNL_CLASS_PREPROCESS = 1u << 1,
    GE_VK_TNL_CLASS_POINTS = 1u << 2,
    GE_VK_TNL_CLASS_SPRITES = 1u << 3,
    GE_VK_TNL_CLASS_ALL =
        GE_VK_TNL_CLASS_DIRECT |
        GE_VK_TNL_CLASS_PREPROCESS |
        GE_VK_TNL_CLASS_POINTS |
        GE_VK_TNL_CLASS_SPRITES,
};

static inline int ge_vk_tnl_class_mask(
    const char *setting, uint32_t *mask_out) {
    if (!mask_out) return 0;
    if (!setting || !setting[0]) {
        *mask_out = GE_VK_TNL_CLASS_ALL;
        return 1;
    }

    uint32_t mask = 0;
    while (setting[0]) {
        const char *end = strchr(setting, ',');
        size_t length = end ? (size_t)(end - setting) : strlen(setting);
        uint32_t class_mask = 0;
        if (length == strlen("direct") &&
            strncmp(setting, "direct", length) == 0) {
            class_mask = GE_VK_TNL_CLASS_DIRECT;
        } else if (length == strlen("preprocess") &&
                   strncmp(setting, "preprocess", length) == 0) {
            class_mask = GE_VK_TNL_CLASS_PREPROCESS;
        } else if (length == strlen("points") &&
                   strncmp(setting, "points", length) == 0) {
            class_mask = GE_VK_TNL_CLASS_POINTS;
        } else if (length == strlen("sprites") &&
                   strncmp(setting, "sprites", length) == 0) {
            class_mask = GE_VK_TNL_CLASS_SPRITES;
        } else {
            return 0;
        }
        mask |= class_mask;
        if (!end) break;
        setting = end + 1;
        if (!setting[0]) return 0;
    }
    *mask_out = mask;
    return 1;
}

#endif

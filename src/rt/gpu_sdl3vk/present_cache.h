#ifndef SR_SDL3VK_PRESENT_CACHE_H
#define SR_SDL3VK_PRESENT_CACHE_H

#include <stdint.h>

typedef struct SrVkPresentCache {
    uint64_t generation;
    unsigned reuses;
    int valid;
} SrVkPresentCache;

#define SR_VK_PRESENT_MAX_REUSES 120u

static inline int sr_vk_present_can_reuse(
    const SrVkPresentCache *cache, uint64_t generation, int redraw) {
    return cache->valid && generation && !redraw &&
           cache->generation == generation &&
           cache->reuses < SR_VK_PRESENT_MAX_REUSES;
}

static inline void sr_vk_present_reused(SrVkPresentCache *cache) {
    cache->reuses++;
}

static inline void sr_vk_present_record(
    SrVkPresentCache *cache, uint64_t generation) {
    cache->generation = generation;
    cache->reuses = 0;
    cache->valid = generation != 0;
}

static inline void sr_vk_present_invalidate(SrVkPresentCache *cache) {
    cache->valid = 0;
}

#endif

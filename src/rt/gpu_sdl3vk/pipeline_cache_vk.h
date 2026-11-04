#ifndef SR_PIPELINE_CACHE_VK_H
#define SR_PIPELINE_CACHE_VK_H

#include "../platform.h"

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct SrVkPipelineCache {
    VkDevice device;
    VkPipelineCache handle;
    VkPhysicalDeviceProperties properties;
    char path[SR_PATH_MAX];
    uint64_t shader_hash;
    uint32_t schema_version;
    int persistent;
} SrVkPipelineCache;

#define SR_VK_PIPELINE_HASH_INIT UINT64_C(14695981039346656037)

uint64_t sr_vk_pipeline_cache_hash(
    uint64_t hash, const void *data, size_t size);
int sr_vk_pipeline_cache_init(
    SrVkPipelineCache *cache,
    VkPhysicalDevice physical_device,
    VkDevice device,
    const char *relative_path,
    uint32_t schema_version,
    uint64_t shader_hash);
void sr_vk_pipeline_cache_shutdown(SrVkPipelineCache *cache);

#endif

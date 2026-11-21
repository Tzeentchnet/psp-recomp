#ifndef SR_SDL3VK_SMAA_H
#define SR_SDL3VK_SMAA_H

#include <stdint.h>
#include <vulkan/vulkan.h>

int sr_vk_smaa_supported(VkPhysicalDevice physical_device);
uint64_t sr_vk_smaa_shader_hash(uint64_t hash);
int sr_vk_smaa_prepare(
    VkPhysicalDevice physical_device, VkDevice device,
    VkQueue queue, uint32_t queue_family,
    VkPipelineCache pipeline_cache,
    uint32_t width, uint32_t height);
VkImage sr_vk_smaa_record(
    VkCommandBuffer command_buffer, VkImage source,
    uint32_t source_width, uint32_t source_height);
void sr_vk_smaa_shutdown(void);

#endif

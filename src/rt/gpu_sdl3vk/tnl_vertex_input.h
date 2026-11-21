#ifndef GE_VK_TNL_VERTEX_INPUT_H
#define GE_VK_TNL_VERTEX_INPUT_H

#include "../ge_tnl.h"

#include <stddef.h>
#include <vulkan/vulkan.h>

static inline void ge_vk_tnl_vertex_input_layout(
    VkVertexInputBindingDescription *binding,
    VkVertexInputAttributeDescription attributes[4]) {
    *binding = (VkVertexInputBindingDescription){
        0, sizeof(GeTnlVertex), VK_VERTEX_INPUT_RATE_VERTEX
    };
    attributes[0] = (VkVertexInputAttributeDescription){
        0, 0, VK_FORMAT_R32G32B32_SFLOAT,
        (uint32_t)offsetof(GeTnlVertex, position)
    };
    attributes[1] = (VkVertexInputAttributeDescription){
        1, 0, VK_FORMAT_R32G32B32_SFLOAT,
        (uint32_t)offsetof(GeTnlVertex, normal)
    };
    attributes[2] = (VkVertexInputAttributeDescription){
        2, 0, VK_FORMAT_R32G32_SFLOAT,
        (uint32_t)offsetof(GeTnlVertex, texcoord)
    };
    attributes[3] = (VkVertexInputAttributeDescription){
        3, 0, VK_FORMAT_R8G8B8A8_UINT,
        (uint32_t)offsetof(GeTnlVertex, color)
    };
}

#endif

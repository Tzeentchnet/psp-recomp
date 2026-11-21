#include "ge_tnl.h"
#include "gpu_sdl3vk/shader_state.h"
#include "gpu_sdl3vk/tnl_vertex_input.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static const uint32_t k_tnl_vertex_shader[] =
#include "../../src/rt/gpu_sdl3vk/psp_tnl_validation_vert.inc"
;

static const uint32_t k_tnl_points_vertex_shader[] =
#include "../../src/rt/gpu_sdl3vk/psp_tnl_points_validation_vert.inc"
;

static const uint32_t k_tnl_sprites_vertex_shader[] =
#include "../../src/rt/gpu_sdl3vk/psp_tnl_sprites_validation_vert.inc"
;

static const uint32_t k_tnl_through_sprites_vertex_shader[] =
#include "../../src/rt/gpu_sdl3vk/psp_tnl_through_sprites_validation_vert.inc"
;

static const uint32_t k_tnl_transform_compute[] =
#include "../../src/rt/gpu_sdl3vk/psp_tnl_transform_comp.inc"
;

static const uint32_t k_tnl_triangles_compute[] =
#include "../../src/rt/gpu_sdl3vk/psp_tnl_triangles_comp.inc"
;

static const uint32_t k_texture_decode_compute[] =
#include "../../src/rt/gpu_sdl3vk/psp_texture_decode_comp.inc"
;

typedef struct TestBuffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *map;
} TestBuffer;

static uint32_t s_validation_messages;

static VKAPI_ATTR VkBool32 VKAPI_CALL validation_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void *user_data) {
    (void)type;
    (void)user_data;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        fprintf(
            stderr, "Vulkan validation: %s\n",
            data && data->pMessage ? data->pMessage : "<no message>");
        s_validation_messages++;
    }
    return VK_FALSE;
}

static int has_extension(
    const VkExtensionProperties *extensions, uint32_t count,
    const char *name) {
    for (uint32_t i = 0; i < count; i++)
        if (strcmp(extensions[i].extensionName, name) == 0)
            return 1;
    return 0;
}

static int has_layer(
    const VkLayerProperties *layers, uint32_t count,
    const char *name) {
    for (uint32_t i = 0; i < count; i++)
        if (strcmp(layers[i].layerName, name) == 0)
            return 1;
    return 0;
}

static uint32_t find_memory_type(
    VkPhysicalDevice physical, uint32_t bits,
    VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & required) ==
                required) {
            return i;
        }
    }
    return UINT32_MAX;
}

static int create_buffer(
    VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
    VkBufferUsageFlags usage, TestBuffer *out) {
    VkBufferCreateInfo buffer_info = {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO
    };
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result =
        vkCreateBuffer(device, &buffer_info, NULL, &out->buffer);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateBuffer failed (%d)\n", result);
        return 0;
    }

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(
        device, out->buffer, &requirements);
    uint32_t memory_type = find_memory_type(
        physical, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory_type == UINT32_MAX) {
        fprintf(stderr, "no coherent host-visible Vulkan memory type\n");
        return 0;
    }
    VkMemoryAllocateInfo allocation = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
    };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    result = vkAllocateMemory(
        device, &allocation, NULL, &out->memory);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateMemory failed (%d)\n", result);
        return 0;
    }
    result = vkBindBufferMemory(
        device, out->buffer, out->memory, 0);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkBindBufferMemory failed (%d)\n", result);
        return 0;
    }
    result = vkMapMemory(
        device, out->memory, 0, VK_WHOLE_SIZE, 0, &out->map);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkMapMemory failed (%d)\n", result);
        return 0;
    }
    return 1;
}

static void destroy_buffer(VkDevice device, TestBuffer *buffer) {
    if (buffer->map)
        vkUnmapMemory(device, buffer->memory);
    if (buffer->buffer)
        vkDestroyBuffer(device, buffer->buffer, NULL);
    if (buffer->memory)
        vkFreeMemory(device, buffer->memory, NULL);
    memset(buffer, 0, sizeof(*buffer));
}

static int compare_float(
    const char *name, uint32_t vertex, uint32_t component,
    float actual, float expected, float tolerance) {
    if (fabsf(actual - expected) <= tolerance)
        return 1;
    fprintf(
        stderr,
        "%s[%u].%u mismatch: got %.9g, expected %.9g\n",
        name, vertex, component, actual, expected);
    return 0;
}

static uint32_t direct_fixture_raw(
    uint32_t format, uint32_t x, uint32_t y) {
    if (y == 0u && x == 0u) return 0;
    if (y == 0u && x == 1u)
        return format == 3u ? UINT32_MAX : 0xffffu;
    if (y == 0u && x == 2u) {
        static const uint32_t edge_cases[4] = {
            0x07e0u, 0x7fffu, 0xf00fu, 0x00ff7f01u,
        };
        return edge_cases[format & 3u];
    }
    uint32_t seed = x * 37u + y * 73u + x * y * 11u;
    switch (format) {
        case 0:
            return (seed & 0x1fu) |
                (((seed >> 2) & 0x3fu) << 5) |
                (((seed >> 5) & 0x1fu) << 11);
        case 1:
            return (seed & 0x1fu) |
                (((seed >> 2) & 0x1fu) << 5) |
                (((seed >> 5) & 0x1fu) << 10) |
                (((x + y) & 1u) << 15);
        case 2:
            return (seed & 0x0fu) |
                (((seed >> 2) & 0x0fu) << 4) |
                (((seed >> 4) & 0x0fu) << 8) |
                (((seed >> 6) & 0x0fu) << 12);
        default:
            return ((seed + 3u) & 0xffu) |
                (((seed * 3u + 17u) & 0xffu) << 8) |
                (((seed * 5u + 29u) & 0xffu) << 16) |
                (((seed * 7u + 41u) & 0xffu) << 24);
    }
}

static void put_raw_texel(
    uint8_t *destination, uint32_t offset,
    uint32_t raw, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++)
        destination[offset + i] =
            (uint8_t)(raw >> (i * 8u));
}

static uint32_t indexed_fixture_raw(
    uint32_t format, uint32_t x, uint32_t y) {
    uint32_t bits = 1u << (format - 2u);
    uint32_t mask = bits == 32u
        ? UINT32_MAX : (1u << bits) - 1u;
    if (y == 0u && x == 0u) return 0;
    if (y == 0u && x == 1u) return mask;
    uint32_t seed =
        x * 0x01010101u + y * 0x10204081u +
        x * y * 0x00010011u;
    return seed & mask;
}

static void put_indexed_texel(
    uint8_t *destination, uint32_t base,
    uint32_t stride, uint32_t format,
    uint32_t swizzled, uint32_t x, uint32_t y,
    uint32_t raw) {
    uint32_t bits = 1u << (format - 2u);
    uint32_t offset = base +
        ge_texture_texel_byte_offset(
            stride, x, y, bits, (int)swizzled);
    if (format == 4u) {
        if (x & 1u)
            destination[offset] =
                (uint8_t)((destination[offset] & 0x0fu) |
                          ((raw & 0x0fu) << 4));
        else
            destination[offset] =
                (uint8_t)((destination[offset] & 0xf0u) |
                          (raw & 0x0fu));
        return;
    }
    put_raw_texel(
        destination, offset, raw, bits >> 3);
}

static void fill_dxt_block(
    uint8_t *block, uint32_t format, uint32_t variant) {
    uint32_t block_size = format == 8u ? 8u : 16u;
    memset(block, 0, block_size);
    for (uint32_t y = 0; y < 4; y++) {
        uint32_t line = 0;
        for (uint32_t x = 0; x < 4; x++)
            line |= ((x + y + variant) & 3u) << (x * 2u);
        block[y] = (uint8_t)line;
    }
    uint32_t color1 = (variant & 1u) ? 0x001fu : 0xf800u;
    uint32_t color2 = (variant & 1u) ? 0x07e0u : 0x001fu;
    put_raw_texel(block, 4, color1, 2);
    put_raw_texel(block, 6, color2, 2);
    if (format == 9u) {
        for (uint32_t y = 0; y < 4; y++) {
            uint32_t alpha_line = 0;
            for (uint32_t x = 0; x < 4; x++) {
                uint32_t alpha =
                    (y * 4u + x + variant) & 15u;
                alpha_line |= alpha << (x * 4u);
            }
            put_raw_texel(
                block, 8u + y * 2u, alpha_line, 2);
        }
    } else if (format == 10u) {
        uint64_t alpha_data = 0;
        for (uint32_t pixel = 0; pixel < 16; pixel++)
            alpha_data |=
                (uint64_t)((pixel + variant) & 7u) <<
                (pixel * 3u);
        for (uint32_t i = 0; i < 6; i++)
            block[8u + i] =
                (uint8_t)(alpha_data >> (i * 8u));
        block[14] = (variant & 1u) ? 16u : 240u;
        block[15] = (variant & 1u) ? 240u : 16u;
    }
}

static int run_texture_decode_test(
    VkPhysicalDevice physical, VkDevice device, VkQueue queue,
    uint32_t queue_family) {
    enum {
        DIRECT_JOB_COUNT = 8,
        INDEXED_JOB_COUNT = 8,
        DXT_JOB_COUNT = 3,
        JOB_COUNT =
            DIRECT_JOB_COUNT + INDEXED_JOB_COUNT + DXT_JOB_COUNT,
        WIDTH = 7,
        HEIGHT = 9,
        DIRECT_STRIDE = 8,
        INDEXED_STRIDE = 32,
        DXT_STRIDE = 8,
        SOURCE_SLOT_BYTES = 2048,
        CLUT_SLOT_BYTES = GE_TEXTURE_RAW_CLUT_BYTES,
        DESTINATION_SLOT_PIXELS = WIDTH * HEIGHT,
    };
    int status = 0;
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    TestBuffer job_buffer = {0};
    TestBuffer source_buffer = {0};
    TestBuffer clut_buffer = {0};
    TestBuffer destination_buffer = {0};
    GeTextureRawJob jobs[JOB_COUNT];
    uint8_t sources[JOB_COUNT * SOURCE_SLOT_BYTES];
    uint8_t cluts[INDEXED_JOB_COUNT * CLUT_SLOT_BYTES];
    uint32_t expected[JOB_COUNT * DESTINATION_SLOT_PIXELS];
    memset(jobs, 0, sizeof(jobs));
    memset(sources, 0, sizeof(sources));
    memset(cluts, 0, sizeof(cluts));
    memset(expected, 0, sizeof(expected));

    for (uint32_t job_index = 0;
         job_index < DIRECT_JOB_COUNT; job_index++) {
        uint32_t format = job_index & 3u;
        uint32_t swizzled = job_index >> 2;
        uint32_t bits = format == 3u ? 32u : 16u;
        uint32_t rows = swizzled
            ? (HEIGHT + 7u) & ~7u : HEIGHT;
        GeTextureRawJob *job = &jobs[job_index];
        job->abi_version = GE_TEXTURE_RAW_JOB_ABI_VERSION;
        job->flags = swizzled ? GE_TEXTURE_RAW_SWIZZLED : 0u;
        job->format = format;
        job->mip_level = job_index;
        job->source_byte_offset =
            job_index * SOURCE_SLOT_BYTES;
        job->source_byte_count =
            (DIRECT_STRIDE * rows * bits + 7u) >> 3;
        job->source_stride = DIRECT_STRIDE;
        job->source_width = WIDTH;
        job->source_height = HEIGHT;
        job->destination_pixel_offset =
            job_index * DESTINATION_SLOT_PIXELS;
        job->destination_width = WIDTH;
        job->destination_height = HEIGHT;
        for (uint32_t y = 0; y < HEIGHT; y++) {
            for (uint32_t x = 0; x < WIDTH; x++) {
                uint32_t raw =
                    direct_fixture_raw(format, x, y);
                uint32_t offset =
                    ge_texture_texel_byte_offset(
                        DIRECT_STRIDE, x, y, bits,
                        (int)swizzled);
                put_raw_texel(
                    sources,
                    job->source_byte_offset + offset,
                    raw, bits >> 3);
                expected[
                    job->destination_pixel_offset +
                    y * WIDTH + x] =
                    ge_texture_unpack_direct_rgba(
                        raw, format);
            }
        }
    }

    static const uint32_t mip_levels[4] = {2, 1, 3, 2};
    for (uint32_t indexed_job = 0;
         indexed_job < INDEXED_JOB_COUNT; indexed_job++) {
        uint32_t job_index = DIRECT_JOB_COUNT + indexed_job;
        uint32_t format = 4u + (indexed_job & 3u);
        uint32_t palette_format = indexed_job & 3u;
        uint32_t swizzled = indexed_job >> 2;
        uint32_t bits = 1u << (format - 2u);
        uint32_t rows = swizzled
            ? (HEIGHT + 7u) & ~7u : HEIGHT;
        uint32_t texture_mode = swizzled ? 0u : 0x100u;
        uint32_t clut_format =
            palette_format |
            (1u << 2) |
            (0x3fu << 8) |
            (1u << 16);
        uint32_t clut_base = indexed_job * CLUT_SLOT_BYTES;
        uint32_t entry_bytes =
            palette_format == 3u ? 4u : 2u;
        uint32_t entry_count =
            CLUT_SLOT_BYTES / entry_bytes;
        for (uint32_t entry = 0;
             entry < entry_count; entry++) {
            uint32_t raw = direct_fixture_raw(
                palette_format, entry % WIDTH,
                (entry / WIDTH) % HEIGHT);
            put_raw_texel(
                cluts, clut_base + entry * entry_bytes,
                raw, entry_bytes);
        }

        GeTextureRawJob *job = &jobs[job_index];
        job->abi_version = GE_TEXTURE_RAW_JOB_ABI_VERSION;
        job->flags = GE_TEXTURE_RAW_INDEXED |
            (swizzled ? GE_TEXTURE_RAW_SWIZZLED |
                 GE_TEXTURE_RAW_CLUT_SHARED : 0u);
        job->format = format;
        job->mip_level = mip_levels[format - 4u];
        job->source_byte_offset =
            job_index * SOURCE_SLOT_BYTES;
        job->source_byte_count =
            (INDEXED_STRIDE * rows * bits + 7u) >> 3;
        job->source_stride = INDEXED_STRIDE;
        job->source_width = WIDTH;
        job->source_height = HEIGHT;
        job->clut_byte_offset = clut_base;
        job->clut_byte_count = CLUT_SLOT_BYTES;
        job->clut_format = clut_format;
        job->texture_mode = texture_mode;
        job->destination_pixel_offset =
            job_index * DESTINATION_SLOT_PIXELS;
        job->destination_width = WIDTH;
        job->destination_height = HEIGHT;

        for (uint32_t y = 0; y < HEIGHT; y++) {
            for (uint32_t x = 0; x < WIDTH; x++) {
                uint32_t raw = indexed_fixture_raw(
                    format, x, y);
                put_indexed_texel(
                    sources, job->source_byte_offset,
                    INDEXED_STRIDE, format, swizzled,
                    x, y, raw);
                uint32_t palette_index =
                    ge_texture_clut_index(
                        raw, format, job->mip_level,
                        clut_format, texture_mode);
                expected[
                    job->destination_pixel_offset +
                    y * WIDTH + x] =
                    ge_texture_clut_rgba(
                        cluts + clut_base,
                        palette_index, clut_format);
            }
        }
    }

    for (uint32_t dxt_job = 0;
         dxt_job < DXT_JOB_COUNT; dxt_job++) {
        uint32_t job_index =
            DIRECT_JOB_COUNT + INDEXED_JOB_COUNT + dxt_job;
        uint32_t format = 8u + dxt_job;
        uint32_t block_size = format == 8u ? 8u : 16u;
        uint32_t blocks_per_row =
            (DXT_STRIDE + 3u) >> 2;
        uint32_t block_rows = (HEIGHT + 3u) >> 2;
        GeTextureRawJob *job = &jobs[job_index];
        job->abi_version = GE_TEXTURE_RAW_JOB_ABI_VERSION;
        job->flags = GE_TEXTURE_RAW_BLOCK_COMPRESSED;
        job->format = format;
        job->source_byte_offset =
            job_index * SOURCE_SLOT_BYTES;
        job->source_byte_count =
            blocks_per_row * block_rows * block_size;
        job->source_stride = DXT_STRIDE;
        job->source_width = WIDTH;
        job->source_height = HEIGHT;
        job->destination_pixel_offset =
            job_index * DESTINATION_SLOT_PIXELS;
        job->destination_width = WIDTH;
        job->destination_height = HEIGHT;
        for (uint32_t block_y = 0;
             block_y < block_rows; block_y++) {
            for (uint32_t block_x = 0;
                 block_x < blocks_per_row; block_x++) {
                uint32_t block_index =
                    block_y * blocks_per_row + block_x;
                fill_dxt_block(
                    sources + job->source_byte_offset +
                        block_index * block_size,
                    format, block_index);
            }
        }
        for (uint32_t y = 0; y < HEIGHT; y++) {
            for (uint32_t x = 0; x < WIDTH; x++) {
                uint32_t block_index =
                    (y >> 2) * blocks_per_row + (x >> 2);
                const uint8_t *block =
                    sources + job->source_byte_offset +
                    block_index * block_size;
                expected[
                    job->destination_pixel_offset +
                    y * WIDTH + x] =
                    ge_texture_dxt_decode(
                        block, format, x, y);
            }
        }
    }

    if (!create_buffer(
            physical, device, sizeof(jobs),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &job_buffer) ||
        !create_buffer(
            physical, device, sizeof(sources),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &source_buffer) ||
        !create_buffer(
            physical, device, sizeof(cluts),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &clut_buffer) ||
        !create_buffer(
            physical, device, sizeof(expected),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &destination_buffer)) {
        goto cleanup;
    }
    memcpy(job_buffer.map, jobs, sizeof(jobs));
    memcpy(source_buffer.map, sources, sizeof(sources));
    memcpy(clut_buffer.map, cluts, sizeof(cluts));
    memset(destination_buffer.map, 0xcd, sizeof(expected));

    VkDescriptorSetLayoutBinding bindings[4];
    memset(bindings, 0, sizeof(bindings));
    for (uint32_t i = 0; i < 4; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layout_info.bindingCount = 4;
    layout_info.pBindings = bindings;
    VkResult result = vkCreateDescriptorSetLayout(
        device, &layout_info, NULL, &descriptor_layout);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "texture descriptor layout failed (%d)\n", result);
        goto cleanup;
    }
    VkDescriptorPoolSize pool_size = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4
    };
    VkDescriptorPoolCreateInfo pool_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    result = vkCreateDescriptorPool(
        device, &pool_info, NULL, &descriptor_pool);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "texture descriptor pool failed (%d)\n", result);
        goto cleanup;
    }
    VkDescriptorSetAllocateInfo set_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    set_info.descriptorPool = descriptor_pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &descriptor_layout;
    result = vkAllocateDescriptorSets(
        device, &set_info, &descriptor_set);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "texture descriptor set failed (%d)\n", result);
        goto cleanup;
    }

    TestBuffer *buffers[4] = {
        &job_buffer, &source_buffer,
        &clut_buffer, &destination_buffer,
    };
    VkDescriptorBufferInfo buffer_info[4];
    VkWriteDescriptorSet writes[4];
    memset(buffer_info, 0, sizeof(buffer_info));
    memset(writes, 0, sizeof(writes));
    for (uint32_t i = 0; i < 4; i++) {
        buffer_info[i].buffer = buffers[i]->buffer;
        buffer_info[i].range = VK_WHOLE_SIZE;
        writes[i].sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptor_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffer_info[i];
    }
    vkUpdateDescriptorSets(device, 4, writes, 0, NULL);

    VkPushConstantRange push_range;
    memset(&push_range, 0, sizeof(push_range));
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.size = sizeof(uint32_t);
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &descriptor_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    result = vkCreatePipelineLayout(
        device, &pipeline_layout_info, NULL,
        &pipeline_layout);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "texture pipeline layout failed (%d)\n", result);
        goto cleanup;
    }
    VkShaderModuleCreateInfo shader_info = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
    };
    shader_info.codeSize = sizeof(k_texture_decode_compute);
    shader_info.pCode = k_texture_decode_compute;
    result = vkCreateShaderModule(
        device, &shader_info, NULL, &shader);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "texture shader module failed (%d)\n", result);
        goto cleanup;
    }
    VkComputePipelineCreateInfo compute_info = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
    };
    compute_info.layout = pipeline_layout;
    compute_info.stage.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    compute_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    compute_info.stage.module = shader;
    compute_info.stage.pName = "main";
    result = vkCreateComputePipelines(
        device, VK_NULL_HANDLE, 1, &compute_info,
        NULL, &pipeline);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "texture compute pipeline failed (%d)\n", result);
        goto cleanup;
    }

    VkCommandPoolCreateInfo command_pool_info = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO
    };
    command_pool_info.queueFamilyIndex = queue_family;
    result = vkCreateCommandPool(
        device, &command_pool_info, NULL, &command_pool);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "texture command pool failed (%d)\n", result);
        goto cleanup;
    }
    VkCommandBufferAllocateInfo command_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
    };
    command_info.commandPool = command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(
        device, &command_info, &command);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "texture command allocation failed (%d)\n", result);
        goto cleanup;
    }
    VkFenceCreateInfo fence_info = {
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    };
    result = vkCreateFence(device, &fence_info, NULL, &fence);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "texture fence failed (%d)\n", result);
        goto cleanup;
    }

    VkCommandBufferBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    result = vkBeginCommandBuffer(command, &begin_info);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "texture command begin failed (%d)\n", result);
        goto cleanup;
    }
    vkCmdBindDescriptorSets(
        command, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipeline_layout, 0, 1, &descriptor_set, 0, NULL);
    vkCmdBindPipeline(
        command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    for (uint32_t job_index = 0;
         job_index < JOB_COUNT; job_index++) {
        vkCmdPushConstants(
            command, pipeline_layout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(job_index), &job_index);
        vkCmdDispatch(
            command, (WIDTH + 7u) / 8u,
            (HEIGHT + 7u) / 8u, 1);
    }
    VkBufferMemoryBarrier destination_barrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER
    };
    destination_barrier.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT;
    destination_barrier.dstAccessMask =
        VK_ACCESS_HOST_READ_BIT;
    destination_barrier.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    destination_barrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    destination_barrier.buffer = destination_buffer.buffer;
    destination_barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0,
        0, NULL, 1, &destination_barrier, 0, NULL);
    result = vkEndCommandBuffer(command);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "texture command end failed (%d)\n", result);
        goto cleanup;
    }
    VkSubmitInfo submit = {
        VK_STRUCTURE_TYPE_SUBMIT_INFO
    };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    result = vkQueueSubmit(queue, 1, &submit, fence);
    if (result != VK_SUCCESS ||
        vkWaitForFences(
            device, 1, &fence, VK_TRUE,
            UINT64_MAX) != VK_SUCCESS) {
        fprintf(
            stderr,
            "texture compute submission failed (%d)\n", result);
        goto cleanup;
    }

    const uint32_t *actual =
        (const uint32_t *)destination_buffer.map;
    for (uint32_t job_index = 0;
         job_index < JOB_COUNT; job_index++) {
        for (uint32_t y = 0; y < HEIGHT; y++) {
            for (uint32_t x = 0; x < WIDTH; x++) {
                uint32_t index =
                    job_index * DESTINATION_SLOT_PIXELS +
                    y * WIDTH + x;
                if (actual[index] != expected[index]) {
                    fprintf(
                        stderr,
                        "texture format %u %s pixel (%u,%u) "
                        "mismatch: got 0x%08x, expected 0x%08x\n",
                        jobs[job_index].format,
                        (jobs[job_index].flags &
                         GE_TEXTURE_RAW_SWIZZLED) != 0
                            ? "swizzled" : "linear",
                        x, y, actual[index], expected[index]);
                    goto cleanup;
                }
            }
        }
    }
    status = 1;

cleanup:
    if (device)
        vkDeviceWaitIdle(device);
    if (fence)
        vkDestroyFence(device, fence, NULL);
    if (command_pool)
        vkDestroyCommandPool(device, command_pool, NULL);
    if (pipeline)
        vkDestroyPipeline(device, pipeline, NULL);
    if (shader)
        vkDestroyShaderModule(device, shader, NULL);
    if (pipeline_layout)
        vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    if (descriptor_pool)
        vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (descriptor_layout)
        vkDestroyDescriptorSetLayout(
            device, descriptor_layout, NULL);
    destroy_buffer(device, &destination_buffer);
    destroy_buffer(device, &clut_buffer);
    destroy_buffer(device, &source_buffer);
    destroy_buffer(device, &job_buffer);
    return status;
}

static int run_preprocess_test(
    VkPhysicalDevice physical, VkDevice device, VkQueue queue,
    uint32_t queue_family) {
    int status = 0;
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule transform_shader = VK_NULL_HANDLE;
    VkShaderModule triangles_shader = VK_NULL_HANDLE;
    VkPipeline transform_pipeline = VK_NULL_HANDLE;
    VkPipeline triangles_pipeline = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    TestBuffer vertex_buffer = {0};
    TestBuffer index_buffer = {0};
    TestBuffer state_buffer = {0};
    TestBuffer clip_buffer = {0};
    TestBuffer screen_buffer = {0};
    enum {
        VERTEX_COUNT = 6,
        INDEX_COUNT = 6,
        TRIANGLE_COUNT = 2,
        SCREEN_COUNT = TRIANGLE_COUNT * 6,
        DIRECT_SCREEN_BASE = SCREEN_COUNT,
        TOTAL_SCREEN_COUNT = SCREEN_COUNT + VERTEX_COUNT,
    };

    GeTnlVertex vertices[VERTEX_COUNT];
    memset(vertices, 0, sizeof(vertices));
    const float positions[VERTEX_COUNT][3] = {
        {-0.5f, -0.5f, -2.0f},
        {0.5f, -0.5f, 0.0f},
        {0.0f, 0.5f, 0.0f},
        {100.0f, 0.0f, 0.0f},
        {0.5f, -0.5f, 0.0f},
        {0.0f, 0.5f, 0.0f},
    };
    for (uint32_t i = 0; i < VERTEX_COUNT; i++) {
        memcpy(vertices[i].position, positions[i], sizeof(positions[i]));
        vertices[i].normal[2] = 1.0f;
        vertices[i].texcoord[0] = (float)i * 0.25f;
        vertices[i].texcoord[1] = (float)i * 0.125f;
        vertices[i].color[0] = (uint8_t)(10u + i * 20u);
        vertices[i].color[1] = (uint8_t)(30u + i * 10u);
        vertices[i].color[2] = (uint8_t)(50u + i * 5u);
        vertices[i].color[3] = 255;
    }
    uint32_t indices[INDEX_COUNT] = {0, 1, 2, 3, 4, 5};

    GeTnlState state;
    memset(&state, 0, sizeof(state));
    state.abi_version = GE_TNL_ABI_VERSION;
    state.flags = GE_TNL_STATE_GOURAUD | GE_TNL_STATE_DEPTH_CLIP;
    state.world[0] = state.world[4] = state.world[8] = 1.0f;
    state.view[0] = state.view[4] = state.view[8] = 1.0f;
    state.projection[0] = state.projection[5] =
        state.projection[10] = state.projection[15] = 1.0f;
    state.texture_scale[0] = state.texture_scale[1] = 1.0f;
    state.texture_width = state.texture_height = 1;
    state.viewport_scale[0] = 240.0f;
    state.viewport_scale[1] = -136.0f;
    state.viewport_scale[2] = 32767.5f;
    state.viewport_center[0] = 240.0f;
    state.viewport_center[1] = 136.0f;
    state.viewport_center[2] = 32767.5f;
    GeTnlGpuState gpu_state;
    ge_tnl_gpu_state_build(&state, &gpu_state);

    GeTnlClipVertex expected_clip[VERTEX_COUNT];
    for (uint32_t i = 0; i < VERTEX_COUNT; i++) {
        if (!ge_tnl_reference_clip_transform(
                &state, &vertices[i], &expected_clip[i])) {
            fprintf(stderr, "CPU clip transform rejected vertex %u\n", i);
            goto cleanup;
        }
    }
    GeTnlScreenVertex expected_screen[TOTAL_SCREEN_COUNT];
    memset(expected_screen, 0, sizeof(expected_screen));
    for (uint32_t i = 0; i < SCREEN_COUNT; i++) {
        expected_screen[i].position[3] = 1.0f;
        expected_screen[i].fog = 1.0f;
    }
    uint32_t first_count = ge_tnl_reference_preprocess_triangle(
        &state, expected_clip, expected_screen);
    uint32_t second_count = ge_tnl_reference_preprocess_triangle(
        &state, expected_clip + 3, expected_screen + 6);
    if (first_count != 6 || second_count != 0) {
        fprintf(
            stderr,
            "unexpected CPU preprocess counts: %u, %u\n",
            first_count, second_count);
        goto cleanup;
    }
    for (uint32_t i = 0; i < VERTEX_COUNT; i++) {
        if (!ge_tnl_clip_project(
                &state, &expected_clip[i],
                &expected_screen[DIRECT_SCREEN_BASE + i])) {
            fprintf(stderr, "CPU direct projection rejected vertex %u\n", i);
            goto cleanup;
        }
    }

    if (!create_buffer(
            physical, device, sizeof(vertices),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &vertex_buffer) ||
        !create_buffer(
            physical, device, sizeof(indices),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &index_buffer) ||
        !create_buffer(
            physical, device, sizeof(gpu_state),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &state_buffer) ||
        !create_buffer(
            physical, device, sizeof(expected_clip),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &clip_buffer) ||
        !create_buffer(
            physical, device, sizeof(expected_screen),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &screen_buffer)) {
        goto cleanup;
    }
    memcpy(vertex_buffer.map, vertices, sizeof(vertices));
    memcpy(index_buffer.map, indices, sizeof(indices));
    memcpy(state_buffer.map, &gpu_state, sizeof(gpu_state));
    memset(clip_buffer.map, 0, sizeof(expected_clip));
    memset(screen_buffer.map, 0, sizeof(expected_screen));

    VkDescriptorSetLayoutBinding bindings[5];
    memset(bindings, 0, sizeof(bindings));
    for (uint32_t i = 0; i < 5; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layout_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layout_info.bindingCount = 5;
    layout_info.pBindings = bindings;
    VkResult result = vkCreateDescriptorSetLayout(
        device, &layout_info, NULL, &descriptor_layout);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDescriptorSetLayout failed (%d)\n", result);
        goto cleanup;
    }
    VkDescriptorPoolSize pool_size = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5
    };
    VkDescriptorPoolCreateInfo pool_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    result = vkCreateDescriptorPool(
        device, &pool_info, NULL, &descriptor_pool);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDescriptorPool failed (%d)\n", result);
        goto cleanup;
    }
    VkDescriptorSetAllocateInfo set_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    set_info.descriptorPool = descriptor_pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &descriptor_layout;
    result = vkAllocateDescriptorSets(
        device, &set_info, &descriptor_set);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateDescriptorSets failed (%d)\n", result);
        goto cleanup;
    }

    TestBuffer *buffers[5] = {
        &vertex_buffer, &index_buffer, &state_buffer,
        &clip_buffer, &screen_buffer,
    };
    VkDescriptorBufferInfo buffer_info[5];
    VkWriteDescriptorSet writes[5];
    memset(buffer_info, 0, sizeof(buffer_info));
    memset(writes, 0, sizeof(writes));
    for (uint32_t i = 0; i < 5; i++) {
        buffer_info[i].buffer = buffers[i]->buffer;
        buffer_info[i].offset = 0;
        buffer_info[i].range = VK_WHOLE_SIZE;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptor_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffer_info[i];
    }
    vkUpdateDescriptorSets(device, 5, writes, 0, NULL);

    VkPushConstantRange push_range;
    memset(&push_range, 0, sizeof(push_range));
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.size = 8u * sizeof(uint32_t);
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &descriptor_layout;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    result = vkCreatePipelineLayout(
        device, &pipeline_layout_info, NULL, &pipeline_layout);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreatePipelineLayout failed (%d)\n", result);
        goto cleanup;
    }

    VkShaderModuleCreateInfo shader_info = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
    };
    shader_info.codeSize = sizeof(k_tnl_transform_compute);
    shader_info.pCode = k_tnl_transform_compute;
    result = vkCreateShaderModule(
        device, &shader_info, NULL, &transform_shader);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "transform shader module failed (%d)\n", result);
        goto cleanup;
    }
    shader_info.codeSize = sizeof(k_tnl_triangles_compute);
    shader_info.pCode = k_tnl_triangles_compute;
    result = vkCreateShaderModule(
        device, &shader_info, NULL, &triangles_shader);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "triangles shader module failed (%d)\n", result);
        goto cleanup;
    }

    VkComputePipelineCreateInfo compute_info = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
    };
    compute_info.layout = pipeline_layout;
    compute_info.stage.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    compute_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    compute_info.stage.pName = "main";
    compute_info.stage.module = transform_shader;
    result = vkCreateComputePipelines(
        device, VK_NULL_HANDLE, 1, &compute_info,
        NULL, &transform_pipeline);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "transform compute pipeline failed (%d)\n", result);
        goto cleanup;
    }
    compute_info.stage.module = triangles_shader;
    result = vkCreateComputePipelines(
        device, VK_NULL_HANDLE, 1, &compute_info,
        NULL, &triangles_pipeline);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "triangles compute pipeline failed (%d)\n", result);
        goto cleanup;
    }

    VkCommandPoolCreateInfo command_pool_info = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO
    };
    command_pool_info.queueFamilyIndex = queue_family;
    result = vkCreateCommandPool(
        device, &command_pool_info, NULL, &command_pool);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "compute command pool failed (%d)\n", result);
        goto cleanup;
    }
    VkCommandBufferAllocateInfo command_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
    };
    command_info.commandPool = command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(
        device, &command_info, &command);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "compute command allocation failed (%d)\n", result);
        goto cleanup;
    }
    VkFenceCreateInfo fence_info = {
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    };
    result = vkCreateFence(device, &fence_info, NULL, &fence);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "compute fence failed (%d)\n", result);
        goto cleanup;
    }

    uint32_t job_data[8] = {
        0, 0, 0, 0,
        VERTEX_COUNT, TRIANGLE_COUNT, 0, 0,
    };
    VkCommandBufferBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    result = vkBeginCommandBuffer(command, &begin_info);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "compute command begin failed (%d)\n", result);
        goto cleanup;
    }
    vkCmdBindDescriptorSets(
        command, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipeline_layout, 0, 1, &descriptor_set, 0, NULL);
    vkCmdPushConstants(
        command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(job_data), job_data);
    vkCmdBindPipeline(
        command, VK_PIPELINE_BIND_POINT_COMPUTE,
        transform_pipeline);
    vkCmdDispatch(command, 1, 1, 1);
    VkBufferMemoryBarrier clip_barrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER
    };
    clip_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    clip_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    clip_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clip_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clip_barrier.buffer = clip_buffer.buffer;
    clip_barrier.offset = 0;
    clip_barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
        0, NULL, 1, &clip_barrier, 0, NULL);
    vkCmdBindPipeline(
        command, VK_PIPELINE_BIND_POINT_COMPUTE,
        triangles_pipeline);
    vkCmdDispatch(command, 1, 1, 1);
    clip_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    clip_barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
        0, NULL, 1, &clip_barrier, 0, NULL);
    uint32_t direct_job_data[8] = {
        0, 0, 0, DIRECT_SCREEN_BASE,
        VERTEX_COUNT, 0, 1, 0,
    };
    vkCmdPushConstants(
        command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(direct_job_data), direct_job_data);
    vkCmdBindPipeline(
        command, VK_PIPELINE_BIND_POINT_COMPUTE,
        transform_pipeline);
    vkCmdDispatch(command, 1, 1, 1);
    VkBufferMemoryBarrier screen_barrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER
    };
    screen_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    screen_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    screen_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    screen_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    screen_barrier.buffer = screen_buffer.buffer;
    screen_barrier.offset = 0;
    screen_barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0,
        0, NULL, 1, &screen_barrier, 0, NULL);
    result = vkEndCommandBuffer(command);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "compute command end failed (%d)\n", result);
        goto cleanup;
    }
    VkSubmitInfo submit = {
        VK_STRUCTURE_TYPE_SUBMIT_INFO
    };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    result = vkQueueSubmit(queue, 1, &submit, fence);
    if (result != VK_SUCCESS ||
        vkWaitForFences(
            device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        fprintf(stderr, "compute submission failed (%d)\n", result);
        goto cleanup;
    }

    const GeTnlClipVertex *actual_clip =
        (const GeTnlClipVertex *)clip_buffer.map;
    const GeTnlScreenVertex *actual_screen =
        (const GeTnlScreenVertex *)screen_buffer.map;
    for (uint32_t i = 0; i < VERTEX_COUNT; i++) {
        for (uint32_t component = 0; component < 4; component++) {
            if (!compare_float(
                    "clip position", i, component,
                    actual_clip[i].clip[component],
                    expected_clip[i].clip[component], 2e-5f) ||
                !compare_float(
                    "clip uv/fog", i, component,
                    actual_clip[i].uv_fog[component],
                    expected_clip[i].uv_fog[component], 2e-5f)) {
                goto cleanup;
            }
            if (actual_clip[i].color[component] !=
                expected_clip[i].color[component]) {
                fprintf(
                    stderr,
                    "clip color[%u].%u mismatch: got %u, expected %u\n",
                    i, component, actual_clip[i].color[component],
                    expected_clip[i].color[component]);
                goto cleanup;
            }
        }
    }
    for (uint32_t i = 0; i < TOTAL_SCREEN_COUNT; i++) {
        for (uint32_t component = 0; component < 4; component++) {
            if (!compare_float(
                    "screen position", i, component,
                    actual_screen[i].position[component],
                    expected_screen[i].position[component], 2e-5f)) {
                goto cleanup;
            }
        }
        for (uint32_t component = 0; component < 2; component++) {
            if (!compare_float(
                    "screen texcoord", i, component,
                    actual_screen[i].texcoord[component],
                    expected_screen[i].texcoord[component], 2e-5f)) {
                goto cleanup;
            }
        }
        if (!compare_float(
                "screen fog", i, 0, actual_screen[i].fog,
                expected_screen[i].fog, 2e-5f) ||
            actual_screen[i].color != expected_screen[i].color) {
            if (actual_screen[i].color != expected_screen[i].color) {
                fprintf(
                    stderr,
                    "screen color[%u] mismatch: got 0x%08x, expected 0x%08x\n",
                    i, actual_screen[i].color, expected_screen[i].color);
            }
            goto cleanup;
        }
    }
    status = 1;

cleanup:
    if (device)
        vkDeviceWaitIdle(device);
    if (fence)
        vkDestroyFence(device, fence, NULL);
    if (command_pool)
        vkDestroyCommandPool(device, command_pool, NULL);
    if (transform_pipeline)
        vkDestroyPipeline(device, transform_pipeline, NULL);
    if (triangles_pipeline)
        vkDestroyPipeline(device, triangles_pipeline, NULL);
    if (transform_shader)
        vkDestroyShaderModule(device, transform_shader, NULL);
    if (triangles_shader)
        vkDestroyShaderModule(device, triangles_shader, NULL);
    if (pipeline_layout)
        vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    if (descriptor_pool)
        vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (descriptor_layout)
        vkDestroyDescriptorSetLayout(device, descriptor_layout, NULL);
    destroy_buffer(device, &screen_buffer);
    destroy_buffer(device, &clip_buffer);
    destroy_buffer(device, &state_buffer);
    destroy_buffer(device, &index_buffer);
    destroy_buffer(device, &vertex_buffer);
    return status;
}

int main(void) {
    int status = 1;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorSetLayout empty_descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkShaderModule points_shader = VK_NULL_HANDLE;
    VkShaderModule sprites_shader = VK_NULL_HANDLE;
    VkShaderModule through_sprites_shader = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipeline points_pipeline = VK_NULL_HANDLE;
    VkPipeline sprites_pipeline = VK_NULL_HANDLE;
    VkPipeline through_sprites_pipeline = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    TestBuffer vertex_buffer = {0};
    TestBuffer index_buffer = {0};
    TestBuffer state_buffer = {0};
    TestBuffer output_buffer = {0};
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties device_properties;
    memset(&device_properties, 0, sizeof(device_properties));

    uint32_t extension_count = 0;
    if (vkEnumerateInstanceExtensionProperties(
            NULL, &extension_count, NULL) != VK_SUCCESS ||
        extension_count > 256u) {
        fprintf(stderr, "could not enumerate Vulkan instance extensions\n");
        goto cleanup;
    }
    VkExtensionProperties instance_extensions[256];
    if (vkEnumerateInstanceExtensionProperties(
            NULL, &extension_count,
            instance_extensions) != VK_SUCCESS) {
        fprintf(stderr, "could not read Vulkan instance extensions\n");
        goto cleanup;
    }

    const char *enabled_extensions[3];
    uint32_t enabled_extension_count = 0;
    VkInstanceCreateFlags instance_flags = 0;
    int has_debug_utils = has_extension(
        instance_extensions, extension_count,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (has_debug_utils)
        enabled_extensions[enabled_extension_count++] =
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (has_extension(
            instance_extensions, extension_count,
            VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        enabled_extensions[enabled_extension_count++] =
            VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        instance_flags |=
            VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        if (has_extension(
                instance_extensions, extension_count,
                VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
            enabled_extensions[enabled_extension_count++] =
                VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
        }
    }

    uint32_t layer_count = 0;
    if (vkEnumerateInstanceLayerProperties(
            &layer_count, NULL) != VK_SUCCESS ||
        layer_count > 64u) {
        fprintf(stderr, "could not enumerate Vulkan layers\n");
        goto cleanup;
    }
    VkLayerProperties layers[64];
    if (vkEnumerateInstanceLayerProperties(
            &layer_count, layers) != VK_SUCCESS) {
        fprintf(stderr, "could not read Vulkan layers\n");
        goto cleanup;
    }
    const char *validation_layer =
        "VK_LAYER_KHRONOS_validation";
    int validation_available =
        has_layer(layers, layer_count, validation_layer);
    if (!validation_available) {
        fprintf(stderr, "Khronos Vulkan validation layer is required\n");
        goto cleanup;
    }

    VkDebugUtilsMessengerCreateInfoEXT debug_info = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT
    };
    debug_info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug_info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug_info.pfnUserCallback = validation_callback;
    VkValidationFeatureEnableEXT validation_feature =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation_features = {
        VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT
    };
    validation_features.enabledValidationFeatureCount = 1;
    validation_features.pEnabledValidationFeatures =
        &validation_feature;
    validation_features.pNext =
        has_debug_utils ? &debug_info : NULL;

    VkApplicationInfo application = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO
    };
    application.pApplicationName = "psp-recomp Vulkan T&L validation";
    application.applicationVersion = 1;
    application.pEngineName = "psp-recomp";
    application.engineVersion = 1;
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instance_info = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
    };
    instance_info.flags = instance_flags;
    instance_info.pApplicationInfo = &application;
    instance_info.enabledExtensionCount =
        enabled_extension_count;
    instance_info.ppEnabledExtensionNames = enabled_extensions;
    instance_info.enabledLayerCount = 1;
    instance_info.ppEnabledLayerNames = &validation_layer;
    instance_info.pNext = &validation_features;
    VkResult result =
        vkCreateInstance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed (%d)\n", result);
        goto cleanup;
    }

    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger = NULL;
    if (has_debug_utils) {
        PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
            (PFN_vkCreateDebugUtilsMessengerEXT)
                vkGetInstanceProcAddr(
                    instance, "vkCreateDebugUtilsMessengerEXT");
        destroy_messenger =
            (PFN_vkDestroyDebugUtilsMessengerEXT)
                vkGetInstanceProcAddr(
                    instance, "vkDestroyDebugUtilsMessengerEXT");
        if (!create_messenger || !destroy_messenger ||
            create_messenger(
                instance, &debug_info, NULL,
                &messenger) != VK_SUCCESS) {
            fprintf(stderr, "could not create Vulkan debug messenger\n");
            goto cleanup;
        }
    }

    uint32_t physical_count = 0;
    if (vkEnumeratePhysicalDevices(
            instance, &physical_count, NULL) != VK_SUCCESS ||
        !physical_count || physical_count > 16u) {
        fprintf(stderr, "no usable Vulkan physical device\n");
        goto cleanup;
    }
    VkPhysicalDevice physical_devices[16];
    if (vkEnumeratePhysicalDevices(
            instance, &physical_count,
            physical_devices) != VK_SUCCESS) {
        fprintf(stderr, "could not enumerate Vulkan physical devices\n");
        goto cleanup;
    }

    uint32_t queue_family = UINT32_MAX;
    for (uint32_t device_index = 0;
         device_index < physical_count &&
         queue_family == UINT32_MAX;
         device_index++) {
        uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(
            physical_devices[device_index],
            &queue_count, NULL);
        if (!queue_count || queue_count > 64u)
            continue;
        VkQueueFamilyProperties queues[64];
        vkGetPhysicalDeviceQueueFamilyProperties(
            physical_devices[device_index],
            &queue_count, queues);
        for (uint32_t i = 0; i < queue_count; i++) {
            if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                physical = physical_devices[device_index];
                queue_family = i;
                break;
            }
        }
    }
    if (!physical || queue_family == UINT32_MAX) {
        fprintf(stderr, "no Vulkan graphics queue\n");
        goto cleanup;
    }
    vkGetPhysicalDeviceProperties(physical, &device_properties);
    VkPhysicalDeviceFeatures available_features;
    vkGetPhysicalDeviceFeatures(
        physical, &available_features);
    if (!available_features.vertexPipelineStoresAndAtomics) {
        fprintf(
            stderr,
            "Vulkan device lacks vertexPipelineStoresAndAtomics\n");
        goto cleanup;
    }
    VkPhysicalDeviceFeatures enabled_features;
    memset(&enabled_features, 0, sizeof(enabled_features));
    enabled_features.vertexPipelineStoresAndAtomics = VK_TRUE;

    uint32_t device_extension_count = 0;
    if (vkEnumerateDeviceExtensionProperties(
            physical, NULL, &device_extension_count,
            NULL) != VK_SUCCESS ||
        device_extension_count > 256u) {
        fprintf(stderr, "could not enumerate Vulkan device extensions\n");
        goto cleanup;
    }
    VkExtensionProperties device_extensions[256];
    if (vkEnumerateDeviceExtensionProperties(
            physical, NULL, &device_extension_count,
            device_extensions) != VK_SUCCESS) {
        fprintf(stderr, "could not read Vulkan device extensions\n");
        goto cleanup;
    }
    const char *enabled_device_extensions[1];
    uint32_t enabled_device_extension_count = 0;
    if (has_extension(
            device_extensions, device_extension_count,
            "VK_KHR_portability_subset")) {
        enabled_device_extensions[enabled_device_extension_count++] =
            "VK_KHR_portability_subset";
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO
    };
    queue_info.queueFamilyIndex = queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info = {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO
    };
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount =
        enabled_device_extension_count;
    device_info.ppEnabledExtensionNames =
        enabled_device_extensions;
    device_info.pEnabledFeatures = &enabled_features;
    result = vkCreateDevice(
        physical, &device_info, NULL, &device);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDevice failed (%d)\n", result);
        goto cleanup;
    }
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    enum { VALIDATION_VERTEX_COUNT = 36 };
    GeTnlVertex vertices[12];
    memset(vertices, 0, sizeof(vertices));
    const float positions[3][3] = {
        {-0.5f, -0.5f, 0.0f},
        {0.5f, -0.5f, 0.25f},
        {0.0f, 0.5f, -0.25f},
    };
    const uint8_t colors[3][4] = {
        {255, 64, 32, 255},
        {16, 128, 240, 192},
        {8, 24, 48, 96},
    };
    for (uint32_t i = 0; i < 3; i++) {
        memcpy(
            vertices[i].position, positions[i],
            sizeof(vertices[i].position));
        vertices[i].normal[2] = 1.0f;
        vertices[i].texcoord[0] = (float)i * 0.25f;
        vertices[i].texcoord[1] = 1.0f - (float)i * 0.125f;
        memcpy(vertices[i].color, colors[i], sizeof(colors[i]));
    }
    const GeTnlVertex through_controls[2] = {
        {
            {17.75f, 22.5f, 1234.0f},
            {0.0f, 0.0f, 0.0f},
            {4.0f, 8.0f},
            {20, 40, 60, 80},
        },
        {
            {53.25f, 61.75f, 4321.0f},
            {0.0f, 0.0f, 0.0f},
            {68.0f, 84.0f},
            {100, 120, 140, 160},
        },
    };
    memcpy(
        &vertices[8], through_controls,
        sizeof(through_controls));
    memcpy(
        &vertices[10], through_controls,
        sizeof(through_controls));
    uint32_t indices[3] = {0, 1, 2};
    GeTnlGpuState gpu_state;
    memset(&gpu_state, 0, sizeof(gpu_state));
    for (uint32_t i = 0; i < 3; i++) {
        gpu_state.world[i][i] = 1.0f;
        gpu_state.view[i][i] = 1.0f;
        gpu_state.texture[i][i] = 1.0f;
    }
    for (uint32_t i = 0; i < 4; i++)
        gpu_state.projection[i][i] = 1.0f;
    gpu_state.texture_params[0] = 1.5f;
    gpu_state.texture_params[1] = 0.75f;
    gpu_state.texture_params[2] = 0.125f;
    gpu_state.texture_params[3] = -0.25f;
    gpu_state.viewport_scale[0] = 100.0f;
    gpu_state.viewport_scale[1] = 80.0f;
    gpu_state.viewport_scale[2] = 200.0f;
    gpu_state.viewport_center[0] = 120.0f;
    gpu_state.viewport_center[1] = 90.0f;
    gpu_state.viewport_center[2] = 1000.0f;
    gpu_state.viewport_offset_size[2] = 256.0f;
    gpu_state.viewport_offset_size[3] = 128.0f;

    if (!create_buffer(
            physical, device, sizeof(vertices),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &vertex_buffer) ||
        !create_buffer(
            physical, device, sizeof(indices),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &index_buffer) ||
        !create_buffer(
            physical, device, sizeof(gpu_state),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &state_buffer) ||
        !create_buffer(
            physical, device,
            sizeof(GeTnlValidationVertex) *
                VALIDATION_VERTEX_COUNT,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &output_buffer)) {
        goto cleanup;
    }
    memcpy(vertex_buffer.map, vertices, sizeof(vertices));
    memcpy(index_buffer.map, indices, sizeof(indices));
    memcpy(state_buffer.map, &gpu_state, sizeof(gpu_state));
    memset(
        output_buffer.map, 0,
        sizeof(GeTnlValidationVertex) *
            VALIDATION_VERTEX_COUNT);

    VkDescriptorSetLayoutBinding bindings[4];
    memset(bindings, 0, sizeof(bindings));
    for (uint32_t i = 0; i < 4; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    }
    VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    result = vkCreateDescriptorSetLayout(
        device, &descriptor_layout_info, NULL,
        &empty_descriptor_layout);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "vkCreateDescriptorSetLayout(empty) failed (%d)\n",
            result);
        goto cleanup;
    }
    descriptor_layout_info.bindingCount = 4;
    descriptor_layout_info.pBindings = bindings;
    result = vkCreateDescriptorSetLayout(
        device, &descriptor_layout_info, NULL,
        &descriptor_layout);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDescriptorSetLayout failed (%d)\n", result);
        goto cleanup;
    }

    VkPushConstantRange push_range;
    memset(&push_range, 0, sizeof(push_range));
    push_range.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT |
        VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.size = sizeof(GeVkShaderState);
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    VkDescriptorSetLayout set_layouts[4] = {
        empty_descriptor_layout,
        empty_descriptor_layout,
        empty_descriptor_layout,
        descriptor_layout,
    };
    pipeline_layout_info.setLayoutCount = 4;
    pipeline_layout_info.pSetLayouts = set_layouts;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    result = vkCreatePipelineLayout(
        device, &pipeline_layout_info, NULL,
        &pipeline_layout);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreatePipelineLayout failed (%d)\n", result);
        goto cleanup;
    }

    VkDescriptorPoolSize pool_size = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4
    };
    VkDescriptorPoolCreateInfo pool_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    result = vkCreateDescriptorPool(
        device, &pool_info, NULL, &descriptor_pool);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateDescriptorPool failed (%d)\n", result);
        goto cleanup;
    }
    VkDescriptorSetAllocateInfo set_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    set_info.descriptorPool = descriptor_pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &descriptor_layout;
    result = vkAllocateDescriptorSets(
        device, &set_info, &descriptor_set);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateDescriptorSets failed (%d)\n", result);
        goto cleanup;
    }
    VkDescriptorBufferInfo buffer_info[4] = {
        { state_buffer.buffer, 0, sizeof(gpu_state) },
        {
            output_buffer.buffer, 0,
            sizeof(GeTnlValidationVertex) *
                VALIDATION_VERTEX_COUNT
        },
        { vertex_buffer.buffer, 0, sizeof(vertices) },
        { index_buffer.buffer, 0, sizeof(indices) },
    };
    VkWriteDescriptorSet writes[4] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
    };
    for (uint32_t i = 0; i < 4; i++) {
        writes[i].dstSet = descriptor_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffer_info[i];
    }
    vkUpdateDescriptorSets(device, 4, writes, 0, NULL);

    VkShaderModuleCreateInfo shader_info = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
    };
    shader_info.codeSize = sizeof(k_tnl_vertex_shader);
    shader_info.pCode = k_tnl_vertex_shader;
    result = vkCreateShaderModule(
        device, &shader_info, NULL, &shader);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateShaderModule failed (%d)\n", result);
        goto cleanup;
    }
    shader_info.codeSize = sizeof(k_tnl_points_vertex_shader);
    shader_info.pCode = k_tnl_points_vertex_shader;
    result = vkCreateShaderModule(
        device, &shader_info, NULL, &points_shader);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "vkCreateShaderModule(points) failed (%d)\n",
            result);
        goto cleanup;
    }
    shader_info.codeSize = sizeof(k_tnl_sprites_vertex_shader);
    shader_info.pCode = k_tnl_sprites_vertex_shader;
    result = vkCreateShaderModule(
        device, &shader_info, NULL, &sprites_shader);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "vkCreateShaderModule(sprites) failed (%d)\n",
            result);
        goto cleanup;
    }
    shader_info.codeSize =
        sizeof(k_tnl_through_sprites_vertex_shader);
    shader_info.pCode =
        k_tnl_through_sprites_vertex_shader;
    result = vkCreateShaderModule(
        device, &shader_info, NULL,
        &through_sprites_shader);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "vkCreateShaderModule(through sprites) failed (%d)\n",
            result);
        goto cleanup;
    }

    VkSubpassDescription subpass;
    memset(&subpass, 0, sizeof(subpass));
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    VkRenderPassCreateInfo render_pass_info = {
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO
    };
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    result = vkCreateRenderPass(
        device, &render_pass_info, NULL, &render_pass);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateRenderPass failed (%d)\n", result);
        goto cleanup;
    }
    VkFramebufferCreateInfo framebuffer_info = {
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO
    };
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.width = 1;
    framebuffer_info.height = 1;
    framebuffer_info.layers = 1;
    result = vkCreateFramebuffer(
        device, &framebuffer_info, NULL, &framebuffer);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateFramebuffer failed (%d)\n", result);
        goto cleanup;
    }

    VkPipelineShaderStageCreateInfo stage = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
    };
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = shader;
    stage.pName = "main";
    VkVertexInputBindingDescription vertex_binding;
    VkVertexInputAttributeDescription attributes[4];
    ge_vk_tnl_vertex_input_layout(&vertex_binding, attributes);
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 4;
    vertex_input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    input_assembly.topology =
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport = {
        0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f
    };
    VkRect2D scissor = {{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo viewport_state = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
    };
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rasterization = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    rasterization.rasterizerDiscardEnable = VK_TRUE;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendStateCreateInfo color_blend = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    VkGraphicsPipelineCreateInfo pipeline_info = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO
    };
    pipeline_info.stageCount = 1;
    pipeline_info.pStages = &stage;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;
    result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipeline_info,
        NULL, &pipeline);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateGraphicsPipelines failed (%d)\n", result);
        goto cleanup;
    }
    stage.module = points_shader;
    vertex_input.vertexBindingDescriptionCount = 0;
    vertex_input.pVertexBindingDescriptions = NULL;
    vertex_input.vertexAttributeDescriptionCount = 0;
    vertex_input.pVertexAttributeDescriptions = NULL;
    result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipeline_info,
        NULL, &points_pipeline);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "vkCreateGraphicsPipelines(points) failed (%d)\n",
            result);
        goto cleanup;
    }
    stage.module = sprites_shader;
    result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipeline_info,
        NULL, &sprites_pipeline);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "vkCreateGraphicsPipelines(sprites) failed (%d)\n",
            result);
        goto cleanup;
    }
    stage.module = through_sprites_shader;
    result = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipeline_info,
        NULL, &through_sprites_pipeline);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "vkCreateGraphicsPipelines(through sprites) "
            "failed (%d)\n",
            result);
        goto cleanup;
    }

    VkCommandPoolCreateInfo command_pool_info = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO
    };
    command_pool_info.queueFamilyIndex = queue_family;
    result = vkCreateCommandPool(
        device, &command_pool_info, NULL,
        &command_pool);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateCommandPool failed (%d)\n", result);
        goto cleanup;
    }
    VkCommandBufferAllocateInfo command_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
    };
    command_info.commandPool = command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(
        device, &command_info, &command);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkAllocateCommandBuffers failed (%d)\n", result);
        goto cleanup;
    }
    VkFenceCreateInfo fence_info = {
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    };
    result = vkCreateFence(
        device, &fence_info, NULL, &fence);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateFence failed (%d)\n", result);
        goto cleanup;
    }

    VkCommandBufferBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    result = vkBeginCommandBuffer(command, &begin_info);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkBeginCommandBuffer failed (%d)\n", result);
        goto cleanup;
    }
    VkBufferMemoryBarrier input_barriers[3] = {
        { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER },
        { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER },
        { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER },
    };
    VkBuffer input_buffers[3] = {
        vertex_buffer.buffer,
        index_buffer.buffer,
        state_buffer.buffer,
    };
    VkAccessFlags input_access[3] = {
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
            VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_INDEX_READ_BIT |
            VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_SHADER_READ_BIT,
    };
    VkDeviceSize input_sizes[3] = {
        sizeof(vertices), sizeof(indices), sizeof(gpu_state)
    };
    for (uint32_t i = 0; i < 3; i++) {
        input_barriers[i].srcAccessMask =
            VK_ACCESS_HOST_WRITE_BIT;
        input_barriers[i].dstAccessMask = input_access[i];
        input_barriers[i].srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        input_barriers[i].dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        input_barriers[i].buffer = input_buffers[i];
        input_barriers[i].size = input_sizes[i];
    }
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        0, 0, NULL, 3, input_barriers, 0, NULL);
    VkRenderPassBeginInfo render_begin = {
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO
    };
    render_begin.renderPass = render_pass;
    render_begin.framebuffer = framebuffer;
    render_begin.renderArea.extent.width = 1;
    render_begin.renderArea.extent.height = 1;
    vkCmdBeginRenderPass(
        command, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(
        command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(
        command, 0, 1, &vertex_buffer.buffer, &zero);
    vkCmdBindIndexBuffer(
        command, index_buffer.buffer, 0,
        VK_INDEX_TYPE_UINT32);
    vkCmdBindDescriptorSets(
        command, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_layout, 3, 1, &descriptor_set,
        0, NULL);
    GeVkShaderState push;
    memset(&push, 0, sizeof(push));
    push.tnl[1] = 1u;
    vkCmdPushConstants(
        command, pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT |
            VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(push), &push);
    vkCmdDrawIndexed(command, 3, 1, 0, 0, 0);
    vkCmdBindPipeline(
        command, VK_PIPELINE_BIND_POINT_GRAPHICS,
        points_pipeline);
    push.tnl[2] = 0u;
    push.tnl[3] = 3u;
    vkCmdPushConstants(
        command, pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT |
            VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(push), &push);
    vkCmdDraw(command, 6, 1, 3, 0);
    vkCmdBindPipeline(
        command, VK_PIPELINE_BIND_POINT_GRAPHICS,
        sprites_pipeline);
    push.tnl[2] = 1u;
    push.tnl[3] = 9u;
    vkCmdPushConstants(
        command, pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT |
            VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(push), &push);
    vkCmdDraw(command, 6, 1, 9, 0);
    push.tnl[3] = 0x8000000fu;
    vkCmdPushConstants(
        command, pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT |
            VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(push), &push);
    vkCmdDraw(command, 6, 1, 15, 0);
    vkCmdBindPipeline(
        command, VK_PIPELINE_BIND_POINT_GRAPHICS,
        through_sprites_pipeline);
    push.tnl[3] = 0u;
    vkCmdPushConstants(
        command, pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT |
            VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(push), &push);
    vkCmdDraw(command, 6, 1, 24, 0);
    push.tnl[3] = 0x80000000u;
    vkCmdPushConstants(
        command, pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT |
            VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(push), &push);
    vkCmdDraw(command, 6, 1, 30, 0);
    vkCmdEndRenderPass(command);
    VkBufferMemoryBarrier output_barrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER
    };
    output_barrier.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT;
    output_barrier.dstAccessMask =
        VK_ACCESS_HOST_READ_BIT;
    output_barrier.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    output_barrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    output_barrier.buffer = output_buffer.buffer;
    output_barrier.size =
        sizeof(GeTnlValidationVertex) *
            VALIDATION_VERTEX_COUNT;
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0,
        0, NULL, 1, &output_barrier, 0, NULL);
    result = vkEndCommandBuffer(command);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkEndCommandBuffer failed (%d)\n", result);
        goto cleanup;
    }
    VkSubmitInfo submit = {
        VK_STRUCTURE_TYPE_SUBMIT_INFO
    };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    result = vkQueueSubmit(queue, 1, &submit, fence);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkQueueSubmit failed (%d)\n", result);
        goto cleanup;
    }
    result = vkWaitForFences(
        device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkWaitForFences failed (%d)\n", result);
        goto cleanup;
    }

    const GeTnlValidationVertex *outputs =
        (const GeTnlValidationVertex *)output_buffer.map;
    static const char *const component_names[12] = {
        "screen.x", "screen.y", "screen.z", "reciprocal_w",
        "uv.x", "uv.y", "fog", "padding",
        "color.r", "color.g", "color.b", "color.a",
    };
    for (uint32_t i = 0; i < 3; i++) {
        GeTnlReferenceVertex reference;
        GeTnlValidationVertex expected;
        if (!ge_tnl_gpu_reference_transform(
                &gpu_state, &vertices[i], &reference)) {
            fprintf(stderr, "reference transform rejected vertex %u\n", i);
            goto cleanup;
        }
        ge_tnl_validation_pack(&reference, &expected);
        uint32_t component = 0;
        if (!ge_tnl_validation_compare(
                &expected, &outputs[i], &component)) {
            float expected_values[12];
            float actual_values[12];
            memcpy(
                expected_values, &expected,
                sizeof(expected_values));
            memcpy(
                actual_values, &outputs[i],
                sizeof(actual_values));
            fprintf(
                stderr,
                "vertex %u component %s mismatch: "
                "expected %.9g, got %.9g\n",
                i, component_names[component],
                expected_values[component],
                actual_values[component]);
            goto cleanup;
        }
    }
    GeTnlReferenceVertex point_reference;
    GeTnlValidationVertex point_expected;
    if (!ge_tnl_gpu_reference_transform(
            &gpu_state, &vertices[0], &point_reference)) {
        fprintf(stderr, "point reference transform rejected\n");
        goto cleanup;
    }
    ge_tnl_validation_pack(&point_reference, &point_expected);
    static const float point_offsets[6][2] = {
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f},
        {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f},
    };
    for (uint32_t i = 0; i < 6; i++) {
        point_expected.screen_reciprocal_w[0] =
            floorf(point_reference.screen[0]) +
            point_offsets[i][0];
        point_expected.screen_reciprocal_w[1] =
            floorf(point_reference.screen[1]) +
            point_offsets[i][1];
        uint32_t component = 0;
        if (!ge_tnl_validation_compare(
                &point_expected, &outputs[3u + i],
                &component)) {
            float expected_values[12];
            float actual_values[12];
            memcpy(
                expected_values, &point_expected,
                sizeof(expected_values));
            memcpy(
                actual_values, &outputs[3u + i],
                sizeof(actual_values));
            fprintf(
                stderr,
                "point corner %u component %s mismatch: "
                "expected %.9g, got %.9g\n",
                i, component_names[component],
                expected_values[component],
                actual_values[component]);
            goto cleanup;
        }
    }
    GeTnlReferenceVertex sprite_reference[2];
    for (uint32_t i = 0; i < 2; i++) {
        if (!ge_tnl_gpu_reference_transform(
                &gpu_state, &vertices[i + 1u],
                &sprite_reference[i])) {
            fprintf(
                stderr,
                "sprite reference transform rejected control %u\n",
                i);
            goto cleanup;
        }
    }
    float sprite_x[2] = {
        floorf(fminf(
            sprite_reference[0].screen[0],
            sprite_reference[1].screen[0])),
        floorf(fmaxf(
            sprite_reference[0].screen[0],
            sprite_reference[1].screen[0])),
    };
    float sprite_y[2] = {
        floorf(fminf(
            sprite_reference[0].screen[1],
            sprite_reference[1].screen[1])),
        floorf(fmaxf(
            sprite_reference[0].screen[1],
            sprite_reference[1].screen[1])),
    };
    float sprite_uv[2][2];
    for (uint32_t i = 0; i < 2; i++) {
        sprite_uv[i][0] =
            sprite_reference[i].uv[0] /
            sprite_reference[i].reciprocal_w;
        sprite_uv[i][1] =
            sprite_reference[i].uv[1] /
            sprite_reference[i].reciprocal_w;
    }
    float sprite_texel_step[2] = {
        (sprite_uv[1][0] - sprite_uv[0][0]) /
            (sprite_x[1] - sprite_x[0]),
        (sprite_uv[1][1] - sprite_uv[0][1]) /
            (sprite_y[1] - sprite_y[0]),
    };
    for (uint32_t linear = 0; linear < 2; linear++) {
        float low_uv[2] = {
            linear ? sprite_uv[0][0]
                : sprite_uv[0][0] -
                    0.5f * sprite_texel_step[0],
            linear ? sprite_uv[0][1]
                : sprite_uv[0][1] -
                    0.5f * sprite_texel_step[1],
        };
        float high_uv[2] = {
            linear ? sprite_uv[1][0]
                : sprite_uv[1][0] -
                    0.5f * sprite_texel_step[0],
            linear ? sprite_uv[1][1]
                : sprite_uv[1][1] -
                    0.5f * sprite_texel_step[1],
        };
        for (uint32_t i = 0; i < 6; i++) {
            GeTnlValidationVertex expected;
            ge_tnl_validation_pack(
                &sprite_reference[1], &expected);
            uint32_t x_corner =
                point_offsets[i][0] != 0.0f;
            uint32_t y_corner =
                point_offsets[i][1] != 0.0f;
            expected.screen_reciprocal_w[0] =
                sprite_x[x_corner];
            expected.screen_reciprocal_w[1] =
                sprite_y[y_corner];
            expected.screen_reciprocal_w[3] = 1.0f;
            expected.uv_fog[0] =
                x_corner ? high_uv[0] : low_uv[0];
            expected.uv_fog[1] =
                y_corner ? high_uv[1] : low_uv[1];
            uint32_t component = 0;
            const GeTnlValidationVertex *actual =
                &outputs[(linear ? 15u : 9u) + i];
            if (!ge_tnl_validation_compare(
                    &expected, actual, &component)) {
                float expected_values[12];
                float actual_values[12];
                memcpy(
                    expected_values, &expected,
                    sizeof(expected_values));
                memcpy(
                    actual_values, actual,
                    sizeof(actual_values));
                fprintf(
                    stderr,
                    "%s sprite corner %u component %s mismatch: "
                    "expected %.9g, got %.9g\n",
                    linear ? "linear" : "nearest",
                    i, component_names[component],
                    expected_values[component],
                    actual_values[component]);
                goto cleanup;
            }
        }
        for (uint32_t linear = 0; linear < 2; linear++) {
            GeTnlScreenVertex expanded[6];
            const GeTnlVertex *controls =
                &vertices[linear ? 10u : 8u];
            if (!ge_tnl_reference_expand_through_sprite(
                    controls, (int)linear, expanded)) {
                fprintf(
                    stderr,
                    "through sprite reference rejected controls\n");
                goto cleanup;
            }
            for (uint32_t i = 0; i < 6; i++) {
                GeTnlValidationVertex expected;
                memset(&expected, 0, sizeof(expected));
                memcpy(
                    expected.screen_reciprocal_w,
                    expanded[i].position,
                    sizeof(expanded[i].position));
                expected.uv_fog[0] =
                    expanded[i].texcoord[0];
                expected.uv_fog[1] =
                    expanded[i].texcoord[1];
                expected.uv_fog[2] = expanded[i].fog;
                for (uint32_t component = 0;
                     component < 4u; component++) {
                    expected.color[component] =
                        (float)((expanded[i].color >>
                            (component * 8u)) & 0xffu) /
                        255.0f;
                }
                uint32_t component = 0;
                uint32_t output_index =
                    (linear ? 30u : 24u) + i;
                if (!ge_tnl_validation_compare(
                        &expected, &outputs[output_index],
                        &component)) {
                    float expected_values[12];
                    float actual_values[12];
                    memcpy(
                        expected_values, &expected,
                        sizeof(expected_values));
                    memcpy(
                        actual_values, &outputs[output_index],
                        sizeof(actual_values));
                    fprintf(
                        stderr,
                        "through sprite %s corner %u component %s "
                        "mismatch: expected %.9g, got %.9g\n",
                        linear ? "linear" : "nearest",
                        i, component_names[component],
                        expected_values[component],
                        actual_values[component]);
                    goto cleanup;
                }
            }
        }
    }
    if (!run_texture_decode_test(
            physical, device, queue, queue_family)) {
        goto cleanup;
    }
    if (s_validation_messages) {
        fprintf(
            stderr, "%u Vulkan validation message(s)\n",
            s_validation_messages);
        goto cleanup;
    }
    if (!run_preprocess_test(
            physical, device, queue, queue_family)) {
        goto cleanup;
    }

    printf(
        "Vulkan GPU T&L, point/sprite/through-sprite expansion, preprocess, "
        "and direct/indexed/DXT texture decode "
        "readback passed on %s\n",
        device_properties.deviceName);
    status = 0;

cleanup:
    if (device)
        vkDeviceWaitIdle(device);
    if (fence)
        vkDestroyFence(device, fence, NULL);
    if (command_pool)
        vkDestroyCommandPool(device, command_pool, NULL);
    if (pipeline)
        vkDestroyPipeline(device, pipeline, NULL);
    if (points_pipeline)
        vkDestroyPipeline(device, points_pipeline, NULL);
    if (sprites_pipeline)
        vkDestroyPipeline(device, sprites_pipeline, NULL);
    if (through_sprites_pipeline)
        vkDestroyPipeline(
            device, through_sprites_pipeline, NULL);
    if (framebuffer)
        vkDestroyFramebuffer(device, framebuffer, NULL);
    if (render_pass)
        vkDestroyRenderPass(device, render_pass, NULL);
    if (shader)
        vkDestroyShaderModule(device, shader, NULL);
    if (points_shader)
        vkDestroyShaderModule(device, points_shader, NULL);
    if (sprites_shader)
        vkDestroyShaderModule(device, sprites_shader, NULL);
    if (through_sprites_shader)
        vkDestroyShaderModule(
            device, through_sprites_shader, NULL);
    if (pipeline_layout)
        vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    if (descriptor_pool)
        vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (descriptor_layout)
        vkDestroyDescriptorSetLayout(
            device, descriptor_layout, NULL);
    if (empty_descriptor_layout)
        vkDestroyDescriptorSetLayout(
            device, empty_descriptor_layout, NULL);
    if (device) {
        destroy_buffer(device, &output_buffer);
        destroy_buffer(device, &state_buffer);
        destroy_buffer(device, &index_buffer);
        destroy_buffer(device, &vertex_buffer);
        vkDestroyDevice(device, NULL);
    }
    if (messenger && instance) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger =
            (PFN_vkDestroyDebugUtilsMessengerEXT)
                vkGetInstanceProcAddr(
                    instance,
                    "vkDestroyDebugUtilsMessengerEXT");
        if (destroy_messenger)
            destroy_messenger(
                instance, messenger, NULL);
    }
    if (instance)
        vkDestroyInstance(instance, NULL);
    return status;
}

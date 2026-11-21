#ifdef __APPLE__
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "pipeline_cache_vk.h"
#include "smaa_vk.h"

#include "../../../third_party/smaa/Textures/AreaTex.h"
#include "../../../third_party/smaa/Textures/SearchTex.h"

#include <stdio.h>
#include <string.h>

static const uint32_t k_smaa_vert_spv[] =
#include "smaa_vert.inc"
;
static const uint32_t k_smaa_edge_frag_spv[] =
#include "smaa_edge_frag.inc"
;
static const uint32_t k_smaa_blend_frag_spv[] =
#include "smaa_blend_frag.inc"
;
static const uint32_t k_smaa_neighborhood_frag_spv[] =
#include "smaa_neighborhood_frag.inc"
;

_Static_assert(
    sizeof(areaTexBytes) == AREATEX_SIZE,
    "invalid SMAA area texture");
_Static_assert(
    sizeof(searchTexBytes) == SEARCHTEX_SIZE,
    "invalid SMAA search texture");

uint64_t sr_vk_smaa_shader_hash(uint64_t hash) {
    hash = sr_vk_pipeline_cache_hash(
        hash, k_smaa_vert_spv, sizeof(k_smaa_vert_spv));
    hash = sr_vk_pipeline_cache_hash(
        hash, k_smaa_edge_frag_spv,
        sizeof(k_smaa_edge_frag_spv));
    hash = sr_vk_pipeline_cache_hash(
        hash, k_smaa_blend_frag_spv,
        sizeof(k_smaa_blend_frag_spv));
    return sr_vk_pipeline_cache_hash(
        hash, k_smaa_neighborhood_frag_spv,
        sizeof(k_smaa_neighborhood_frag_spv));
}

typedef struct SrVkSmaaImage {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkFramebuffer framebuffer;
    VkImageLayout layout;
} SrVkSmaaImage;

static VkPhysicalDevice s_physical_device;
static VkDevice s_device;
static VkQueue s_queue;
static VkPipelineCache s_pipeline_cache;
static uint32_t s_queue_family;
static uint32_t s_width;
static uint32_t s_height;

static VkRenderPass s_render_pass;
static VkDescriptorSetLayout s_descriptor_layout;
static VkPipelineLayout s_pipeline_layout;
static VkPipeline s_edge_pipeline;
static VkPipeline s_blend_pipeline;
static VkPipeline s_neighborhood_pipeline;
static VkShaderModule s_vertex_shader;
static VkShaderModule s_edge_shader;
static VkShaderModule s_blend_shader;
static VkShaderModule s_neighborhood_shader;
static VkSampler s_sampler;

static SrVkSmaaImage s_input;
static SrVkSmaaImage s_edges;
static SrVkSmaaImage s_blend;
static SrVkSmaaImage s_output;
static SrVkSmaaImage s_area;
static SrVkSmaaImage s_search;
static VkDescriptorPool s_descriptor_pool;
static VkDescriptorSet s_edge_set;
static VkDescriptorSet s_blend_set;
static VkDescriptorSet s_neighborhood_set;

#define SMAA_VK_CHECK(expression) do { \
    VkResult result_ = (expression); \
    if (result_ != VK_SUCCESS) { \
        fprintf( \
            stderr, "sdl3vk: SMAA %s failed: %d\n", \
            #expression, (int)result_); \
        return 0; \
    } \
} while (0)

static uint32_t find_memory_type(
    uint32_t bits, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(
        s_physical_device, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & properties) ==
                properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

static int format_supports(
    VkPhysicalDevice physical_device, VkFormat format,
    VkFormatFeatureFlags required) {
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(
        physical_device, format, &properties);
    return
        (properties.optimalTilingFeatures & required) == required;
}

int sr_vk_smaa_supported(VkPhysicalDevice physical_device) {
    const VkFormatFeatureFlags rgba =
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    const VkFormatFeatureFlags lookup =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (!physical_device ||
        !format_supports(
            physical_device, VK_FORMAT_R8G8B8A8_UNORM, rgba) ||
        !format_supports(
            physical_device,
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !format_supports(
            physical_device, VK_FORMAT_R8G8_UNORM, lookup) ||
        !format_supports(
            physical_device, VK_FORMAT_R8_UNORM, lookup)) {
        return 0;
    }
    return 1;
}

static void transition_image(
    VkCommandBuffer command_buffer, VkImage image,
    VkImageAspectFlags aspect, VkImageLayout before,
    VkImageLayout after, VkAccessFlags source_access,
    VkAccessFlags destination_access,
    VkPipelineStageFlags source_stage,
    VkPipelineStageFlags destination_stage) {
    if (before == after)
        return;
    VkImageMemoryBarrier barrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER
    };
    barrier.srcAccessMask = source_access;
    barrier.dstAccessMask = destination_access;
    barrier.oldLayout = before;
    barrier.newLayout = after;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        command_buffer, source_stage, destination_stage,
        0, 0, NULL, 0, NULL, 1, &barrier);
}

static int create_image(
    uint32_t width, uint32_t height, VkFormat format,
    VkImageUsageFlags usage, SrVkSmaaImage *image) {
    VkImageCreateInfo image_info = {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO
    };
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    SMAA_VK_CHECK(vkCreateImage(
        s_device, &image_info, NULL, &image->image));

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(
        s_device, image->image, &requirements);
    uint32_t memory_type = find_memory_type(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == UINT32_MAX) {
        fprintf(stderr, "sdl3vk: SMAA has no device-local memory\n");
        return 0;
    }
    VkMemoryAllocateInfo allocation = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
    };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    SMAA_VK_CHECK(vkAllocateMemory(
        s_device, &allocation, NULL, &image->memory));
    SMAA_VK_CHECK(vkBindImageMemory(
        s_device, image->image, image->memory, 0));

    VkImageViewCreateInfo view_info = {
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO
    };
    view_info.image = image->image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    SMAA_VK_CHECK(vkCreateImageView(
        s_device, &view_info, NULL, &image->view));
    image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return 1;
}

static void destroy_image(SrVkSmaaImage *image) {
    if (image->framebuffer)
        vkDestroyFramebuffer(
            s_device, image->framebuffer, NULL);
    if (image->view)
        vkDestroyImageView(s_device, image->view, NULL);
    if (image->image)
        vkDestroyImage(s_device, image->image, NULL);
    if (image->memory)
        vkFreeMemory(s_device, image->memory, NULL);
    memset(image, 0, sizeof(*image));
}

static int create_shader(
    const uint32_t *code, size_t size,
    VkShaderModule *shader) {
    VkShaderModuleCreateInfo info = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
    };
    info.codeSize = size;
    info.pCode = code;
    SMAA_VK_CHECK(vkCreateShaderModule(
        s_device, &info, NULL, shader));
    return 1;
}

static int create_pipeline(
    VkShaderModule fragment_shader, VkPipeline *pipeline) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = s_vertex_shader;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    input_assembly.topology =
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
    };
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment = {0};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    const VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamic = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
    };
    dynamic.dynamicStateCount =
        (uint32_t)(sizeof(dynamic_states) /
                   sizeof(dynamic_states[0]));
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo info = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO
    };
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = s_pipeline_layout;
    info.renderPass = s_render_pass;
    info.subpass = 0;
    SMAA_VK_CHECK(vkCreateGraphicsPipelines(
        s_device, s_pipeline_cache, 1, &info, NULL, pipeline));
    return 1;
}

static int create_static_resources(void) {
    VkAttachmentDescription attachment = {0};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference color = {
        0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };
    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint =
        VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    VkSubpassDependency dependencies[2] = {0};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask =
        VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo render_pass = {
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO
    };
    render_pass.attachmentCount = 1;
    render_pass.pAttachments = &attachment;
    render_pass.subpassCount = 1;
    render_pass.pSubpasses = &subpass;
    render_pass.dependencyCount = 2;
    render_pass.pDependencies = dependencies;
    SMAA_VK_CHECK(vkCreateRenderPass(
        s_device, &render_pass, NULL, &s_render_pass));

    VkDescriptorSetLayoutBinding bindings[3] = {0};
    for (uint32_t i = 0; i < 3; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo descriptor_layout = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    descriptor_layout.bindingCount = 3;
    descriptor_layout.pBindings = bindings;
    SMAA_VK_CHECK(vkCreateDescriptorSetLayout(
        s_device, &descriptor_layout, NULL,
        &s_descriptor_layout));

    VkPushConstantRange push_constant = {
        VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 4u
    };
    VkPipelineLayoutCreateInfo pipeline_layout = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &s_descriptor_layout;
    pipeline_layout.pushConstantRangeCount = 1;
    pipeline_layout.pPushConstantRanges = &push_constant;
    SMAA_VK_CHECK(vkCreatePipelineLayout(
        s_device, &pipeline_layout, NULL,
        &s_pipeline_layout));

    if (!create_shader(
            k_smaa_vert_spv, sizeof(k_smaa_vert_spv),
            &s_vertex_shader) ||
        !create_shader(
            k_smaa_edge_frag_spv,
            sizeof(k_smaa_edge_frag_spv), &s_edge_shader) ||
        !create_shader(
            k_smaa_blend_frag_spv,
            sizeof(k_smaa_blend_frag_spv), &s_blend_shader) ||
        !create_shader(
            k_smaa_neighborhood_frag_spv,
            sizeof(k_smaa_neighborhood_frag_spv),
            &s_neighborhood_shader) ||
        !create_pipeline(s_edge_shader, &s_edge_pipeline) ||
        !create_pipeline(s_blend_shader, &s_blend_pipeline) ||
        !create_pipeline(
            s_neighborhood_shader,
            &s_neighborhood_pipeline)) {
        return 0;
    }

    VkSamplerCreateInfo sampler = {
        VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO
    };
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 0.0f;
    SMAA_VK_CHECK(vkCreateSampler(
        s_device, &sampler, NULL, &s_sampler));
    return 1;
}

static int upload_lookup_textures(void) {
    if (!create_image(
            AREATEX_WIDTH, AREATEX_HEIGHT,
            VK_FORMAT_R8G8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            &s_area) ||
        !create_image(
            SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT,
            VK_FORMAT_R8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            &s_search)) {
        return 0;
    }

    VkDeviceSize area_size = sizeof(areaTexBytes);
    VkDeviceSize search_offset = (area_size + 3u) & ~3u;
    VkDeviceSize staging_size =
        search_offset + sizeof(searchTexBytes);
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    int ok = 0;

    VkBufferCreateInfo buffer_info = {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO
    };
    buffer_info.size = staging_size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkResult result = vkCreateBuffer(
        s_device, &buffer_info, NULL, &staging);
    if (result != VK_SUCCESS)
        goto cleanup;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(
        s_device, staging, &requirements);
    uint32_t memory_type = find_memory_type(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory_type == UINT32_MAX)
        goto cleanup;
    VkMemoryAllocateInfo allocation = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
    };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    result = vkAllocateMemory(
        s_device, &allocation, NULL, &staging_memory);
    if (result != VK_SUCCESS)
        goto cleanup;
    result = vkBindBufferMemory(
        s_device, staging, staging_memory, 0);
    if (result != VK_SUCCESS)
        goto cleanup;
    void *mapped = NULL;
    result = vkMapMemory(
        s_device, staging_memory, 0, staging_size, 0, &mapped);
    if (result != VK_SUCCESS)
        goto cleanup;
    memcpy(mapped, areaTexBytes, sizeof(areaTexBytes));
    memcpy(
        (unsigned char *)mapped + search_offset,
        searchTexBytes, sizeof(searchTexBytes));
    vkUnmapMemory(s_device, staging_memory);

    VkCommandPoolCreateInfo pool_info = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO
    };
    pool_info.queueFamilyIndex = s_queue_family;
    result = vkCreateCommandPool(
        s_device, &pool_info, NULL, &command_pool);
    if (result != VK_SUCCESS)
        goto cleanup;
    VkCommandBuffer command_buffer;
    VkCommandBufferAllocateInfo command_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
    };
    command_info.commandPool = command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(
        s_device, &command_info, &command_buffer);
    if (result != VK_SUCCESS)
        goto cleanup;
    VkCommandBufferBeginInfo begin = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command_buffer, &begin);
    if (result != VK_SUCCESS)
        goto cleanup;

    transition_image(
        command_buffer, s_area.image,
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    transition_image(
        command_buffer, s_search.image,
        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copies[2] = {0};
    copies[0].imageSubresource.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;
    copies[0].imageSubresource.layerCount = 1;
    copies[0].imageExtent.width = AREATEX_WIDTH;
    copies[0].imageExtent.height = AREATEX_HEIGHT;
    copies[0].imageExtent.depth = 1;
    copies[1].bufferOffset = search_offset;
    copies[1].imageSubresource.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;
    copies[1].imageSubresource.layerCount = 1;
    copies[1].imageExtent.width = SEARCHTEX_WIDTH;
    copies[1].imageExtent.height = SEARCHTEX_HEIGHT;
    copies[1].imageExtent.depth = 1;
    vkCmdCopyBufferToImage(
        command_buffer, staging, s_area.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copies[0]);
    vkCmdCopyBufferToImage(
        command_buffer, staging, s_search.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copies[1]);
    transition_image(
        command_buffer, s_area.image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    transition_image(
        command_buffer, s_search.image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS)
        goto cleanup;
    VkSubmitInfo submit = {
        VK_STRUCTURE_TYPE_SUBMIT_INFO
    };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command_buffer;
    VkFenceCreateInfo fence_info = {
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    };
    result = vkCreateFence(
        s_device, &fence_info, NULL, &fence);
    if (result != VK_SUCCESS)
        goto cleanup;
    result = vkQueueSubmit(s_queue, 1, &submit, fence);
    if (result == VK_SUCCESS)
        result = vkWaitForFences(
            s_device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS)
        goto cleanup;
    s_area.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    s_search.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ok = 1;

cleanup:
    if (!ok)
        fprintf(
            stderr,
            "sdl3vk: SMAA lookup texture upload failed: %d\n",
            (int)result);
    if (fence)
        vkDestroyFence(s_device, fence, NULL);
    if (command_pool)
        vkDestroyCommandPool(s_device, command_pool, NULL);
    if (staging)
        vkDestroyBuffer(s_device, staging, NULL);
    if (staging_memory)
        vkFreeMemory(s_device, staging_memory, NULL);
    return ok;
}

static int create_framebuffer(SrVkSmaaImage *image) {
    VkFramebufferCreateInfo info = {
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO
    };
    info.renderPass = s_render_pass;
    info.attachmentCount = 1;
    info.pAttachments = &image->view;
    info.width = s_width;
    info.height = s_height;
    info.layers = 1;
    SMAA_VK_CHECK(vkCreateFramebuffer(
        s_device, &info, NULL, &image->framebuffer));
    return 1;
}

static void update_set(
    VkDescriptorSet set, VkImageView first,
    VkImageView second, VkImageView third) {
    const VkImageView views[3] = {first, second, third};
    VkDescriptorImageInfo images[3] = {0};
    VkWriteDescriptorSet writes[3] = {0};
    for (uint32_t i = 0; i < 3; i++) {
        images[i].sampler = s_sampler;
        images[i].imageView = views[i];
        images[i].imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(
        s_device, 3, writes, 0, NULL);
}

static void destroy_size_resources(void) {
    if (s_descriptor_pool)
        vkDestroyDescriptorPool(
            s_device, s_descriptor_pool, NULL);
    s_descriptor_pool = VK_NULL_HANDLE;
    s_edge_set = VK_NULL_HANDLE;
    s_blend_set = VK_NULL_HANDLE;
    s_neighborhood_set = VK_NULL_HANDLE;
    destroy_image(&s_input);
    destroy_image(&s_edges);
    destroy_image(&s_blend);
    destroy_image(&s_output);
    s_width = 0;
    s_height = 0;
}

static int create_size_resources(
    uint32_t width, uint32_t height) {
    s_width = width;
    s_height = height;
    VkImageUsageFlags target_usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!create_image(
            width, height, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            &s_input) ||
        !create_image(
            width, height, VK_FORMAT_R8G8B8A8_UNORM,
            target_usage, &s_edges) ||
        !create_image(
            width, height, VK_FORMAT_R8G8B8A8_UNORM,
            target_usage, &s_blend) ||
        !create_image(
            width, height, VK_FORMAT_R8G8B8A8_UNORM,
            target_usage, &s_output) ||
        !create_framebuffer(&s_edges) ||
        !create_framebuffer(&s_blend) ||
        !create_framebuffer(&s_output)) {
        return 0;
    }

    VkDescriptorPoolSize pool_size = {
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 9
    };
    VkDescriptorPoolCreateInfo pool_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };
    pool_info.maxSets = 3;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    SMAA_VK_CHECK(vkCreateDescriptorPool(
        s_device, &pool_info, NULL, &s_descriptor_pool));
    VkDescriptorSetLayout layouts[3] = {
        s_descriptor_layout,
        s_descriptor_layout,
        s_descriptor_layout,
    };
    VkDescriptorSet sets[3];
    VkDescriptorSetAllocateInfo allocation = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    allocation.descriptorPool = s_descriptor_pool;
    allocation.descriptorSetCount = 3;
    allocation.pSetLayouts = layouts;
    SMAA_VK_CHECK(vkAllocateDescriptorSets(
        s_device, &allocation, sets));
    s_edge_set = sets[0];
    s_blend_set = sets[1];
    s_neighborhood_set = sets[2];
    update_set(
        s_edge_set, s_input.view, s_area.view, s_search.view);
    update_set(
        s_blend_set, s_edges.view, s_area.view, s_search.view);
    update_set(
        s_neighborhood_set,
        s_input.view, s_blend.view, s_search.view);
    return 1;
}

int sr_vk_smaa_prepare(
    VkPhysicalDevice physical_device, VkDevice device,
    VkQueue queue, uint32_t queue_family,
    VkPipelineCache pipeline_cache,
    uint32_t width, uint32_t height) {
    if (!physical_device || !device || !queue || !width || !height) {
        fprintf(stderr, "sdl3vk: invalid SMAA configuration\n");
        return 0;
    }
    if (s_device && s_device != device)
        sr_vk_smaa_shutdown();
    if (!s_device) {
        if (!sr_vk_smaa_supported(physical_device)) {
            fprintf(
                stderr,
                "sdl3vk: SMAA required Vulkan image formats are "
                "unsupported\n");
            return 0;
        }
        s_physical_device = physical_device;
        s_device = device;
        s_queue = queue;
        s_queue_family = queue_family;
        s_pipeline_cache = pipeline_cache;
        if (!create_static_resources() ||
            !upload_lookup_textures()) {
            sr_vk_smaa_shutdown();
            return 0;
        }
    }
    if (s_width == width && s_height == height)
        return 1;
    if (s_width || s_height) {
        VkResult idle = vkDeviceWaitIdle(s_device);
        if (idle != VK_SUCCESS) {
            fprintf(
                stderr,
                "sdl3vk: SMAA resize wait failed: %d\n",
                (int)idle);
            return 0;
        }
        destroy_size_resources();
    }
    if (!create_size_resources(width, height)) {
        fprintf(
            stderr,
            "sdl3vk: cannot create %ux%u SMAA targets\n",
            width, height);
        sr_vk_smaa_shutdown();
        return 0;
    }
    fprintf(
        stderr, "sdl3vk: SMAA 1x High active at %ux%u\n",
        width, height);
    return 1;
}

static void record_pass(
    VkCommandBuffer command_buffer, VkFramebuffer framebuffer,
    VkPipeline pipeline, VkDescriptorSet descriptor) {
    VkClearValue clear = {{{0.0f, 0.0f, 0.0f, 0.0f}}};
    VkRenderPassBeginInfo begin = {
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO
    };
    begin.renderPass = s_render_pass;
    begin.framebuffer = framebuffer;
    begin.renderArea.extent.width = s_width;
    begin.renderArea.extent.height = s_height;
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    vkCmdBeginRenderPass(
        command_buffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport = {
        0.0f, 0.0f, (float)s_width, (float)s_height,
        0.0f, 1.0f
    };
    VkRect2D scissor = {{0, 0}, {s_width, s_height}};
    const float metrics[4] = {
        1.0f / (float)s_width,
        1.0f / (float)s_height,
        (float)s_width,
        (float)s_height,
    };
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(
        command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline);
    vkCmdBindDescriptorSets(
        command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        s_pipeline_layout, 0, 1, &descriptor, 0, NULL);
    vkCmdPushConstants(
        command_buffer, s_pipeline_layout,
        VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        sizeof(metrics), metrics);
    vkCmdDraw(command_buffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(command_buffer);
}

VkImage sr_vk_smaa_record(
    VkCommandBuffer command_buffer, VkImage source,
    uint32_t source_width, uint32_t source_height) {
    if (!command_buffer || !source || !source_width ||
        !source_height || !s_width || !s_height) {
        return VK_NULL_HANDLE;
    }
    transition_image(
        command_buffer, s_input.image,
        VK_IMAGE_ASPECT_COLOR_BIT, s_input.layout,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        s_input.layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0 : VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        s_input.layout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
            : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    s_input.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkImageBlit blit = {0};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = (int32_t)source_width;
    blit.srcOffsets[1].y = (int32_t)source_height;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1].x = (int32_t)s_width;
    blit.dstOffsets[1].y = (int32_t)s_height;
    blit.dstOffsets[1].z = 1;
    vkCmdBlitImage(
        command_buffer, source,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        s_input.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_NEAREST);
    transition_image(
        command_buffer, s_input.image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    s_input.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    record_pass(
        command_buffer, s_edges.framebuffer,
        s_edge_pipeline, s_edge_set);
    s_edges.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    record_pass(
        command_buffer, s_blend.framebuffer,
        s_blend_pipeline, s_blend_set);
    s_blend.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    record_pass(
        command_buffer, s_output.framebuffer,
        s_neighborhood_pipeline, s_neighborhood_set);
    s_output.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    transition_image(
        command_buffer, s_output.image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT);
    s_output.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return s_output.image;
}

void sr_vk_smaa_shutdown(void) {
    if (!s_device)
        return;
    destroy_size_resources();
    destroy_image(&s_area);
    destroy_image(&s_search);
    if (s_sampler)
        vkDestroySampler(s_device, s_sampler, NULL);
    if (s_edge_pipeline)
        vkDestroyPipeline(s_device, s_edge_pipeline, NULL);
    if (s_blend_pipeline)
        vkDestroyPipeline(s_device, s_blend_pipeline, NULL);
    if (s_neighborhood_pipeline)
        vkDestroyPipeline(
            s_device, s_neighborhood_pipeline, NULL);
    if (s_vertex_shader)
        vkDestroyShaderModule(s_device, s_vertex_shader, NULL);
    if (s_edge_shader)
        vkDestroyShaderModule(s_device, s_edge_shader, NULL);
    if (s_blend_shader)
        vkDestroyShaderModule(s_device, s_blend_shader, NULL);
    if (s_neighborhood_shader)
        vkDestroyShaderModule(
            s_device, s_neighborhood_shader, NULL);
    if (s_pipeline_layout)
        vkDestroyPipelineLayout(
            s_device, s_pipeline_layout, NULL);
    if (s_descriptor_layout)
        vkDestroyDescriptorSetLayout(
            s_device, s_descriptor_layout, NULL);
    if (s_render_pass)
        vkDestroyRenderPass(s_device, s_render_pass, NULL);
    s_physical_device = VK_NULL_HANDLE;
    s_device = VK_NULL_HANDLE;
    s_queue = VK_NULL_HANDLE;
    s_pipeline_cache = VK_NULL_HANDLE;
    s_queue_family = 0;
    s_render_pass = VK_NULL_HANDLE;
    s_descriptor_layout = VK_NULL_HANDLE;
    s_pipeline_layout = VK_NULL_HANDLE;
    s_edge_pipeline = VK_NULL_HANDLE;
    s_blend_pipeline = VK_NULL_HANDLE;
    s_neighborhood_pipeline = VK_NULL_HANDLE;
    s_vertex_shader = VK_NULL_HANDLE;
    s_edge_shader = VK_NULL_HANDLE;
    s_blend_shader = VK_NULL_HANDLE;
    s_neighborhood_shader = VK_NULL_HANDLE;
    s_sampler = VK_NULL_HANDLE;
}

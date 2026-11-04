#include "pipeline_cache_vk.h"

#include "../settings_path.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIPELINE_CACHE_MAGIC UINT32_C(0x43505253)
#define PIPELINE_CACHE_FORMAT_VERSION 1u
#define PIPELINE_CACHE_MAX_SIZE (64u * 1024u * 1024u)

typedef struct PipelineCacheHeader {
    uint32_t magic;
    uint32_t format_version;
    uint32_t header_size;
    uint32_t schema_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t driver_version;
    uint32_t api_version;
    uint8_t uuid[VK_UUID_SIZE];
    uint64_t shader_hash;
    uint64_t data_size;
} PipelineCacheHeader;

uint64_t sr_vk_pipeline_cache_hash(
    uint64_t hash, const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void fill_header(
    const SrVkPipelineCache *cache,
    PipelineCacheHeader *header,
    size_t data_size) {
    memset(header, 0, sizeof(*header));
    header->magic = PIPELINE_CACHE_MAGIC;
    header->format_version = PIPELINE_CACHE_FORMAT_VERSION;
    header->header_size = sizeof(*header);
    header->schema_version = cache->schema_version;
    header->vendor_id = cache->properties.vendorID;
    header->device_id = cache->properties.deviceID;
    header->driver_version = cache->properties.driverVersion;
    header->api_version = cache->properties.apiVersion;
    memcpy(
        header->uuid, cache->properties.pipelineCacheUUID,
        sizeof(header->uuid));
    header->shader_hash = cache->shader_hash;
    header->data_size = data_size;
}

static int header_matches(
    const SrVkPipelineCache *cache,
    const PipelineCacheHeader *header) {
    return
        header->magic == PIPELINE_CACHE_MAGIC &&
        header->format_version == PIPELINE_CACHE_FORMAT_VERSION &&
        header->header_size == sizeof(*header) &&
        header->schema_version == cache->schema_version &&
        header->vendor_id == cache->properties.vendorID &&
        header->device_id == cache->properties.deviceID &&
        header->driver_version == cache->properties.driverVersion &&
        header->api_version == cache->properties.apiVersion &&
        memcmp(
            header->uuid, cache->properties.pipelineCacheUUID,
            sizeof(header->uuid)) == 0 &&
        header->shader_hash == cache->shader_hash &&
        header->data_size > 0 &&
        header->data_size <= PIPELINE_CACHE_MAX_SIZE;
}

static void *load_cache_data(
    const SrVkPipelineCache *cache, size_t *data_size) {
    *data_size = 0;
    errno = 0;
    FILE *file = fopen(cache->path, "rb");
    if (!file) {
        if (errno == ENOENT) {
            fprintf(stderr, "sdl3vk: pipeline cache cold (no saved cache)\n");
        } else {
            fprintf(
                stderr, "sdl3vk: cannot open pipeline cache %s: %s\n",
                cache->path, strerror(errno));
        }
        return NULL;
    }

    PipelineCacheHeader header;
    if (fread(&header, 1, sizeof(header), file) != sizeof(header) ||
        !header_matches(cache, &header)) {
        fprintf(
            stderr,
            "sdl3vk: ignoring incompatible or corrupt pipeline cache %s\n",
            cache->path);
        fclose(file);
        return NULL;
    }

    void *data = malloc((size_t)header.data_size);
    if (!data) {
        fprintf(
            stderr,
            "sdl3vk: cannot allocate %llu-byte pipeline cache\n",
            (unsigned long long)header.data_size);
        fclose(file);
        return NULL;
    }
    size_t read_size =
        fread(data, 1, (size_t)header.data_size, file);
    int trailing = fgetc(file);
    int read_error = ferror(file);
    if (fclose(file) != 0)
        read_error = 1;
    if (read_size != (size_t)header.data_size ||
        trailing != EOF || read_error) {
        fprintf(
            stderr, "sdl3vk: pipeline cache payload is corrupt: %s\n",
            cache->path);
        free(data);
        return NULL;
    }
    *data_size = (size_t)header.data_size;
    return data;
}

static int create_empty_cache(SrVkPipelineCache *cache) {
    VkPipelineCacheCreateInfo create_info = {
        VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO
    };
    VkResult result = vkCreatePipelineCache(
        cache->device, &create_info, NULL, &cache->handle);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr, "sdl3vk: vkCreatePipelineCache failed: %d\n",
            (int)result);
        cache->handle = VK_NULL_HANDLE;
        return 0;
    }
    return 1;
}

int sr_vk_pipeline_cache_init(
    SrVkPipelineCache *cache,
    VkPhysicalDevice physical_device,
    VkDevice device,
    const char *relative_path,
    uint32_t schema_version,
    uint64_t shader_hash) {
    if (!cache || !physical_device || !device ||
        !relative_path || !relative_path[0]) {
        fprintf(stderr, "sdl3vk: invalid pipeline cache configuration\n");
        return 0;
    }

    memset(cache, 0, sizeof(*cache));
    cache->device = device;
    cache->schema_version = schema_version;
    cache->shader_hash = shader_hash;
    vkGetPhysicalDeviceProperties(
        physical_device, &cache->properties);

    const char *enabled = getenv("SR_VULKAN_PIPELINE_CACHE");
    if (enabled && enabled[0] && strcmp(enabled, "0") == 0) {
        fprintf(stderr, "sdl3vk: persistent pipeline cache disabled\n");
        return create_empty_cache(cache);
    }

    char error[256];
    if (!sr_settings_path(
            relative_path, cache->path, sizeof(cache->path),
            error, sizeof(error))) {
        fprintf(
            stderr,
            "sdl3vk: pipeline cache persistence unavailable: %s\n",
            error[0] ? error : "cannot resolve cache path");
        cache->path[0] = 0;
        return create_empty_cache(cache);
    }
    cache->persistent = 1;

    size_t initial_size = 0;
    void *initial_data = load_cache_data(cache, &initial_size);
    VkPipelineCacheCreateInfo create_info = {
        VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO
    };
    create_info.initialDataSize = initial_size;
    create_info.pInitialData = initial_data;
    VkResult result = vkCreatePipelineCache(
        device, &create_info, NULL, &cache->handle);
    free(initial_data);
    if (result != VK_SUCCESS && initial_size) {
        fprintf(
            stderr,
            "sdl3vk: Vulkan rejected saved pipeline cache (%d); "
            "starting empty\n",
            (int)result);
        cache->handle = VK_NULL_HANDLE;
        return create_empty_cache(cache);
    }
    if (result != VK_SUCCESS) {
        fprintf(
            stderr, "sdl3vk: vkCreatePipelineCache failed: %d\n",
            (int)result);
        cache->handle = VK_NULL_HANDLE;
        return 0;
    }
    if (initial_size) {
        fprintf(
            stderr, "sdl3vk: loaded %zu-byte pipeline cache\n",
            initial_size);
    }
    return 1;
}

static int save_cache(const SrVkPipelineCache *cache) {
    size_t data_size = 0;
    VkResult result = vkGetPipelineCacheData(
        cache->device, cache->handle, &data_size, NULL);
    if (result != VK_SUCCESS || !data_size ||
        data_size > PIPELINE_CACHE_MAX_SIZE) {
        fprintf(
            stderr,
            "sdl3vk: cannot size pipeline cache (result %d, size %zu)\n",
            (int)result, data_size);
        return 0;
    }

    void *data = malloc(data_size);
    if (!data) {
        fprintf(
            stderr, "sdl3vk: cannot allocate %zu-byte pipeline cache\n",
            data_size);
        return 0;
    }
    result = vkGetPipelineCacheData(
        cache->device, cache->handle, &data_size, data);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr, "sdl3vk: cannot read pipeline cache data: %d\n",
            (int)result);
        free(data);
        return 0;
    }
    char temporary[SR_PATH_MAX];
    int length = snprintf(
        temporary, sizeof(temporary), "%s.tmp", cache->path);
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        fprintf(stderr, "sdl3vk: pipeline cache temporary path is too long\n");
        free(data);
        return 0;
    }
    FILE *file = fopen(temporary, "wb");
    if (!file) {
        fprintf(
            stderr,
            "sdl3vk: cannot create temporary pipeline cache %s: %s\n",
            temporary, strerror(errno));
        free(data);
        return 0;
    }

    PipelineCacheHeader header;
    fill_header(cache, &header, data_size);
    int ok =
        fwrite(&header, 1, sizeof(header), file) == sizeof(header) &&
        fwrite(data, 1, data_size, file) == data_size &&
        sr_file_commit(file);
    if (fclose(file) != 0)
        ok = 0;
    free(data);
    if (!ok) {
        fprintf(
            stderr, "sdl3vk: cannot write pipeline cache %s: %s\n",
            temporary, strerror(errno));
        sr_file_remove(temporary);
        return 0;
    }
    if (!sr_file_replace(temporary, cache->path)) {
        fprintf(
            stderr, "sdl3vk: cannot publish pipeline cache %s: %s\n",
            cache->path, strerror(errno));
        sr_file_remove(temporary);
        return 0;
    }
    fprintf(
        stderr, "sdl3vk: saved %zu-byte pipeline cache\n",
        data_size);
    return 1;
}

void sr_vk_pipeline_cache_shutdown(SrVkPipelineCache *cache) {
    if (!cache || !cache->device)
        return;
    if (cache->handle && cache->persistent && cache->path[0])
        save_cache(cache);
    if (cache->handle)
        vkDestroyPipelineCache(
            cache->device, cache->handle, NULL);
    memset(cache, 0, sizeof(*cache));
}

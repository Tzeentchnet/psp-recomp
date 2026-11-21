/* SDL3 + Vulkan window, input, device, and presentation backend.
 *
 * Software frames use a persistently mapped upload buffer. The Vulkan GE backend can
 * instead present its persistent target directly. Both paths blit the visible 480x272
 * region into an aspect-correct swapchain rectangle. Input (keyboard + gamepads) comes
 * from SDL3 and maps to the same PSP sceCtrl bits as the other presenters.
 *
 * Deliberately single-frame-in-flight (fence after submit): presentation cost is trivial
 * and it keeps swapchain recreation and shutdown simple while the architecture settles.
 *
 * AI disclosure: this renderer is an original implementation (it does not reuse PPSSPP's
 * GPU) written with substantial assistance from an LLM (Anthropic Claude). See CREDITS.md.
 * GPLv2+: it consumes ge.c, whose GE semantics are derived from PPSSPP. */

#ifdef __APPLE__
#define VK_ENABLE_BETA_EXTENSIONS
#endif

#include "sdl3vk.h"
#include "ge_gpu.h"
#include "pipeline_cache_vk.h"
#include "present_cache.h"
#include "present_layout.h"
#include "../audio.h"
#include "../host_lifecycle.h"
#include "../input_sdl.h"
#include "../graphics_config.h"
#include "../platform.h"
#include "../recomp.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include "smaa_vk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSP_W 480
#define PSP_H 272
#define VULKAN_PIPELINE_CACHE_SCHEMA 1u

static SDL_Window      *s_win;
static SDL_Gamepad     *s_pad;
static VkInstance       s_inst;
static VkSurfaceKHR     s_surf;
static VkPhysicalDevice s_pdev;
static VkDevice         s_dev;
static uint32_t         s_qfam;
static VkQueue          s_queue;
static VkCommandPool    s_pool;
static VkCommandBuffer  s_cmd;
static VkFence          s_fence;
static VkSemaphore      s_sem_acq, s_sem_done[8];
static VkDebugUtilsMessengerEXT s_debug_messenger;
static SrVkPipelineCache s_pipeline_cache;

static VkSwapchainKHR s_swap;
static VkFormat       s_swap_fmt;
static VkExtent2D     s_swap_ext;
static uint32_t       s_swap_n;
static VkImage        s_swap_img[8];

static VkBuffer        s_staging;
static VkDeviceMemory  s_staging_mem;
static void           *s_staging_map;
static VkImage         s_fbimg;
static VkDeviceMemory  s_fbimg_mem;
static VkQueryPool     s_timestamp_pool;
static uint32_t        s_timestamp_bits;
static float           s_timestamp_period;
static uint64_t        s_gpu_flip;
static uint64_t        s_last_gpu_flip;
static double          s_last_gpu_ms = -1.0;

enum { CAPTURE_PATH_COUNT = 2 };
static char             s_capture_path[CAPTURE_PATH_COUNT][SR_PATH_MAX];
static int              s_capture_count;
static VkBuffer         s_capture_buffer;
static VkDeviceMemory   s_capture_memory;
static void            *s_capture_map;
static VkDeviceSize     s_capture_size;
static uint64_t         s_host_upload_bytes;
static uint64_t         s_host_readback_bytes;

static uint32_t s_buttons;
static uint8_t  s_lx = 128, s_ly = 128;
static int      s_pad_present;
static SrVkPresentCache s_present_cache;
static uint64_t s_present_reused;
static int s_present_repeats;
static int s_smaa_enabled;
static uint32_t s_smaa_width;
static uint32_t s_smaa_height;
static SrHostLifecycle s_lifecycle;
static SDL_AtomicInt s_system_suspended;
static SDL_AtomicInt s_system_suspend_requested;
static SDL_AtomicInt s_system_resume_requested;
static SDL_AtomicInt s_terminate_requested;
static SDL_AtomicInt s_low_memory_requested;
static int s_event_watch_installed;

#define VK_TRY(expr) do { VkResult vr_ = (expr); if (vr_ != VK_SUCCESS) { \
    fprintf(stderr, "sdl3vk: %s failed: %d\n", #expr, (int)vr_); return 0; } } while (0)

static VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void *user) {
    const char *level =
        severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" :
        severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ? "warning" :
        "info";
    (void)type;
    (void)user;
    fprintf(
        stderr, "sdl3vk validation %s: %s\n",
        level, data && data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

static int validation_layer_available(void) {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, NULL) != VK_SUCCESS)
        return 0;
    VkLayerProperties *properties =
        (VkLayerProperties *)malloc(count * sizeof(*properties));
    if (!properties) return 0;
    int found = 0;
    if (vkEnumerateInstanceLayerProperties(&count, properties) ==
        VK_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(
                    properties[i].layerName,
                    "VK_LAYER_KHRONOS_validation") == 0) {
                found = 1;
                break;
            }
        }
    }
    free(properties);
    return found;
}

static int instance_extension_available(const char *name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) !=
        VK_SUCCESS) {
        return 0;
    }
    VkExtensionProperties *properties =
        (VkExtensionProperties *)malloc(count * sizeof(*properties));
    if (!properties) return 0;
    int found = 0;
    if (vkEnumerateInstanceExtensionProperties(
            NULL, &count, properties) == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(properties[i].extensionName, name) == 0) {
                found = 1;
                break;
            }
        }
    }
    free(properties);
    return found;
}

static int device_extension_available(
    VkPhysicalDevice device, const char *name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(
            device, NULL, &count, NULL) != VK_SUCCESS) {
        return 0;
    }
    VkExtensionProperties *properties =
        (VkExtensionProperties *)malloc(count * sizeof(*properties));
    if (!properties) return 0;
    int found = 0;
    if (vkEnumerateDeviceExtensionProperties(
            device, NULL, &count, properties) == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(properties[i].extensionName, name) == 0) {
                found = 1;
                break;
            }
        }
    }
    free(properties);
    return found;
}

static VkDebugUtilsMessengerCreateInfoEXT validation_create_info(void) {
    VkDebugUtilsMessengerCreateInfoEXT info = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT
    };
    info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = validation_message;
    return info;
}

static uint32_t find_mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(s_pdev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static void destroy_capture_buffer(void) {
    if (s_capture_map)
        vkUnmapMemory(s_dev, s_capture_memory);
    s_capture_map = NULL;
    if (s_capture_buffer)
        vkDestroyBuffer(s_dev, s_capture_buffer, NULL);
    s_capture_buffer = VK_NULL_HANDLE;
    if (s_capture_memory)
        vkFreeMemory(s_dev, s_capture_memory, NULL);
    s_capture_memory = VK_NULL_HANDLE;
    s_capture_size = 0;
}

static int ensure_capture_buffer(VkDeviceSize size) {
    if (s_capture_buffer && s_capture_size >= size)
        return 1;

    destroy_capture_buffer();
    VkBufferCreateInfo buffer_info = {
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO
    };
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkResult result = vkCreateBuffer(
        s_dev, &buffer_info, NULL, &s_capture_buffer);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr, "sdl3vk: vkCreateBuffer(capture) failed: %d\n",
            (int)result);
        return 0;
    }

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(
        s_dev, s_capture_buffer, &requirements);
    uint32_t memory_type = find_mem_type(
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory_type == UINT32_MAX) {
        fprintf(stderr, "sdl3vk: no host-visible capture memory type\n");
        destroy_capture_buffer();
        return 0;
    }

    VkMemoryAllocateInfo allocation = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
    };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    result = vkAllocateMemory(
        s_dev, &allocation, NULL, &s_capture_memory);
    if (result == VK_SUCCESS)
        result = vkBindBufferMemory(
            s_dev, s_capture_buffer, s_capture_memory, 0);
    if (result == VK_SUCCESS)
        result = vkMapMemory(
            s_dev, s_capture_memory, 0, VK_WHOLE_SIZE, 0,
            &s_capture_map);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr, "sdl3vk: preparing capture buffer failed: %d\n",
            (int)result);
        destroy_capture_buffer();
        return 0;
    }
    s_capture_size = requirements.size;
    return 1;
}

static int write_capture(
    const char *path, uint32_t source_width,
    const SrVkCaptureLayout *layout, int bgra) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "sdl3vk: cannot open capture %s\n", path);
        return 0;
    }
    if (fprintf(
            file, "P6\n%u %u\n255\n",
            layout->width, layout->height) < 0) {
        fprintf(stderr, "sdl3vk: cannot write capture header %s\n", path);
        fclose(file);
        return 0;
    }

    size_t row_size = (size_t)layout->width * 3u;
    uint8_t *row = (uint8_t *)malloc(row_size);
    if (!row) {
        fprintf(stderr, "sdl3vk: cannot allocate capture row\n");
        fclose(file);
        return 0;
    }
    const uint8_t *pixels = (const uint8_t *)s_capture_map;
    int ok = 1;
    for (uint32_t y = 0; y < layout->height; y++) {
        uint32_t source_y =
            y * layout->source_step + layout->sample_offset;
        const uint8_t *source =
            pixels + (size_t)source_y * source_width * 4u;
        for (uint32_t x = 0; x < layout->width; x++) {
            uint32_t source_x =
                x * layout->source_step + layout->sample_offset;
            row[x * 3u] =
                source[source_x * 4u + (bgra ? 2u : 0u)];
            row[x * 3u + 1u] = source[source_x * 4u + 1u];
            row[x * 3u + 2u] =
                source[source_x * 4u + (bgra ? 0u : 2u)];
        }
        if (fwrite(row, 1, row_size, file) != row_size) {
            ok = 0;
            break;
        }
    }
    free(row);
    if (fclose(file) != 0)
        ok = 0;
    if (!ok)
        fprintf(stderr, "sdl3vk: cannot write capture %s\n", path);
    return ok;
}

/* ---- swapchain ---------------------------------------------------------------------- */

static void destroy_swapchain(void) {
    if (s_swap) { vkDestroySwapchainKHR(s_dev, s_swap, NULL); s_swap = VK_NULL_HANDLE; }
}

static int create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_pdev, s_surf, &caps));

    /* Prefer BGRA8 UNORM (matches the framebuffer byte order); fall back to whatever the
     * surface offers first. */
    uint32_t nf = 0;
    VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(
        s_pdev, s_surf, &nf, NULL));
    if (!nf) {
        fprintf(stderr, "sdl3vk: surface exposes no formats\n");
        return 0;
    }
    VkSurfaceFormatKHR *fmts =
        (VkSurfaceFormatKHR *)malloc(nf * sizeof(*fmts));
    if (!fmts) {
        fprintf(stderr, "sdl3vk: cannot allocate surface formats\n");
        return 0;
    }
    VkResult format_result =
        vkGetPhysicalDeviceSurfaceFormatsKHR(
            s_pdev, s_surf, &nf, fmts);
    if (format_result != VK_SUCCESS) {
        fprintf(
            stderr,
            "sdl3vk: vkGetPhysicalDeviceSurfaceFormatsKHR failed: %d\n",
            (int)format_result);
        free(fmts);
        return 0;
    }
    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { pick = fmts[i]; break; }
    free(fmts);
    s_swap_fmt = pick.format;

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {   /* surface lets us choose: use the window pixel size */
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(s_win, &w, &h);
        ext.width  = (uint32_t)w;  ext.height = (uint32_t)h;
    }
    if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
    if (ext.width  > caps.maxImageExtent.width)  ext.width  = caps.maxImageExtent.width;
    if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    if (ext.width == 0 || ext.height == 0) return 0;   /* minimized: keep old swapchain */
    s_swap_ext = ext;

    uint32_t n = caps.minImageCount + 1;
    if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;

    VkSwapchainKHR old = s_swap;
    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface          = s_surf;
    sci.minImageCount    = n;
    sci.imageFormat      = pick.format;
    sci.imageColorSpace  = pick.colorSpace;
    sci.imageExtent      = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform     = caps.currentTransform;
    sci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    /* Game speed is paced by the scheduler's vblank clock (sched.c vblank_pace), not by
     * presentation. MAILBOX (no tearing, never blocks) avoids beating against the monitor
     * refresh; FIFO is the mandated fallback. */
    sci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    {
        VkPresentModeKHR pm[8]; uint32_t npm = 8;
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(s_pdev, s_surf, &npm, pm) >= VK_SUCCESS)
            for (uint32_t i = 0; i < npm; i++)
                if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR) { sci.presentMode = pm[i]; break; }
    }
    sci.clipped          = VK_TRUE;
    sci.oldSwapchain     = old;
    VK_TRY(vkCreateSwapchainKHR(s_dev, &sci, NULL, &s_swap));
    if (old) vkDestroySwapchainKHR(s_dev, old, NULL);

    s_swap_n = 8;
    VK_TRY(vkGetSwapchainImagesKHR(s_dev, s_swap, &s_swap_n, s_swap_img));
    return 1;
}

static void log_window_metrics(const char *reason) {
    int logical_width = 0;
    int logical_height = 0;
    int pixel_width = 0;
    int pixel_height = 0;
    SDL_GetWindowSize(s_win, &logical_width, &logical_height);
    SDL_GetWindowSizeInPixels(s_win, &pixel_width, &pixel_height);
    fprintf(
        stderr,
        "sdl3vk: window %s logical=%dx%d drawable=%dx%d density=%.2f\n",
        reason, logical_width, logical_height, pixel_width, pixel_height,
        (double)SDL_GetWindowPixelDensity(s_win));
}

static bool SDLCALL lifecycle_event_watch(
    void *userdata, SDL_Event *event) {
    (void)userdata;
    if (!event) return true;
    switch (event->type) {
    case SDL_EVENT_TERMINATING:
        SDL_SetAtomicInt(&s_terminate_requested, 1);
        break;
    case SDL_EVENT_LOW_MEMORY:
        SDL_SetAtomicInt(&s_low_memory_requested, 1);
        break;
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        sdl3vk_set_system_suspended(1);
        break;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        sdl3vk_set_system_suspended(0);
        break;
    default:
        break;
    }
    return true;
}

void sdl3vk_set_system_suspended(int suspended) {
    suspended = suspended != 0;
    SDL_SetAtomicInt(
        &s_system_suspended, suspended);
    SDL_SetAtomicInt(
        suspended
            ? &s_system_suspend_requested
            : &s_system_resume_requested,
        1);
}

static void clear_input_state(void) {
    s_buttons = 0;
    s_lx = 128;
    s_ly = 128;
    sr_input_gamepad_disconnected();
}

static void apply_lifecycle_actions(
    uint32_t actions, int *redraw) {
    if (actions & SR_HOST_ACTION_CLEAR_INPUT)
        clear_input_state();
    if (actions & SR_HOST_ACTION_AUDIO_SUSPEND) {
        fprintf(stderr, "sdl3vk: host lifecycle suspended\n");
        sr_audio_set_suspended(1);
    }
    if (actions & SR_HOST_ACTION_REBASE_CLOCK)
        sched_host_resumed();
    if (actions & SR_HOST_ACTION_AUDIO_RESUME) {
        sr_audio_set_suspended(0);
        fprintf(stderr, "sdl3vk: host lifecycle resumed\n");
    }
    if ((actions & SR_HOST_ACTION_REDRAW) && redraw)
        *redraw = 1;
}

static void apply_lifecycle_event(
    SrHostLifecycleEvent event, int *redraw) {
    apply_lifecycle_actions(
        sr_host_lifecycle_apply(&s_lifecycle, event),
        redraw);
}

static int open_gamepad(SDL_JoystickID id) {
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    if (!pad) {
        fprintf(
            stderr, "sdl3vk: cannot open gamepad %u: %s\n",
            (unsigned)id, SDL_GetError());
        return 0;
    }
    s_pad = pad;
    const char *name = SDL_GetGamepadName(s_pad);
    fprintf(
        stderr, "sdl3vk: gamepad connected: %s\n",
        name ? name : "unnamed gamepad");
    return 1;
}

static void open_first_gamepad(void) {
    if (s_pad) return;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (!ids) return;
    for (int i = 0; i < count && !s_pad; i++)
        open_gamepad(ids[i]);
    SDL_free(ids);
}

/* ---- init --------------------------------------------------------------------------- */

int sdl3vk_init(const char *title) {
    const SrGraphicsConfig *graphics;
    const char *repeats = getenv("SR_GPU_PRESENT_REPEATS");
    s_present_repeats = repeats && repeats[0] && strcmp(repeats, "0") != 0;
    s_present_reused = 0;
    s_capture_count = 0;
    s_gpu_flip = 0;
    s_last_gpu_flip = 0;
    s_last_gpu_ms = -1.0;
    s_host_upload_bytes = 0;
    s_host_readback_bytes = 0;
    s_timestamp_bits = 0;
    s_timestamp_period = 0.0f;
    s_smaa_enabled = 0;
    s_smaa_width = 0;
    s_smaa_height = 0;
    memset(&s_pipeline_cache, 0, sizeof(s_pipeline_cache));
    sr_vk_present_invalidate(&s_present_cache);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "sdl3vk: SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    s_win = SDL_CreateWindow(title ? title : "PSP Recomp",
                             PSP_W * 2, PSP_H * 2,
                             SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                 SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!s_win) {
        fprintf(stderr, "sdl3vk: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
    }
    log_window_metrics("created");

    Uint32 next = 0;
    const char * const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&next);
    const char *instance_extensions[32];
    const char *validation_layers[] = {
        "VK_LAYER_KHRONOS_validation"
    };
    int validation =
        getenv("SR_VK_VALIDATION") &&
        getenv("SR_VK_VALIDATION")[0] &&
        strcmp(getenv("SR_VK_VALIDATION"), "0") != 0;
    if (next > 31) {
        fprintf(stderr, "sdl3vk: too many required instance extensions\n");
        return 0;
    }
    for (Uint32 i = 0; i < next; i++)
        instance_extensions[i] = sdl_ext[i];
    int portability =
        instance_extension_available(
            VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    if (portability) {
        int included = 0;
        for (Uint32 i = 0; i < next; i++)
            if (strcmp(
                    instance_extensions[i],
                    VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0)
                included = 1;
        if (!included)
            instance_extensions[next++] =
                VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    }
    if (validation) {
        if (!validation_layer_available()) {
            fprintf(
                stderr,
                "sdl3vk: SR_VK_VALIDATION requested but "
                "VK_LAYER_KHRONOS_validation is unavailable\n");
            return 0;
        }
        int has_debug_utils = 0;
        for (Uint32 i = 0; i < next; i++)
            if (strcmp(
                    instance_extensions[i],
                    VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
                has_debug_utils = 1;
        if (!has_debug_utils)
            instance_extensions[next++] =
                VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "psp_recomp";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkDebugUtilsMessengerCreateInfoEXT debug_info =
        validation_create_info();
    ici.pApplicationInfo = &ai;
    if (portability)
        ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ici.enabledExtensionCount = next;
    ici.ppEnabledExtensionNames = instance_extensions;
    if (validation) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = validation_layers;
        ici.pNext = &debug_info;
    }
    VK_TRY(vkCreateInstance(&ici, NULL, &s_inst));
    if (validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create_debug =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                s_inst, "vkCreateDebugUtilsMessengerEXT");
        if (!create_debug ||
            create_debug(
                s_inst, &debug_info, NULL,
                &s_debug_messenger) != VK_SUCCESS) {
            fprintf(
                stderr,
                "sdl3vk: failed to create Vulkan validation messenger\n");
            return 0;
        }
        fprintf(stderr, "sdl3vk: Vulkan validation enabled\n");
    }

    if (!SDL_Vulkan_CreateSurface(s_win, s_inst, NULL, &s_surf)) {
        fprintf(stderr, "sdl3vk: SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return 0;
    }

    /* Physical device: first one with a graphics queue that can present to our surface. */
    VkPhysicalDevice devs[16]; uint32_t nd = 16;
    VK_TRY(vkEnumeratePhysicalDevices(s_inst, &nd, devs));
    s_pdev = VK_NULL_HANDLE;
    s_timestamp_bits = 0;
    for (uint32_t d = 0; d < nd && !s_pdev; d++) {
        VkQueueFamilyProperties qf[16]; uint32_t nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, qf);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 can_present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[d], q, s_surf, &can_present);
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && can_present) {
                s_pdev = devs[d];
                s_qfam = q;
                s_timestamp_bits = qf[q].timestampValidBits;
                break;
            }
        }
    }
    if (!s_pdev) { fprintf(stderr, "sdl3vk: no usable Vulkan device\n"); return 0; }
    {
        VkPhysicalDeviceProperties pp;
        SrGraphicsAutoCaps caps;
        SrGraphicsAutoSelection selection;
        char quality_error[256];
        int drawable_width = 0;
        int drawable_height = 0;
        vkGetPhysicalDeviceProperties(s_pdev, &pp);
        fprintf(stderr, "sdl3vk: using %s\n", pp.deviceName);
        s_timestamp_period = pp.limits.timestampPeriod;
        if (!SDL_GetWindowSizeInPixels(
                s_win, &drawable_width, &drawable_height)) {
            drawable_width = 0;
            drawable_height = 0;
        }
        graphics = sr_graphics_config_get();
        memset(&caps, 0, sizeof(caps));
        caps.device_name = pp.deviceName;
        caps.drawable_width =
            drawable_width > 0 ? (uint32_t)drawable_width : 0u;
        caps.drawable_height =
            drawable_height > 0 ? (uint32_t)drawable_height : 0u;
        caps.max_image_dimension_2d =
            pp.limits.maxImageDimension2D;
        caps.hardware_renderer =
            graphics &&
            graphics->renderer != SR_GRAPHICS_RENDERER_SOFTWARE;
        caps.smaa_supported = sr_vk_smaa_supported(s_pdev);
        if (!sr_graphics_config_resolve_auto(
                &caps, &selection,
                quality_error, sizeof(quality_error))) {
            fprintf(
                stderr, "sdl3vk: automatic quality failed: %s\n",
                quality_error);
            return 0;
        }
        graphics = sr_graphics_config_get();
        if (graphics->anti_aliasing ==
                SR_GRAPHICS_ANTI_ALIASING_SMAA &&
            !caps.smaa_supported) {
            fprintf(
                stderr,
                "sdl3vk: SMAA required Vulkan image formats are "
                "unsupported\n");
            return 0;
        }
        s_smaa_enabled =
            graphics->anti_aliasing ==
            SR_GRAPHICS_ANTI_ALIASING_SMAA;
        fprintf(
            stderr,
            "graphics: %s quality -> %ux internal, %ux textures, "
            "%s (%s)\n",
            sr_graphics_auto_profile_name(selection.profile),
            selection.internal_resolution,
            selection.texture_scale,
            sr_graphics_anti_aliasing_name(
                selection.anti_aliasing),
            selection.reason);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = s_qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char *dev_ext[2] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    uint32_t dev_ext_count = 1;
    if (device_extension_available(
            s_pdev, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
        dev_ext[dev_ext_count++] =
            VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME;
    }
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = dev_ext_count;
    dci.ppEnabledExtensionNames = dev_ext;
    VkPhysicalDeviceFeatures enabled_features = {0};
    const char *tnl_validation =
        getenv("SR_GPU_TNL_VALIDATE");
    if (tnl_validation && tnl_validation[0] &&
        strcmp(tnl_validation, "0") != 0) {
        const char *gpu_tnl = getenv("SR_GPU_TNL");
        if (!gpu_tnl || !gpu_tnl[0] ||
            strcmp(gpu_tnl, "0") == 0) {
            fprintf(
                stderr,
                "sdl3vk: SR_GPU_TNL_VALIDATE requires "
                "SR_GPU_TNL=1\n");
            return 0;
        }
        VkPhysicalDeviceFeatures available_features;
        vkGetPhysicalDeviceFeatures(
            s_pdev, &available_features);
        if (!available_features.vertexPipelineStoresAndAtomics) {
            fprintf(
                stderr,
                "sdl3vk: SR_GPU_TNL_VALIDATE requires "
                "vertexPipelineStoresAndAtomics\n");
            return 0;
        }
        enabled_features.vertexPipelineStoresAndAtomics = VK_TRUE;
        dci.pEnabledFeatures = &enabled_features;
    }
    VK_TRY(vkCreateDevice(s_pdev, &dci, NULL, &s_dev));
    vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);
    {
        uint64_t shader_hash = SR_VK_PIPELINE_HASH_INIT;
        shader_hash = gegpu_shader_hash(shader_hash);
        shader_hash = sr_vk_smaa_shader_hash(shader_hash);
        if (!sr_vk_pipeline_cache_init(
                &s_pipeline_cache, s_pdev, s_dev,
                "cache/vulkan-pipelines.bin",
                VULKAN_PIPELINE_CACHE_SCHEMA, shader_hash)) {
            fprintf(
                stderr,
                "sdl3vk: continuing without a Vulkan pipeline cache\n");
        }
    }

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = s_qfam;
    VK_TRY(vkCreateCommandPool(s_dev, &cpi, NULL, &s_pool));
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = s_pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VK_TRY(vkAllocateCommandBuffers(s_dev, &cbi, &s_cmd));

    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_TRY(vkCreateFence(s_dev, &fci, NULL, &s_fence));
    VkSemaphoreCreateInfo sci2 = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK_TRY(vkCreateSemaphore(s_dev, &sci2, NULL, &s_sem_acq));
    for (uint32_t i = 0; i < 8; i++)
        VK_TRY(vkCreateSemaphore(
            s_dev, &sci2, NULL, &s_sem_done[i]));
    if (s_timestamp_bits && s_timestamp_period > 0.0f) {
        VkQueryPoolCreateInfo query_info = {
            VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO
        };
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = 2;
        VkResult query_result = vkCreateQueryPool(
            s_dev, &query_info, NULL, &s_timestamp_pool);
        if (query_result != VK_SUCCESS) {
            fprintf(
                stderr,
                "sdl3vk: GPU timestamp queries unavailable: %d\n",
                (int)query_result);
            s_timestamp_pool = VK_NULL_HANDLE;
        }
    }

    /* Persistently-mapped upload buffer + the device-local blit source image. */
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = PSP_W * PSP_H * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VK_TRY(vkCreateBuffer(s_dev, &bci, NULL, &s_staging));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(s_dev, s_staging, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_TRY(vkAllocateMemory(s_dev, &mai, NULL, &s_staging_mem));
    VK_TRY(vkBindBufferMemory(s_dev, s_staging, s_staging_mem, 0));
    VK_TRY(vkMapMemory(s_dev, s_staging_mem, 0, VK_WHOLE_SIZE, 0, &s_staging_map));

    VkImageCreateInfo imi = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imi.imageType = VK_IMAGE_TYPE_2D;
    imi.format = VK_FORMAT_B8G8R8A8_UNORM;
    imi.extent.width = PSP_W; imi.extent.height = PSP_H; imi.extent.depth = 1;
    imi.mipLevels = 1; imi.arrayLayers = 1;
    imi.samples = VK_SAMPLE_COUNT_1_BIT;
    imi.tiling = VK_IMAGE_TILING_OPTIMAL;
    imi.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_TRY(vkCreateImage(s_dev, &imi, NULL, &s_fbimg));
    vkGetImageMemoryRequirements(s_dev, s_fbimg, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_TRY(vkAllocateMemory(s_dev, &mai, NULL, &s_fbimg_mem));
    VK_TRY(vkBindImageMemory(s_dev, s_fbimg, s_fbimg_mem, 0));

    if (!create_swapchain()) return 0;

    open_first_gamepad();
    Uint32 window_flags = SDL_GetWindowFlags(s_win);
    sr_host_lifecycle_init(
        &s_lifecycle,
        SDL_GetAtomicInt(&s_system_suspended),
        (window_flags & SDL_WINDOW_INPUT_FOCUS) != 0,
        (window_flags & SDL_WINDOW_MINIMIZED) != 0);
    SDL_SetAtomicInt(&s_terminate_requested, 0);
    SDL_SetAtomicInt(&s_low_memory_requested, 0);
    if (!SDL_AddEventWatch(lifecycle_event_watch, NULL)) {
        fprintf(
            stderr, "sdl3vk: cannot install lifecycle event watch: %s\n",
            SDL_GetError());
        sdl3vk_shutdown();
        return 0;
    }
    s_event_watch_installed = 1;
    fprintf(
        stderr,
        "sdl3vk: init ok (%ux%u swapchain, fmt %d, gamepad=%s, "
        "gpu_timing=%s, smaa=%s)\n",
        s_swap_ext.width, s_swap_ext.height, (int)s_swap_fmt,
        s_pad ? "yes" : "no", s_timestamp_pool ? "yes" : "no",
        s_smaa_enabled ? "enabled" : "off");
    return 1;
}

void sdl3vk_set_title(const char *title) {
    if (s_win && title) SDL_SetWindowTitle(s_win, title);
}

/* ---- input -------------------------------------------------------------------------- */

static void toggle_fullscreen(void) {
    int fullscreen =
        (SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN) != 0;
    if (!SDL_SetWindowFullscreen(s_win, !fullscreen)) {
        fprintf(
            stderr, "sdl3vk: fullscreen toggle failed: %s\n",
            SDL_GetError());
    }
}

static void poll_input(int *quit, int *redraw) {
    SDL_Event ev;
    if (SDL_GetAtomicInt(&s_terminate_requested))
        *quit = 1;
    if (SDL_SetAtomicInt(&s_low_memory_requested, 0))
        fprintf(stderr, "sdl3vk: host reported low memory\n");
    if (SDL_SetAtomicInt(
            &s_system_suspend_requested, 0)) {
        apply_lifecycle_event(
            SR_HOST_SYSTEM_SUSPEND, redraw);
    }
    if (SDL_SetAtomicInt(
            &s_system_resume_requested, 0)) {
        apply_lifecycle_event(
            SR_HOST_SYSTEM_RESUME, redraw);
    }
    apply_lifecycle_event(
        SDL_GetAtomicInt(&s_system_suspended)
            ? SR_HOST_SYSTEM_SUSPEND
            : SR_HOST_SYSTEM_RESUME,
        redraw);
    while (SDL_PollEvent(&ev)) {
        if ((ev.type >= SDL_EVENT_WINDOW_FIRST && ev.type <= SDL_EVENT_WINDOW_LAST) ||
            (ev.type >= SDL_EVENT_DISPLAY_FIRST && ev.type <= SDL_EVENT_DISPLAY_LAST))
            *redraw = 1;
        switch (ev.type) {
        case SDL_EVENT_QUIT: *quit = 1; break;
        case SDL_EVENT_TERMINATING:
            *quit = 1;
            break;
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
        case SDL_EVENT_DID_ENTER_BACKGROUND:
            apply_lifecycle_event(
                SR_HOST_SYSTEM_SUSPEND, redraw);
            break;
        case SDL_EVENT_WILL_ENTER_FOREGROUND:
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            apply_lifecycle_event(
                SR_HOST_SYSTEM_RESUME, redraw);
            break;
        case SDL_EVENT_KEY_DOWN:
            if (ev.key.key == SDLK_ESCAPE) *quit = 1;
            if (!ev.key.repeat &&
                (ev.key.key == SDLK_F11 ||
                 (ev.key.key == SDLK_F &&
                  (ev.key.mod & SDL_KMOD_CTRL) &&
                  (ev.key.mod & SDL_KMOD_GUI)))) {
                toggle_fullscreen();
                *redraw = 1;
            }
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            log_window_metrics("drawable changed");
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            fprintf(stderr, "sdl3vk: window focus lost\n");
            apply_lifecycle_event(
                SR_HOST_FOCUS_LOST, redraw);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            fprintf(stderr, "sdl3vk: window focus gained\n");
            apply_lifecycle_event(
                SR_HOST_FOCUS_GAINED, redraw);
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            fprintf(stderr, "sdl3vk: window minimized\n");
            apply_lifecycle_event(
                SR_HOST_WINDOW_MINIMIZED, redraw);
            break;
        case SDL_EVENT_WINDOW_RESTORED:
            fprintf(stderr, "sdl3vk: window restored\n");
            apply_lifecycle_event(
                SR_HOST_WINDOW_RESTORED, redraw);
            break;
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            log_window_metrics("display changed");
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!s_pad) open_gamepad(ev.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (s_pad && SDL_GetGamepadID(s_pad) == ev.gdevice.which) {
                fprintf(stderr, "sdl3vk: gamepad disconnected\n");
                SDL_CloseGamepad(s_pad);
                s_pad = NULL;
                clear_input_state();
                open_first_gamepad();
            }
            break;
        case SDL_EVENT_AUDIO_DEVICE_REMOVED:
        case SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED:
            if (getenv("SR_AUDIO_LOG"))
                fprintf(
                    stderr,
                    "audio: SDL device event type=0x%x id=%u\n",
                    (unsigned)ev.type,
                    (unsigned)ev.adevice.which);
            break;
        default: break;
        }
    }

    s_pad_present = s_pad != NULL;
    if (!sr_host_lifecycle_input_active(&s_lifecycle)) {
        clear_input_state();
        return;
    }
    const bool *k = SDL_GetKeyboardState(NULL);
    Uint32 mouse = SDL_GetMouseState(NULL, NULL);
    s_buttons = sr_input_buttons(
        sr_input_sdl_key_down, (void *)k,
        sr_input_sdl_mouse_down, &mouse,
        s_pad ? sr_input_sdl_pad_down : NULL, s_pad);
    int key_x = 0, key_y = 0;
    sr_input_keyboard_axis(
        sr_input_sdl_key_down, (void *)k, &key_x, &key_y);
    int pad_x =
        s_pad ? SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTX) : 0;
    int pad_y =
        s_pad ? SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTY) : 0;
    if (s_pad) {
        SrInputSticks sticks = {
            key_x || key_y ? key_x : pad_x,
            key_x || key_y ? key_y : pad_y,
            SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHTX),
            SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHTY),
        };
        s_buttons =
            sr_input_gamepad_sticks(s_buttons, &sticks, &s_lx, &s_ly);
    } else {
        sr_input_gamepad_disconnected();
        sr_input_analog(key_x, key_y, &s_lx, &s_ly);
    }
}

int sdl3vk_get_vk(Sdl3VkInfo *out) {
    if (!s_dev || !out) return 0;
    out->instance = (void *)s_inst;
    out->physical = (void *)s_pdev;
    out->device   = (void *)s_dev;
    out->queue    = (void *)s_queue;
    out->pipeline_cache =
        (void *)s_pipeline_cache.handle;
    out->queue_family = s_qfam;
    return 1;
}

uint32_t sdl3vk_buttons(void) { return s_buttons; }
void sdl3vk_analog(uint8_t *lx, uint8_t *ly) { if (lx) *lx = s_lx; if (ly) *ly = s_ly; }
int  sdl3vk_pad_present(void) { return s_pad_present; }

/* ---- present ------------------------------------------------------------------------ */

static void barrier(VkCommandBuffer cmd, VkImage img,
                    VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_acc, VkAccessFlags dst_acc,
                    VkPipelineStageFlags src_st, VkPipelineStageFlags dst_st) {
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    mb.srcAccessMask = src_acc;
    mb.dstAccessMask = dst_acc;
    mb.oldLayout = from;
    mb.newLayout = to;
    mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = img;
    mb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    mb.subresourceRange.levelCount = 1;
    mb.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src_st, dst_st, 0, 0, NULL, 0, NULL, 1, &mb);
}

/* Blit the source region to the swapchain; the CPU path uploads it first. */
static int present_common(
    VkImage src, int srcw, int srch, int do_upload, uint64_t generation,
    VkImageLayout source_layout, int *transitioned) {
    int quit = 0;
    int displayw = srcw;
    int displayh = srch;
    int capture_requested = s_capture_count > 0;
    int redraw = s_present_repeats || capture_requested;
    SrVkCaptureLayout capture_layout = {0};
    if (s_smaa_enabled) {
        SrVkExtent extent;
        if (!sr_vk_internal_extent(
                sr_graphics_config_get()->internal_resolution,
                &extent)) {
            fprintf(stderr, "sdl3vk: invalid SMAA internal resolution\n");
            return -2;
        }
        displayw = (int)extent.width;
        displayh = (int)extent.height;
        if (s_smaa_width != (uint32_t)displayw ||
            s_smaa_height != (uint32_t)displayh) {
            redraw = 1;
        }
    }
    poll_input(&quit, &redraw);
    if (quit) return 0;
    if (s_smaa_enabled &&
        !sr_vk_smaa_prepare(
            s_pdev, s_dev, s_queue, s_qfam,
            s_pipeline_cache.handle,
            (uint32_t)displayw, (uint32_t)displayh)) {
        return -2;
    }
    if (s_smaa_enabled) {
        s_smaa_width = (uint32_t)displayw;
        s_smaa_height = (uint32_t)displayh;
    }
    if (sr_vk_present_can_reuse(&s_present_cache, generation, redraw)) {
        sr_vk_present_reused(&s_present_cache);
        s_present_reused++;
        return 1;
    }
    sr_vk_present_invalidate(&s_present_cache);
    if (capture_requested) {
        const char *internal_setting =
            getenv("SR_VULKAN_CAPTURE_INTERNAL");
        int internal_resolution =
            internal_setting && internal_setting[0] &&
            strcmp(internal_setting, "0") != 0;
        if (!sr_vk_capture_layout(
                (uint32_t)displayw, (uint32_t)displayh,
                internal_resolution, &capture_layout)) {
            fprintf(
                stderr,
                "sdl3vk: unsupported capture source size %dx%d\n",
                displayw, displayh);
            return -2;
        }
    }
    if (capture_requested &&
        !ensure_capture_buffer(
            (VkDeviceSize)(uint32_t)displayw *
            (VkDeviceSize)(uint32_t)displayh * 4u)) {
        return -2;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(s_dev, s_swap, UINT64_MAX, s_sem_acq,
                                            VK_NULL_HANDLE, &idx);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
            VkResult idle = vkDeviceWaitIdle(s_dev);
            if (idle != VK_SUCCESS) {
                fprintf(stderr, "sdl3vk: vkDeviceWaitIdle failed: %d\n", (int)idle);
                return -2;
            }
            if (!create_swapchain()) return 1;   /* minimized: drop the frame, stay alive */
            continue;
        }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "sdl3vk: vkAcquireNextImageKHR failed: %d\n", (int)ar);
            return -2;
        }

        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult result = vkResetCommandBuffer(s_cmd, 0);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "sdl3vk: vkResetCommandBuffer failed: %d\n", (int)result);
            return -2;
        }
        result = vkBeginCommandBuffer(s_cmd, &bi);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "sdl3vk: vkBeginCommandBuffer failed: %d\n", (int)result);
            return -2;
        }
        if (s_timestamp_pool) {
            vkCmdResetQueryPool(s_cmd, s_timestamp_pool, 0, 2);
            vkCmdWriteTimestamp(
                s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                s_timestamp_pool, 0);
        }

        if (do_upload) {
            /* staging buffer -> fb image */
            barrier(s_cmd, s_fbimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy bic = {0};
            bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bic.imageSubresource.layerCount = 1;
            bic.imageExtent.width = PSP_W; bic.imageExtent.height = PSP_H; bic.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(s_cmd, s_staging, s_fbimg,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
            barrier(s_cmd, s_fbimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        } else if (source_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            barrier(s_cmd, src, source_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        VkImage display_source = src;
        int capture_bgra = do_upload;
        if (s_smaa_enabled) {
            display_source = sr_vk_smaa_record(
                s_cmd, src, (uint32_t)srcw, (uint32_t)srch);
            if (!display_source) {
                fprintf(stderr, "sdl3vk: SMAA recording failed\n");
                return -2;
            }
            capture_bgra = 0;
        }
        if (capture_requested) {
            VkBufferImageCopy copy = {0};
            copy.imageSubresource.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent.width = (uint32_t)displayw;
            copy.imageExtent.height = (uint32_t)displayh;
            copy.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(
                s_cmd, display_source,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                s_capture_buffer, 1, &copy);

            VkBufferMemoryBarrier capture_barrier = {
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER
            };
            capture_barrier.srcAccessMask =
                VK_ACCESS_TRANSFER_WRITE_BIT;
            capture_barrier.dstAccessMask =
                VK_ACCESS_HOST_READ_BIT;
            capture_barrier.srcQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            capture_barrier.dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            capture_barrier.buffer = s_capture_buffer;
            capture_barrier.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(
                s_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                &capture_barrier, 0, NULL);
        }

        /* fb image -> swapchain, aspect-correct letterbox blit */
        barrier(s_cmd, s_swap_img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        {
            VkClearColorValue black = {{0, 0, 0, 1}};
            VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(s_cmd, s_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &black, 1, &rng);
        }
        barrier(s_cmd, s_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        SrVkPresentRect rect;
        if (!sr_vk_present_rect(
                s_swap_ext.width, s_swap_ext.height, &rect)) {
            fprintf(stderr, "sdl3vk: invalid swapchain extent\n");
            return -2;
        }
        VkImageBlit blt = {0};
        blt.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blt.srcSubresource.layerCount = 1;
        blt.srcOffsets[1].x = displayw;
        blt.srcOffsets[1].y = displayh;
        blt.srcOffsets[1].z = 1;
        blt.dstSubresource = blt.srcSubresource;
        blt.dstOffsets[0].x = (int32_t)rect.x;
        blt.dstOffsets[0].y = (int32_t)rect.y;
        blt.dstOffsets[1].x = (int32_t)(rect.x + rect.width);
        blt.dstOffsets[1].y = (int32_t)(rect.y + rect.height);
        blt.dstOffsets[1].z = 1;
        blt.dstOffsets[0].z = 0;
        vkCmdBlitImage(
                       s_cmd, display_source,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       s_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blt, VK_FILTER_LINEAR);
        barrier(s_cmd, s_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        if (s_timestamp_pool) {
            vkCmdWriteTimestamp(
                s_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                s_timestamp_pool, 1);
        }
        result = vkEndCommandBuffer(s_cmd);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "sdl3vk: vkEndCommandBuffer failed: %d\n", (int)result);
            return -2;
        }

        VkPipelineStageFlags wait_st = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s_sem_acq;
        si.pWaitDstStageMask = &wait_st;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s_cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s_sem_done[idx];
        result = vkQueueSubmit(s_queue, 1, &si, s_fence);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "sdl3vk: vkQueueSubmit failed: %d\n", (int)result);
            return -2;
        }
        if (transitioned && source_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            *transitioned = 1;

        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &s_sem_done[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &s_swap;
        pi.pImageIndices = &idx;
        VkResult pr = vkQueuePresentKHR(s_queue, &pi);

        result = vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "sdl3vk: vkWaitForFences failed: %d\n", (int)result);
            return -2;
        }
        uint64_t submitted_flip = ++s_gpu_flip;
        if (do_upload)
            s_host_upload_bytes +=
                (uint64_t)PSP_W * PSP_H * 4u;
        if (capture_requested)
            s_host_readback_bytes +=
                (uint64_t)(uint32_t)displayw *
                (uint64_t)(uint32_t)displayh * 4u;
        if (s_timestamp_pool) {
            uint64_t timestamps[2] = {0, 0};
            VkResult query_result = vkGetQueryPoolResults(
                s_dev, s_timestamp_pool, 0, 2,
                sizeof(timestamps), timestamps, sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT);
            if (query_result == VK_SUCCESS) {
                uint64_t mask =
                    s_timestamp_bits >= 64
                        ? UINT64_MAX
                        : ((UINT64_C(1) << s_timestamp_bits) - 1u);
                uint64_t delta =
                    (timestamps[1] - timestamps[0]) & mask;
                s_last_gpu_ms =
                    (double)delta * (double)s_timestamp_period /
                    1000000.0;
                s_last_gpu_flip = submitted_flip;
            } else if (query_result != VK_NOT_READY) {
                fprintf(
                    stderr,
                    "sdl3vk: vkGetQueryPoolResults failed: %d\n",
                    (int)query_result);
            }
        }
        if (capture_requested) {
            int capture_ok = 1;
            int count = s_capture_count;
            s_capture_count = 0;
            for (int i = 0; i < count; i++) {
                if (!write_capture(
                        s_capture_path[i], (uint32_t)displayw,
                        &capture_layout, capture_bgra)) {
                    capture_ok = 0;
                } else {
                    fprintf(
                        stderr,
                        "sdl3vk: captured %ux%u GPU frame -> %s\n",
                        capture_layout.width, capture_layout.height,
                        s_capture_path[i]);
                }
                s_capture_path[i][0] = '\0';
            }
            if (!capture_ok)
                return -2;
        }
        result = vkResetFences(s_dev, 1, &s_fence);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "sdl3vk: vkResetFences failed: %d\n", (int)result);
            return -2;
        }

        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR ||
            (pr == VK_SUCCESS && ar == VK_SUBOPTIMAL_KHR)) {
            result = vkDeviceWaitIdle(s_dev);
            if (result != VK_SUCCESS) {
                fprintf(stderr, "sdl3vk: vkDeviceWaitIdle failed: %d\n", (int)result);
                return -2;
            }
            create_swapchain();
        } else if (pr == VK_SUCCESS) {
            sr_vk_present_record(&s_present_cache, generation);
        } else {
            fprintf(stderr, "sdl3vk: vkQueuePresentKHR failed: %d\n", (int)pr);
            return -2;
        }
        break;
    }
    return 1;
}

int sdl3vk_present_rgba(const uint32_t *px) {
    memcpy(s_staging_map, px, PSP_W * PSP_H * 4);
    return present_common(
        s_fbimg, PSP_W, PSP_H, 1, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, NULL);
}

int sdl3vk_present_image(
    void *vk_image, uint32_t source_width, uint32_t source_height,
    uint64_t generation, uint32_t source_layout, int *transitioned) {
    *transitioned = 0;
    return present_common(
        (VkImage)vk_image, (int)source_width, (int)source_height, 0, generation,
        (VkImageLayout)source_layout, transitioned);
}

int sdl3vk_capture_next(const char *path) {
    if (!s_dev || !path || !path[0])
        return 0;
    size_t length = strlen(path);
    if (length >= sizeof(s_capture_path[0])) {
        fprintf(stderr, "sdl3vk: capture path is too long\n");
        return 0;
    }
    if (s_capture_count == CAPTURE_PATH_COUNT) {
        fprintf(
            stderr,
            "sdl3vk: too many captures requested for one frame\n");
        return 0;
    }
    memcpy(
        s_capture_path[s_capture_count], path, length + 1u);
    s_capture_count++;
    return 1;
}

double sdl3vk_last_gpu_ms(void) {
    return s_last_gpu_ms;
}

int sdl3vk_last_gpu_sample(
    uint64_t *flip, double *milliseconds) {
    if (!s_last_gpu_flip || s_last_gpu_ms < 0.0)
        return 0;
    if (flip)
        *flip = s_last_gpu_flip;
    if (milliseconds)
        *milliseconds = s_last_gpu_ms;
    return 1;
}

int sdl3vk_transfer_stats(Sdl3VkTransferStats *stats) {
    if (!s_dev || !stats)
        return 0;
    stats->host_upload_bytes = s_host_upload_bytes;
    stats->host_readback_bytes = s_host_readback_bytes;
    return 1;
}

void sdl3vk_shutdown(void) {
    if (s_event_watch_installed) {
        SDL_RemoveEventWatch(lifecycle_event_watch, NULL);
        s_event_watch_installed = 0;
    }
    if (s_present_reused)
        fprintf(stderr, "sdl3vk: reused %llu unchanged presentations\n",
                (unsigned long long)s_present_reused);
    s_present_reused = 0;
    sr_vk_present_invalidate(&s_present_cache);
    if (s_dev) vkDeviceWaitIdle(s_dev);
    destroy_swapchain();
    sr_vk_smaa_shutdown();
    sr_vk_pipeline_cache_shutdown(&s_pipeline_cache);
    s_smaa_enabled = 0;
    s_smaa_width = 0;
    s_smaa_height = 0;
    destroy_capture_buffer();
    if (s_timestamp_pool)
        vkDestroyQueryPool(s_dev, s_timestamp_pool, NULL);
    s_timestamp_pool = VK_NULL_HANDLE;
    if (s_fbimg)       vkDestroyImage(s_dev, s_fbimg, NULL);
    if (s_fbimg_mem)   vkFreeMemory(s_dev, s_fbimg_mem, NULL);
    if (s_staging_map) vkUnmapMemory(s_dev, s_staging_mem);
    s_staging_map = NULL;
    if (s_staging)     vkDestroyBuffer(s_dev, s_staging, NULL);
    if (s_staging_mem) vkFreeMemory(s_dev, s_staging_mem, NULL);
    if (s_sem_acq)     vkDestroySemaphore(s_dev, s_sem_acq, NULL);
    for (uint32_t i = 0; i < 8; i++)
        if (s_sem_done[i])
            vkDestroySemaphore(s_dev, s_sem_done[i], NULL);
    if (s_fence)       vkDestroyFence(s_dev, s_fence, NULL);
    if (s_pool)        vkDestroyCommandPool(s_dev, s_pool, NULL);
    if (s_dev)         vkDestroyDevice(s_dev, NULL);
    if (s_surf)        vkDestroySurfaceKHR(s_inst, s_surf, NULL);
    if (s_debug_messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                s_inst, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy_debug)
            destroy_debug(s_inst, s_debug_messenger, NULL);
    }
    if (s_inst)        vkDestroyInstance(s_inst, NULL);
    if (s_pad)         SDL_CloseGamepad(s_pad);
    if (s_win)         SDL_DestroyWindow(s_win);
    s_pad = NULL;
    s_win = NULL;
    s_inst = VK_NULL_HANDLE;
    s_surf = VK_NULL_HANDLE;
    s_pdev = VK_NULL_HANDLE;
    s_dev = VK_NULL_HANDLE;
    s_queue = VK_NULL_HANDLE;
    s_pool = VK_NULL_HANDLE;
    s_cmd = VK_NULL_HANDLE;
    s_fence = VK_NULL_HANDLE;
    s_sem_acq = VK_NULL_HANDLE;
    memset(s_sem_done, 0, sizeof(s_sem_done));
    s_debug_messenger = VK_NULL_HANDLE;
    s_timestamp_pool = VK_NULL_HANDLE;
    s_fbimg = VK_NULL_HANDLE;
    s_fbimg_mem = VK_NULL_HANDLE;
    s_staging = VK_NULL_HANDLE;
    s_staging_mem = VK_NULL_HANDLE;
    s_pad_present = 0;
    clear_input_state();
    SDL_Quit();
}

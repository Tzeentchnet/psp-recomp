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

#include "sdl3vk.h"
#include "../input_sdl.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSP_W 480
#define PSP_H 272

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

static uint32_t s_buttons;
static uint8_t  s_lx = 128, s_ly = 128;
static int      s_pad_present;

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

/* ---- swapchain ---------------------------------------------------------------------- */

static void destroy_swapchain(void) {
    if (s_swap) { vkDestroySwapchainKHR(s_dev, s_swap, NULL); s_swap = VK_NULL_HANDLE; }
}

static int create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_pdev, s_surf, &caps));

    /* Prefer BGRA8 UNORM (matches the framebuffer byte order); fall back to whatever the
     * surface offers first. */
    VkSurfaceFormatKHR fmts[32]; uint32_t nf = 32;
    VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(s_pdev, s_surf, &nf, fmts));
    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { pick = fmts[i]; break; }
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

/* ---- init --------------------------------------------------------------------------- */

int sdl3vk_init(const char *title) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "sdl3vk: SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    s_win = SDL_CreateWindow(title ? title : "PSP Recomp",
                             PSP_W * 2, PSP_H * 2,
                             SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!s_win) {
        fprintf(stderr, "sdl3vk: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
    }

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
    for (uint32_t d = 0; d < nd && !s_pdev; d++) {
        VkQueueFamilyProperties qf[16]; uint32_t nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, qf);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 can_present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[d], q, s_surf, &can_present);
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && can_present) {
                s_pdev = devs[d]; s_qfam = q; break;
            }
        }
    }
    if (!s_pdev) { fprintf(stderr, "sdl3vk: no usable Vulkan device\n"); return 0; }
    {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(s_pdev, &pp);
        fprintf(stderr, "sdl3vk: using %s\n", pp.deviceName);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = s_qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char *dev_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_ext;
    VK_TRY(vkCreateDevice(s_pdev, &dci, NULL, &s_dev));
    vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);

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

    if (SDL_HasGamepad()) {
        int npads = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&npads);
        if (ids && npads > 0) s_pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    fprintf(stderr, "sdl3vk: init ok (%ux%u swapchain, fmt %d, gamepad=%s)\n",
            s_swap_ext.width, s_swap_ext.height, (int)s_swap_fmt, s_pad ? "yes" : "no");
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

static void poll_input(int *quit) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_QUIT: *quit = 1; break;
        case SDL_EVENT_KEY_DOWN:
            if (ev.key.key == SDLK_ESCAPE) *quit = 1;
            if (ev.key.key == SDLK_F11 && !ev.key.repeat)
                toggle_fullscreen();
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!s_pad) s_pad = SDL_OpenGamepad(ev.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (s_pad && SDL_GetGamepadID(s_pad) == ev.gdevice.which) {
                SDL_CloseGamepad(s_pad); s_pad = NULL;
            }
            break;
        default: break;
        }
    }

    const bool *k = SDL_GetKeyboardState(NULL);
    Uint32 mouse = SDL_GetMouseState(NULL, NULL);
    s_pad_present = s_pad != NULL;
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

/* Common present: blit `src` (already TRANSFER_SRC_OPTIMAL; srcw x srch source region)
 * onto the swapchain with aspect-correct letterboxing. upload!=NULL additionally
 * records the staging->s_fbimg copy first (the CPU framebuffer path). */
static int present_common(VkImage src, int srcw, int srch, int do_upload) {
    int quit = 0;
    poll_input(&quit);
    if (quit) return 0;

    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(s_dev, s_swap, UINT64_MAX, s_sem_acq,
                                            VK_NULL_HANDLE, &idx);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(s_dev);
            if (!create_swapchain()) return 1;   /* minimized: drop the frame, stay alive */
            continue;
        }
        if (ar != VK_SUCCESS) return 1;

        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(s_cmd, 0);
        vkBeginCommandBuffer(s_cmd, &bi);

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
        }

        /* fb image -> swapchain, aspect-correct letterbox blit */
        barrier(s_cmd, s_swap_img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        {
            VkClearColorValue black = {{0, 0, 0, 1}};
            VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(s_cmd, s_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &black, 1, &rng);
        }
        int dw = (int)s_swap_ext.width, dh = (int)s_swap_ext.height;
        int vw = dw, vh = dw * PSP_H / PSP_W;
        if (vh > dh) { vh = dh; vw = dh * PSP_W / PSP_H; }
        int x0 = (dw - vw) / 2, y0 = (dh - vh) / 2;
        VkImageBlit blt = {0};
        blt.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blt.srcSubresource.layerCount = 1;
        blt.srcOffsets[1].x = srcw; blt.srcOffsets[1].y = srch; blt.srcOffsets[1].z = 1;
        blt.dstSubresource = blt.srcSubresource;
        blt.dstOffsets[0].x = x0;      blt.dstOffsets[0].y = y0;
        blt.dstOffsets[1].x = x0 + vw; blt.dstOffsets[1].y = y0 + vh; blt.dstOffsets[1].z = 1;
        blt.dstOffsets[0].z = 0;
        vkCmdBlitImage(s_cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       s_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blt, VK_FILTER_LINEAR);
        barrier(s_cmd, s_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vkEndCommandBuffer(s_cmd);

        VkPipelineStageFlags wait_st = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s_sem_acq;
        si.pWaitDstStageMask = &wait_st;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s_cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s_sem_done[idx];
        vkQueueSubmit(s_queue, 1, &si, s_fence);

        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &s_sem_done[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &s_swap;
        pi.pImageIndices = &idx;
        VkResult pr = vkQueuePresentKHR(s_queue, &pi);

        vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX);
        vkResetFences(s_dev, 1, &s_fence);

        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(s_dev);
            create_swapchain();
        }
        break;
    }
    return 1;
}

int sdl3vk_present_rgba(const uint32_t *px) {
    memcpy(s_staging_map, px, PSP_W * PSP_H * 4);
    return present_common(s_fbimg, PSP_W, PSP_H, 1);
}

int sdl3vk_present_image(void *vk_image) {
    /* 512x272 GE target: only the visible 480x272 region is shown */
    return present_common((VkImage)vk_image, PSP_W, PSP_H, 0);
}

void sdl3vk_shutdown(void) {
    if (s_dev) vkDeviceWaitIdle(s_dev);
    destroy_swapchain();
    if (s_fbimg)       vkDestroyImage(s_dev, s_fbimg, NULL);
    if (s_fbimg_mem)   vkFreeMemory(s_dev, s_fbimg_mem, NULL);
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
    SDL_Quit();
}

/* SDL3 + Vulkan presentation backend (Phase 0 of the GPU renderer plan, see README.md).
 *
 * C ABI consumed by gui.c when built with -DSR_SDL3VK. Phase 0 presents the software-GE
 * framebuffer through a Vulkan swapchain (SDL3 owns the window and all input devices);
 * later phases move GE rasterization itself onto the GPU behind this same boundary.
 *
 * This is the project's own Vulkan backend, written from scratch; it does not reuse
 * PPSSPP's GPU. (An earlier PPSSPP-GPU bridge under src/rt/gpu_vk was a frozen experiment
 * and has been removed.) */
#ifndef SR_SDL3VK_H
#define SR_SDL3VK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create the SDL3 window and the Vulkan device/swapchain. Returns 1 on success, 0 on any
 * failure (caller falls back to the GDI path). */
int  sdl3vk_init(const char *title);
void sdl3vk_set_title(const char *title);

/* Present one 480x272 frame. px points to 480*272 little-endian BGRA words (the same
 * packing gui.c already produces for StretchDIBits: (r<<16)|(g<<8)|b). Pumps the SDL
 * event loop and samples input. Returns 0 when the user closed the window / pressed ESC,
 * 1 otherwise, or -2 on a Vulkan failure. */
int  sdl3vk_present_rgba(const uint32_t *px);

/* Present an RGBA8 VkImage owned by the GPU rasterizer (ge_gpu.c). The image's
 * current Vulkan layout is supplied as its numeric value; transitioned is set
 * if a submitted command changed it to TRANSFER_SRC_OPTIMAL. A nonzero
 * generation identifies the contents and permits reuse of an unchanged frame.
 * Returns -2 on a Vulkan failure, 0 on close, or 1 otherwise. */
int  sdl3vk_present_image(
    void *vk_image, uint32_t source_width, uint32_t source_height,
    uint64_t generation, uint32_t source_layout, int *transitioned);

/* Input state captured by the last present (PSP sceCtrl button mask / analog stick). */
uint32_t sdl3vk_buttons(void);
void     sdl3vk_analog(uint8_t *lx, uint8_t *ly);
int      sdl3vk_pad_present(void);

/* Vulkan objects shared with the Phase-1 GPU rasterizer (ge_gpu.c). Handles are typed
 * void* here so this header stays vulkan.h-free; they are the real VkInstance etc.
 * Returns 0 until sdl3vk_init() has succeeded. */
typedef struct Sdl3VkInfo {
    void    *instance, *physical, *device, *queue;
    uint32_t queue_family;
} Sdl3VkInfo;
int sdl3vk_get_vk(Sdl3VkInfo *out);

void sdl3vk_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif

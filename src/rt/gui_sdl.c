#include "recomp.h"

#include "fps_counter.h"
#include "ge_shared.h"
#include "graphics_config.h"
#include "input_config.h"
#include "gpu_sdl3vk/ge_gpu.h"
#include "gpu_sdl3vk/sdl3vk.h"
#include "platform.h"
#ifdef __APPLE__
#include "macos_app.h"
#endif

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSP_W 480
#define PSP_H 272

#ifndef SR_GAME_TITLE
#define SR_GAME_TITLE "PSP Recomp"
#endif

static int s_on;
static uint32_t *s_pixels;
static uint32_t s_buttons;
static uint8_t s_lx = 128;
static uint8_t s_ly = 128;
static int s_pad_present;
static const GeBackend *s_backend;
static SrFpsCounter s_fps_counter;
static int s_fps_counter_enabled = -1;
static uint64_t s_fps_last;
static char s_window_title[128] = "PSP Recomp";

static void update_fps_counter(void) {
    if (s_fps_counter_enabled < 0)
        s_fps_counter_enabled = getenv("SR_FPS_COUNTER") ? 1 : 0;
    if (!s_fps_counter_enabled) return;
    uint64_t now = sr_time_now();
    double fps;
    double frame_ms;
    if (!sr_fps_counter_tick(
            &s_fps_counter, now, sr_time_frequency(),
            0.5, &fps, &frame_ms)) {
        return;
    }
    char title[256];
    snprintf(
        title, sizeof(title), "%s | %.1f FPS | %.2f ms",
        s_window_title, fps, frame_ms);
    sdl3vk_set_title(title);
    s_fps_last = now;
}

static int renderer_requested(void) {
    const char *override = getenv("SR_GPU");
    if (override && override[0]) {
        if (strcmp(override, "vk") == 0 ||
            strcmp(override, "vulkan") == 0)
            return SR_GRAPHICS_RENDERER_VULKAN;
        if (strcmp(override, "sw") == 0 ||
            strcmp(override, "software") == 0)
            return SR_GRAPHICS_RENDERER_SOFTWARE;
        fprintf(
            stderr,
            "gui_init: invalid SR_GPU=%s (expected vk or sw)\n",
            override);
        return -1;
    }
    const SrGraphicsConfig *config = sr_graphics_config_get();
    if (!config || config->renderer == SR_GRAPHICS_RENDERER_AUTO)
        return SR_GRAPHICS_RENDERER_VULKAN;
    return config->renderer;
}

int gui_init(const char *title) {
    if (s_on) return 1;
    if (!sr_graphics_config_init() || !sr_input_init()) return 0;
    if (sr_graphics_config_first_run()) {
        SrGraphicsConfig defaults = *sr_graphics_config_get();
        char error[512];
        if (!sr_graphics_config_save(
                &defaults, error, sizeof(error))) {
            fprintf(
                stderr, "graphics: cannot save defaults: %s\n",
                error);
            return 0;
        }
    }
    int renderer = renderer_requested();
    if (renderer < 0) return 0;
    if (renderer == SR_GRAPHICS_RENDERER_D3D12) {
        fprintf(
            stderr,
            "gui_init: D3D12 is unavailable on this platform; "
            "select renderer = vk or sw in graphics.ini\n");
        return 0;
    }
    if (sr_graphics_config_get()->quality_mode ==
            SR_GRAPHICS_QUALITY_MANUAL &&
        sr_graphics_config_get()->anti_aliasing ==
            SR_GRAPHICS_ANTI_ALIASING_SMAA &&
        renderer != SR_GRAPHICS_RENDERER_VULKAN) {
        fprintf(
            stderr,
            "gui_init: SMAA 1x High requires renderer = vk "
            "on this platform\n");
        return 0;
    }
    snprintf(
        s_window_title, sizeof(s_window_title), "%s",
        title ? title : SR_GAME_TITLE);
    if (!sdl3vk_init(s_window_title)) {
        fprintf(
            stderr,
            "gui_init: SDL3/Vulkan initialization failed; "
            "verify that a Vulkan ICD is installed and available\n");
        return 0;
    }
    s_pixels = (uint32_t *)malloc(PSP_W * PSP_H * sizeof(*s_pixels));
    if (!s_pixels) {
        fprintf(stderr, "gui_init: framebuffer allocation failed\n");
        sdl3vk_shutdown();
        return 0;
    }
    if (renderer == SR_GRAPHICS_RENDERER_VULKAN) {
        s_backend = gegpu_backend();
        if (!s_backend || !s_backend->init || !s_backend->init()) {
            fprintf(stderr, "gui_init: Vulkan GE backend initialization failed\n");
            if (s_backend && s_backend->shutdown)
                s_backend->shutdown();
            s_backend = NULL;
            free(s_pixels);
            s_pixels = NULL;
            sdl3vk_shutdown();
            return 0;
        }
        ge_set_backend(s_backend);
    }
    if (!sr_input_start_hot_reload()) {
        if (s_backend && s_backend->shutdown)
            s_backend->shutdown();
        ge_set_backend(NULL);
        s_backend = NULL;
        free(s_pixels);
        s_pixels = NULL;
        sdl3vk_shutdown();
        return 0;
    }
    s_fps_last = sr_time_now();
    s_on = 1;
    fprintf(
        stderr, "gui_init: GE backend %s (Vulkan presentation)\n",
        s_backend ? s_backend->name : "sw");
    return 1;
}

void gui_shutdown(void) {
    if (!s_on) return;
    sr_input_stop_hot_reload();
    ge_set_backend(NULL);
    if (s_backend && s_backend->shutdown)
        s_backend->shutdown();
    s_backend = NULL;
    sdl3vk_shutdown();
    free(s_pixels);
    s_pixels = NULL;
    s_buttons = 0;
    s_lx = s_ly = 128;
    s_pad_present = 0;
    s_on = 0;
    s_fps_counter_enabled = -1;
    sr_fps_counter_reset(&s_fps_counter);
}

int gui_on(void) {
    return s_on;
}

uint32_t gui_buttons(void) {
    return s_buttons;
}

void gui_analog(uint8_t *lx, uint8_t *ly) {
    if (lx) *lx = s_lx;
    if (ly) *ly = s_ly;
}

int gui_pad_present(void) {
    return s_pad_present;
}

int gui_message(
    const char *text, int yes_no,
    int allow_cancel, int default_no) {
    int fallback = yes_no && default_no ? 2 : 1;
    if (!s_on || getenv("SR_NO_DIALOGS")) return fallback;
    SDL_MessageBoxButtonData buttons[3];
    int count = 0;
    buttons[count++] = (SDL_MessageBoxButtonData){
        default_no ? 0u : SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,
        1, yes_no ? "Yes" : "OK",
    };
    if (yes_no) {
        buttons[count++] = (SDL_MessageBoxButtonData){
            default_no ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0u,
            2, "No",
        };
    }
    if (allow_cancel) {
        buttons[count++] = (SDL_MessageBoxButtonData){
            SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,
            3, "Cancel",
        };
    }
    SDL_MessageBoxData box = {
        .flags = SDL_MESSAGEBOX_INFORMATION,
        .title = SR_GAME_TITLE,
        .message = text && text[0] ? text : SR_GAME_TITLE,
        .numbuttons = count,
        .buttons = buttons,
    };
    int selected = fallback;
    if (!SDL_ShowMessageBox(&box, &selected)) {
        fprintf(stderr, "gui_message: %s\n", SDL_GetError());
        return fallback;
    }
    return selected < 0 ? fallback : selected;
}

int gui_capture_next(const char *path) {
    return s_on ? sdl3vk_capture_next(path) : 0;
}

double gui_last_gpu_ms(void) {
    return s_on ? sdl3vk_last_gpu_ms() : -1.0;
}

int gui_last_gpu_sample(uint64_t *flip, double *milliseconds) {
    return s_on
        ? sdl3vk_last_gpu_sample(flip, milliseconds)
        : 0;
}

int gui_thermal_state(void) {
#ifdef __APPLE__
    static uint64_t last_check;
    static int state = -1;
    uint64_t now = sr_time_now();
    uint64_t frequency = sr_time_frequency();
    if (!last_check || !frequency || now - last_check >= frequency) {
        state = sr_macos_thermal_state();
        last_check = now;
    }
    return state;
#else
    return -1;
#endif
}

int gui_last_backend_timing(SrBackendTiming *timing) {
    if (s_on && timing) {
        GeVkBackendTiming source;
        if (!gegpu_last_backend_timing(&source))
            return 0;
        timing->flip = source.flip;
        timing->begin_ms = source.begin_ms;
        timing->vertices_ms = source.vertices_ms;
        timing->framebuffer_ms = source.framebuffer_ms;
        timing->textures_ms = source.textures_ms;
        timing->draws_ms = source.draws_ms;
        timing->present_ms = source.present_ms;
        timing->submit_ms = source.submit_ms;
        timing->vertices = source.vertices;
        timing->batches = source.batches;
        return 1;
    }
    return 0;
}

int gui_framebuffer_stats(SrFramebufferStats *stats) {
    if (s_on && stats) {
        GeVkFramebufferStats source;
        Sdl3VkTransferStats presenter;
        if (!gegpu_framebuffer_stats(&source))
            return 0;
        memset(stats, 0, sizeof(*stats));
        stats->readbacks = source.readbacks;
        stats->aliases = source.aliases;
        stats->feedback_snapshots = source.feedback_snapshots;
        stats->feedback_snapshot_pixels =
            source.feedback_snapshot_pixels;
        stats->feedback_reuses = source.feedback_reuses;
        stats->pipeline_key_hits = source.pipeline_key_hits;
        stats->pipelines_created = source.pipelines_created;
        stats->pipeline_create_us = source.pipeline_create_us;
        stats->texture_cache_hits = source.texture_cache_hits;
        stats->texture_cache_misses = source.texture_cache_misses;
        stats->texture_uploads = source.texture_uploads;
        stats->texture_decode_us = source.texture_decode_us;
        stats->texture_scale_us = source.texture_scale_us;
        stats->host_upload_bytes = source.host_upload_bytes;
        stats->host_readback_bytes = source.host_readback_bytes;
        if (sdl3vk_transfer_stats(&presenter)) {
            stats->host_upload_bytes += presenter.host_upload_bytes;
            stats->host_readback_bytes += presenter.host_readback_bytes;
        }
        return 1;
    }
    return 0;
}

static void convert_framebuffer(
    uint32_t address, int format, uint32_t stride) {
    for (int y = 0; y < PSP_H; y++) {
        for (int x = 0; x < PSP_W; x++) {
            uint32_t index = (uint32_t)y * stride + (uint32_t)x;
            int red;
            int green;
            int blue;
            if (format == 3) {
                uint32_t pixel = sr_r32(address + index * 4u);
                red = pixel & 0xff;
                green = (pixel >> 8) & 0xff;
                blue = (pixel >> 16) & 0xff;
            } else {
                uint16_t pixel = sr_r16(address + index * 2u);
                if (format == 1) {
                    red = (pixel & 0x1f) << 3;
                    green = ((pixel >> 5) & 0x1f) << 3;
                    blue = ((pixel >> 10) & 0x1f) << 3;
                } else if (format == 2) {
                    red = (pixel & 0x0f) << 4;
                    green = ((pixel >> 4) & 0x0f) << 4;
                    blue = ((pixel >> 8) & 0x0f) << 4;
                } else {
                    red = (pixel & 0x1f) << 3;
                    green = ((pixel >> 5) & 0x3f) << 2;
                    blue = ((pixel >> 11) & 0x1f) << 3;
                }
            }
            s_pixels[y * PSP_W + x] =
                ((uint32_t)red << 16) |
                ((uint32_t)green << 8) |
                (uint32_t)blue;
        }
    }
}

void gui_present(uint32_t address, int format, uint32_t stride) {
    if (!s_on) return;
    sr_input_poll_reload();
    if (!stride) stride = 512;
    int shown = s_backend && s_backend->present
        ? s_backend->present(address, format, stride)
        : -1;
    if (shown < -1) {
        fprintf(stderr, "gui_present: Vulkan backend failed\n");
        gui_shutdown();
        _Exit(2);
    }
    if (shown == -1) {
        convert_framebuffer(address, format, stride);
        shown = sdl3vk_present_rgba(s_pixels);
        if (shown < 0) {
            fprintf(stderr, "gui_present: Vulkan presenter failed\n");
            gui_shutdown();
            _Exit(2);
        }
    }
    if (!shown) {
        gui_shutdown();
        _Exit(0);
    }
    s_buttons = sdl3vk_buttons();
    sdl3vk_analog(&s_lx, &s_ly);
    s_pad_present = sdl3vk_pad_present();
    update_fps_counter();
}

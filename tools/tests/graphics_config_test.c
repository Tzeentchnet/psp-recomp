#include "graphics_config.h"
#include "test_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check_true(const char *name, int value) {
    if (!value) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    }
}

static void check_u32(const char *name, uint32_t got, uint32_t expected) {
    if (got != expected) {
        fprintf(
            stderr, "FAIL: %s: got %u, expected %u\n",
            name, got, expected);
        failures++;
    }
}

int main(void) {
    SrGraphicsConfig config;
    char error[256];
    char config_path[SR_TEST_PATH_MAX];

    sr_graphics_config_defaults(&config);
    check_u32(
        "default renderer", (uint32_t)config.renderer,
#ifdef _WIN32
        SR_GRAPHICS_RENDERER_D3D12);
#else
        SR_GRAPHICS_RENDERER_VULKAN);
#endif
#ifdef __APPLE__
    check_u32("default internal resolution", config.internal_resolution, 1);
#else
    check_u32("default internal resolution", config.internal_resolution, 2);
#endif
    check_u32("default texture scale", config.texture_scale, 1);
    check_u32("default D3D12 full-rate", config.d3d12_full_rate, 0);
    check_u32(
        "default anti-aliasing", (uint32_t)config.anti_aliasing,
        SR_GRAPHICS_ANTI_ALIASING_OFF);

    check_true(
        "parse graphics config",
        sr_graphics_config_parse(
            &config,
            "renderer = software\n"
            "internal_resolution = 8\n"
            "texture_scale = 4\n"
            "d3d12_full_rate = 1\n"
            "anti_aliasing = smaa\n",
            error, sizeof(error)));
    check_u32(
        "parsed renderer", (uint32_t)config.renderer,
        SR_GRAPHICS_RENDERER_SOFTWARE);
    check_u32("parsed internal resolution", config.internal_resolution, 8);
    check_u32("parsed texture scale", config.texture_scale, 4);
    check_u32("parsed D3D12 full-rate", config.d3d12_full_rate, 1);
    check_u32(
        "parsed anti-aliasing", (uint32_t)config.anti_aliasing,
        SR_GRAPHICS_ANTI_ALIASING_SMAA);

    sr_graphics_config_defaults(&config);
    check_true(
        "parse v1 graphics config",
        sr_graphics_config_parse(
            &config,
            "renderer = vk\n"
            "internal_resolution = 3\n",
            error, sizeof(error)));
    check_u32("v1 texture scale default", config.texture_scale, 1);
    check_u32("v1 D3D12 full-rate default", config.d3d12_full_rate, 0);
    check_u32(
        "v1 anti-aliasing default", (uint32_t)config.anti_aliasing,
        SR_GRAPHICS_ANTI_ALIASING_OFF);

    sr_graphics_config_defaults(&config);
    check_true(
        "reject duplicate renderer",
        !sr_graphics_config_parse(
            &config,
            "renderer=d3d12\nrenderer=sw\n",
            error, sizeof(error)));
    check_true(
        "duplicate diagnostic",
        strstr(error, "duplicate setting") != NULL);
    sr_graphics_config_defaults(&config);
    check_true(
        "reject invalid scale",
        !sr_graphics_config_parse(
            &config, "internal_resolution=9\n",
            error, sizeof(error)));
    sr_graphics_config_defaults(&config);
    check_true(
        "reject invalid texture scale",
        !sr_graphics_config_parse(
            &config, "texture_scale=6\n",
            error, sizeof(error)));
    sr_graphics_config_defaults(&config);
    check_true(
        "reject invalid D3D12 full-rate",
        !sr_graphics_config_parse(
            &config, "d3d12_full_rate=2\n",
            error, sizeof(error)));
    sr_graphics_config_defaults(&config);
    check_true(
        "reject invalid anti-aliasing",
        !sr_graphics_config_parse(
            &config, "anti_aliasing=msaa\n",
            error, sizeof(error)));
    sr_graphics_config_defaults(&config);
    check_true(
        "reject unknown setting",
        !sr_graphics_config_parse(
            &config, "anisotropic_filtering=16\n",
            error, sizeof(error)));

    check_true(
        "temporary config path",
        sr_test_temp_path(
            config_path, sizeof(config_path), "kzl-graphics", ".ini"));
    remove(config_path);
    check_true(
        "set graphics config path",
        sr_test_set_env("SR_GRAPHICS_CONFIG", config_path));
    check_true("initialize missing graphics config", sr_graphics_config_init());
    check_true("first run detected", sr_graphics_config_first_run());
    check_true(
        "graphics config path",
        strcmp(sr_graphics_config_path(), config_path) == 0);

    config = *sr_graphics_config_get();
    config.renderer = SR_GRAPHICS_RENDERER_SOFTWARE;
    config.internal_resolution = 6;
    config.texture_scale = 3;
    config.d3d12_full_rate = 1;
    config.anti_aliasing = SR_GRAPHICS_ANTI_ALIASING_SMAA;
    check_true(
        "save graphics config",
        sr_graphics_config_save(&config, error, sizeof(error)));
    check_true("first run cleared", !sr_graphics_config_first_run());
    check_u32(
        "saved renderer",
        (uint32_t)sr_graphics_config_get()->renderer,
        SR_GRAPHICS_RENDERER_SOFTWARE);
    check_u32(
        "saved internal resolution",
        sr_graphics_config_get()->internal_resolution, 6);
    check_u32(
        "saved texture scale",
        sr_graphics_config_get()->texture_scale, 3);
    check_u32(
        "saved D3D12 full-rate",
        sr_graphics_config_get()->d3d12_full_rate, 1);
    check_u32(
        "saved anti-aliasing",
        (uint32_t)sr_graphics_config_get()->anti_aliasing,
        SR_GRAPHICS_ANTI_ALIASING_SMAA);
    {
        FILE *file = fopen(config_path, "rb");
        char text[256] = {0};
        check_true("open saved graphics config", file != NULL);
        if (file) {
            size_t length = fread(text, 1, sizeof(text) - 1u, file);
            text[length] = 0;
            check_true("close graphics config", fclose(file) == 0);
            check_true(
                "saved renderer text",
                strstr(text, "renderer = sw") != NULL);
            check_true(
                "saved scale text",
                strstr(text, "internal_resolution = 6") != NULL);
            check_true(
                "saved texture scale text",
                strstr(text, "texture_scale = 3") != NULL);
            check_true(
                "saved D3D12 full-rate text",
                strstr(text, "d3d12_full_rate = 1") != NULL);
            check_true(
                "saved anti-aliasing text",
                strstr(text, "anti_aliasing = smaa") != NULL);
        }
    }
    check_true(
        "set scale override",
        sr_test_set_env("SR_D3D12_SCALE", "7"));
    {
        uint32_t scale = 0;
        check_true(
            "effective scale override",
            sr_graphics_effective_scale(&scale, error, sizeof(error)));
        check_u32("overridden scale", scale, 7);
    }
    check_true(
        "set invalid scale override",
        sr_test_set_env("SR_D3D12_SCALE", "0"));
    {
        uint32_t scale = 0;
        check_true(
            "reject scale override",
            !sr_graphics_effective_scale(&scale, error, sizeof(error)));
    }
    check_true(
        "clear scale override",
        sr_test_set_env("SR_D3D12_SCALE", NULL));
    remove(config_path);

    if (failures) {
        fprintf(stderr, "graphics config tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("graphics config tests passed\n");
    return 0;
}

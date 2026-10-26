#include "gpu_sdl3vk/shader_state.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int s_failures;

static void fail(const char *name) {
    fprintf(stderr, "FAIL: %s\n", name);
    s_failures++;
}

static void check_u32(
    const char *name, uint32_t actual, uint32_t expected) {
    if (actual != expected) {
        fprintf(
            stderr, "FAIL: %s: got 0x%08x, expected 0x%08x\n",
            name, actual, expected);
        s_failures++;
    }
}

static uint32_t shader_flags(const GeVkShaderState *state) {
    return (uint32_t)state->config[0] >> 8;
}

static void test_direct_classification(void) {
    GeState state;
    memset(&state, 0, sizeof(state));
    state.fbfmt = 3;
    if (!ge_vk_shader_direct_compatible(&state))
        fail("basic state rejected from direct path");

    state.blend_enable = 1;
    state.blend_mode = 2u | (3u << 4);
    if (!ge_vk_shader_direct_compatible(&state))
        fail("source-alpha blend rejected from direct path");

    state.dither_enable = 1;
    if (ge_vk_shader_direct_compatible(&state))
        fail("blend-before-dither ordering admitted to direct path");
    state.dither_enable = 0;

    state.blend_mode = 6u | (3u << 4);
    if (ge_vk_shader_direct_compatible(&state))
        fail("double-source-alpha blend admitted to direct path");
    state.blend_enable = 0;

    state.logic_op_enable = 1;
    if (ge_vk_shader_direct_compatible(&state))
        fail("logic operation admitted to direct path");
    state.logic_op_enable = 0;

    state.mask_rgb = 1;
    if (ge_vk_shader_direct_compatible(&state))
        fail("native RGB mask admitted to direct path");
    state.mask_rgb = 0;

    state.clear = 1;
    if (ge_vk_shader_direct_compatible(&state))
        fail("clear admitted to direct path");
}

static void test_stencil_classification(void) {
    GeState state;
    memset(&state, 0, sizeof(state));
    state.fbfmt = 1;
    state.stencil_test_enable = 1;
    state.stencil_test = 1u | (0x66u << 8) | (0xffu << 16);
    state.stencil_op = 4u << 16;
    if (!ge_vk_shader_direct_compatible(&state))
        fail("constant 5551 stencil increment rejected");
    if (!ge_vk_shader_direct_stencil_write(&state))
        fail("direct stencil write not reported");

    state.stencil_op = 1u << 8;
    if (ge_vk_shader_direct_compatible(&state))
        fail("depth-fail stencil mutation admitted to direct path");
    state.stencil_op = 4u << 16;

    state.mask_alpha = 0xff;
    if (ge_vk_shader_direct_compatible(&state))
        fail("masked stencil write admitted to direct path");
    state.mask_alpha = 0;

    state.fbfmt = 3;
    if (ge_vk_shader_direct_compatible(&state))
        fail("destination-dependent 8888 stencil increment admitted");
}

static void test_state_packing(void) {
    GeState source;
    GeVkShaderState state;
    uint32_t expected_flags =
        GE_VK_SHADER_TEXTURE |
        GE_VK_SHADER_TEXTURE_RGBA |
        GE_VK_SHADER_COLOR_DOUBLE |
        GE_VK_SHADER_FOG |
        GE_VK_SHADER_DEPTH_RANGE |
        GE_VK_SHADER_NEAREST |
        GE_VK_SHADER_COLOR_TEST |
        GE_VK_SHADER_DITHER |
        GE_VK_SHADER_STENCIL |
        GE_VK_SHADER_CLAMP_U |
        GE_VK_SHADER_CLAMP_V;
    memset(&source, 0, sizeof(source));
    source.fbfmt = 2;
    source.tex_enable = 1;
    source.tex_addr = 0x04010000u;
    source.tex_func = 3u | 0x100u | 0x10000u;
    source.tex_filter = 0;
    source.tex_wrap = 1u | (1u << 8);
    source.tex_w = 64;
    source.tex_h = 128;
    source.tex_env = 0x00112233u;
    source.fog_enable = 1;
    source.fog_color = 0x00445566u;
    source.atest_enable = 1;
    source.atest = 6u | (0x77u << 8) | (0xf0u << 16);
    source.minz = 0x1234;
    source.maxz = 0xabcd;
    source.color_test_enable = 1;
    source.color_test = 3;
    source.color_ref = 0x00778899u;
    source.color_test_mask = 0x00aabbccu;
    source.clut_fmt = 0x01234567u;
    source.stencil_test_enable = 1;
    source.stencil_test = 5u | (0x66u << 8) | (0xf0u << 16);
    source.stencil_op = 1u | (2u << 8) | (3u << 16);
    source.mask_alpha = 0x78;
    source.dither_enable = 1;
    source.dith[0][0] = -4;
    source.dith[0][1] = -3;
    source.dith[0][2] = -2;
    source.dith[0][3] = -1;
    source.blend_enable = 1;
    source.blend_mode = 6u | (9u << 4) | (2u << 8);
    source.blend_fixa = 0x00123456u;
    source.blend_fixb = 0x00654321u;
    source.mask_rgb = 0x00abcdefu;
    source.logic_op_enable = 1;
    source.logic_op = 6;
    source.ztest_enable = 1;
    source.ztest = 7;
    source.zwrite_disable = 0;

    ge_vk_shader_state_build(&source, 1, 0, &state);
    check_u32("push constant size", sizeof(state), 112);
    check_u32("shader flags", shader_flags(&state), expected_flags);
    check_u32("texture function", (uint32_t)state.config[0] & 7u, 3);
    check_u32("alpha test", (uint32_t)state.config[1], source.atest);
    check_u32("minimum depth", (uint32_t)state.config[2], source.minz);
    check_u32("maximum depth", (uint32_t)state.config[3], source.maxz);
    check_u32("texture environment", state.texture[0], 0x00112233u);
    check_u32("fog color", state.texture[1], 0x00445566u);
    check_u32("texture dimensions", state.texture[2], 64u | (128u << 16));
    check_u32("color-test function", (uint32_t)state.color_test[0], 3);
    check_u32("color-test reference", (uint32_t)state.color_test[1], 0x00778899u);
    check_u32("color-test mask", (uint32_t)state.color_test[2], 0x00aabbccu);
    check_u32(
        "CLUT and wrap state", (uint32_t)state.color_test[3],
        source.clut_fmt | (1u << 30) | (1u << 31));
    check_u32("stencil test", (uint32_t)state.stencil[0], source.stencil_test);
    check_u32("stencil operations", (uint32_t)state.stencil[1], source.stencil_op);
    check_u32("stencil write mask", (uint32_t)state.stencil[2], 0x78);
    check_u32("framebuffer format", (uint32_t)state.stencil[3], 2);
    check_u32("signed dither row", (uint32_t)state.dither[0], 0xfedcu);
    check_u32("blend enabled", (uint32_t)state.blend[0], 1);
    check_u32("blend mode", (uint32_t)state.blend[1], source.blend_mode);
    check_u32("fixed source", (uint32_t)state.blend[2], 0x00123456u);
    check_u32("fixed destination", (uint32_t)state.blend[3], 0x00654321u);
    check_u32(
        "native write mask", (uint32_t)state.fragment[0], 0x78abcdefu);
    check_u32("logic state", (uint32_t)state.fragment[1], 1u | (6u << 8));
    check_u32(
        "depth state", (uint32_t)state.fragment[3],
        1u | (7u << 8) | (1u << 16));
}

static void test_clear_packing(void) {
    GeState source;
    GeVkShaderState state;
    memset(&source, 0, sizeof(source));
    source.clear = 1;
    source.clear_mode = 0x500;
    source.tex_enable = 1;
    source.tex_addr = 0x04010000u;
    source.atest_enable = 1;
    source.color_test_enable = 1;
    source.stencil_test_enable = 1;
    source.blend_enable = 1;
    source.logic_op_enable = 1;

    ge_vk_shader_state_build(&source, 1, 1, &state);
    check_u32(
        "clear flags", shader_flags(&state), GE_VK_SHADER_CLEAR);
    check_u32("clear alpha test", (uint32_t)state.config[1], 1);
    check_u32("clear color test", (uint32_t)state.color_test[0], 1);
    check_u32("clear stencil test", (uint32_t)state.stencil[0], 1);
    check_u32("clear blend", (uint32_t)state.blend[0], 0);
    check_u32("clear logic", (uint32_t)state.fragment[1] & 1u, 0);
    check_u32("clear mode", (uint32_t)state.fragment[2], 0x500);
}

int main(void) {
    test_direct_classification();
    test_stencil_classification();
    test_state_packing();
    test_clear_packing();
    if (s_failures) {
        fprintf(stderr, "%d Vulkan shader-state test(s) failed\n", s_failures);
        return 1;
    }
    printf("Vulkan shader-state tests passed\n");
    return 0;
}

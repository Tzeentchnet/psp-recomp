#include "../../src/rt/display_state.h"

#include <stdio.h>
#include <stdlib.h>

static void fail(const char *message) {
    fprintf(stderr, "display state test failed: %s\n", message);
    exit(1);
}

static void check_int(const char *name, int actual, int expected) {
    if (actual != expected) {
        fprintf(
            stderr, "display state test failed: %s: got %d, expected %d\n",
            name, actual, expected);
        exit(1);
    }
}

static void check_framebuffer(
    const char *name, const SrDisplayFramebuffer *actual,
    const SrDisplayFramebuffer *expected) {
    if (actual->address != expected->address ||
        actual->stride != expected->stride ||
        actual->format != expected->format) {
        fprintf(
            stderr,
            "display state test failed: %s: got %08x/%u/%u, "
            "expected %08x/%u/%u\n",
            name, actual->address, actual->stride, actual->format,
            expected->address, expected->stride, expected->format);
        exit(1);
    }
}

static void test_immediate_coalescing(void) {
    SrDisplayState state;
    SrDisplayFramebuffer first = { 0x04000000u, 512, 3 };
    SrDisplayFramebuffer second = { 0x04088000u, 512, 3 };
    sr_display_state_init(&state);

    check_int(
        "first immediate flip",
        sr_display_state_set_framebuffer(
            &state, &first, SR_DISPLAY_SETBUF_IMMEDIATE),
        SR_DISPLAY_FLIP);
    check_int(
        "second immediate coalesced",
        sr_display_state_set_framebuffer(
            &state, &second, SR_DISPLAY_SETBUF_IMMEDIATE),
        SR_DISPLAY_NO_FLIP);
    check_framebuffer("immediate current", &state.current, &second);
    check_framebuffer("immediate latched", &state.latched, &second);
    check_int(
        "vblank does not duplicate immediate",
        sr_display_state_begin_vblank(&state), SR_DISPLAY_NO_FLIP);
    sr_display_state_end_vblank(&state);
    check_int(
        "unchanged framebuffer flips next vblank",
        sr_display_state_begin_vblank(&state), SR_DISPLAY_FLIP);
    check_framebuffer("vblank current", &state.current, &second);
}

static void test_nextframe_latching(void) {
    SrDisplayState state;
    SrDisplayFramebuffer first = { 0x04000000u, 512, 0 };
    SrDisplayFramebuffer second = { 0x04044000u, 512, 3 };
    sr_display_state_init(&state);

    check_int(
        "first nextframe deferred",
        sr_display_state_set_framebuffer(
            &state, &first, SR_DISPLAY_SETBUF_NEXTFRAME),
        SR_DISPLAY_NO_FLIP);
    check_int(
        "second nextframe deferred",
        sr_display_state_set_framebuffer(
            &state, &second, SR_DISPLAY_SETBUF_NEXTFRAME),
        SR_DISPLAY_NO_FLIP);
    if (!state.latched_pending) fail("nextframe was not marked pending");
    check_int("current address remains deferred", (int)state.current.address, 0);
    check_int("current stride updates immediately", (int)state.current.stride, 512);
    check_int("current format updates immediately", (int)state.current.format, 3);
    check_framebuffer("latest nextframe wins", &state.latched, &second);
    check_int(
        "nextframe flips at vblank",
        sr_display_state_begin_vblank(&state), SR_DISPLAY_FLIP);
    check_framebuffer("latched becomes current", &state.current, &second);
    if (state.latched_pending) fail("latched flag was not cleared");
}

static void test_latched_flip_after_immediate(void) {
    SrDisplayState state;
    SrDisplayFramebuffer immediate = { 0x04000000u, 512, 3 };
    SrDisplayFramebuffer next = { 0x04088000u, 512, 3 };
    sr_display_state_init(&state);

    check_int(
        "immediate before latch",
        sr_display_state_set_framebuffer(
            &state, &immediate, SR_DISPLAY_SETBUF_IMMEDIATE),
        SR_DISPLAY_FLIP);
    check_int(
        "nextframe after immediate",
        sr_display_state_set_framebuffer(
            &state, &next, SR_DISPLAY_SETBUF_NEXTFRAME),
        SR_DISPLAY_NO_FLIP);
    check_int(
        "latched vblank overrides immediate coalescing",
        sr_display_state_begin_vblank(&state), SR_DISPLAY_FLIP);
    check_framebuffer("latched current after immediate", &state.current, &next);
}

static void test_immediate_updates_pending_latch(void) {
    SrDisplayState state;
    SrDisplayFramebuffer next = { 0x04000000u, 512, 0 };
    SrDisplayFramebuffer immediate = { 0x04044000u, 512, 3 };
    sr_display_state_init(&state);

    sr_display_state_set_framebuffer(
        &state, &next, SR_DISPLAY_SETBUF_NEXTFRAME);
    check_int(
        "immediate while pending",
        sr_display_state_set_framebuffer(
            &state, &immediate, SR_DISPLAY_SETBUF_IMMEDIATE),
        SR_DISPLAY_FLIP);
    if (!state.latched_pending)
        fail("immediate incorrectly cleared pending latch");
    check_framebuffer(
        "immediate updates pending framebuffer", &state.latched, &immediate);
    check_int(
        "pending immediate still flips at vblank",
        sr_display_state_begin_vblank(&state), SR_DISPLAY_FLIP);
}

static void test_invalid_sync(void) {
    SrDisplayState state;
    SrDisplayFramebuffer framebuffer = { 0x04000000u, 512, 3 };
    sr_display_state_init(&state);

    check_int(
        "invalid sync",
        sr_display_state_set_framebuffer(&state, &framebuffer, 2),
        SR_DISPLAY_INVALID_SYNC);
    check_int("invalid sync current address", (int)state.current.address, 0);
    check_int("invalid sync pending", state.latched_pending, 0);
}

int main(void) {
    test_immediate_coalescing();
    test_nextframe_latching();
    test_latched_flip_after_immediate();
    test_immediate_updates_pending_latch();
    test_invalid_sync();
    puts("display state tests passed");
    return 0;
}

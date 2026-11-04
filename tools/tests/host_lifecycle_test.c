#include "host_lifecycle.h"

#include <stdint.h>
#include <stdio.h>

static int s_failures;

static void check_int(
    const char *name, int actual, int expected) {
    if (actual == expected) return;
    fprintf(
        stderr, "FAIL: %s: got %d expected %d\n",
        name, actual, expected);
    s_failures++;
}

static void check_actions(
    const char *name, uint32_t actual, uint32_t expected) {
    if (actual == expected) return;
    fprintf(
        stderr, "FAIL: %s: got 0x%x expected 0x%x\n",
        name, actual, expected);
    s_failures++;
}

int main(void) {
    SrHostLifecycle state;
    sr_host_lifecycle_init(&state, 0, 1, 0);
    check_int(
        "initial input active",
        sr_host_lifecycle_input_active(&state), 1);

    check_actions(
        "focus lost",
        sr_host_lifecycle_apply(
            &state, SR_HOST_FOCUS_LOST),
        SR_HOST_ACTION_CLEAR_INPUT);
    check_int(
        "unfocused input inactive",
        sr_host_lifecycle_input_active(&state), 0);
    check_actions(
        "duplicate focus lost",
        sr_host_lifecycle_apply(
            &state, SR_HOST_FOCUS_LOST), 0);
    check_actions(
        "focus gained",
        sr_host_lifecycle_apply(
            &state, SR_HOST_FOCUS_GAINED),
        SR_HOST_ACTION_REDRAW);
    check_int(
        "focused input active",
        sr_host_lifecycle_input_active(&state), 1);

    check_actions(
        "minimized",
        sr_host_lifecycle_apply(
            &state, SR_HOST_WINDOW_MINIMIZED),
        SR_HOST_ACTION_CLEAR_INPUT);
    check_int(
        "minimized input inactive",
        sr_host_lifecycle_input_active(&state), 0);
    check_actions(
        "restored",
        sr_host_lifecycle_apply(
            &state, SR_HOST_WINDOW_RESTORED),
        SR_HOST_ACTION_REDRAW);

    check_actions(
        "system suspend",
        sr_host_lifecycle_apply(
            &state, SR_HOST_SYSTEM_SUSPEND),
        SR_HOST_ACTION_CLEAR_INPUT |
            SR_HOST_ACTION_AUDIO_SUSPEND);
    check_actions(
        "duplicate system suspend",
        sr_host_lifecycle_apply(
            &state, SR_HOST_SYSTEM_SUSPEND), 0);
    check_int(
        "suspended input inactive",
        sr_host_lifecycle_input_active(&state), 0);
    check_actions(
        "system resume",
        sr_host_lifecycle_apply(
            &state, SR_HOST_SYSTEM_RESUME),
        SR_HOST_ACTION_AUDIO_RESUME |
            SR_HOST_ACTION_REBASE_CLOCK |
            SR_HOST_ACTION_REDRAW);
    check_actions(
        "duplicate system resume",
        sr_host_lifecycle_apply(
            &state, SR_HOST_SYSTEM_RESUME), 0);
    check_int(
        "resumed input active",
        sr_host_lifecycle_input_active(&state), 1);

    sr_host_lifecycle_init(&state, 1, 0, 1);
    check_int(
        "inactive combined state",
        sr_host_lifecycle_input_active(&state), 0);
    check_actions(
        "combined resume",
        sr_host_lifecycle_apply(
            &state, SR_HOST_SYSTEM_RESUME),
        SR_HOST_ACTION_AUDIO_RESUME |
            SR_HOST_ACTION_REBASE_CLOCK |
            SR_HOST_ACTION_REDRAW);
    check_int(
        "focus and minimize still gate input",
        sr_host_lifecycle_input_active(&state), 0);

    if (s_failures) {
        fprintf(
            stderr, "%d lifecycle test(s) failed\n",
            s_failures);
        return 1;
    }
    puts("host lifecycle tests passed");
    return 0;
}

#include "platform.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void *s_main_fiber;
static void *s_fiber_a;
static void *s_fiber_b;
static int s_events[16];
static int s_event_count;
static int s_failures;

#if defined(__APPLE__) && defined(__aarch64__)
int sr_test_arm64_preserved_registers(void *target);
#endif

static void fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    s_failures++;
}

static void record_event(int event) {
    if (s_event_count >= (int)(sizeof(s_events) / sizeof(s_events[0]))) {
        fail("event buffer overflow");
        return;
    }
    s_events[s_event_count++] = event;
}

static void check_stack_alignment(void) {
#if defined(__aarch64__)
    uintptr_t stack_pointer;
    __asm__ volatile("mov %0, sp" : "=r"(stack_pointer));
    if ((stack_pointer & 15u) != 0)
        fail("fiber stack is not 16-byte aligned");
#endif
}

static void fiber_a_proc(void *parameter) {
    if (parameter != (void *)(uintptr_t)0xa1u)
        fail("fiber A parameter");
    check_stack_alignment();
    record_event(1);
    SwitchToFiber(s_fiber_b);
    record_event(3);
    SwitchToFiber(s_main_fiber);
    record_event(5);
    SwitchToFiber(s_fiber_b);
    record_event(7);
    SwitchToFiber(s_main_fiber);
    fail("completed fiber A was resumed");
    abort();
}

static void fiber_b_proc(void *parameter) {
    if (parameter != (void *)(uintptr_t)0xb2u)
        fail("fiber B parameter");
    check_stack_alignment();
    record_event(2);
    SwitchToFiber(s_fiber_a);
    record_event(4);
    SwitchToFiber(s_main_fiber);
    record_event(6);
    SwitchToFiber(s_fiber_a);
    fail("completed fiber B was resumed");
    abort();
}

static void probe_proc(void *parameter) {
    if (parameter != (void *)(uintptr_t)0xc3u)
        fail("register probe parameter");
    check_stack_alignment();
    SwitchToFiber(s_main_fiber);
    fail("register probe was resumed");
    abort();
}

static void check_events(const int *expected, int count) {
    if (s_event_count != count) {
        fail("fiber event count");
        return;
    }
    for (int i = 0; i < count; i++) {
        if (s_events[i] != expected[i]) {
            fail("fiber event order");
            return;
        }
    }
}

int main(void) {
    s_main_fiber = ConvertThreadToFiber(NULL);
    if (!s_main_fiber) {
        perror("ConvertThreadToFiber");
        return 2;
    }
    if (ConvertThreadToFiber(NULL) != s_main_fiber)
        fail("thread conversion is not idempotent");
    SwitchToFiber(s_main_fiber);

    errno = 0;
    if (CreateFiberEx(0, 0, 0, fiber_a_proc, NULL) != NULL ||
        errno != EINVAL)
        fail("zero-size stack was accepted");
    errno = 0;
    if (CreateFiberEx(0, 64u * 1024u, 0, NULL, NULL) != NULL ||
        errno != EINVAL)
        fail("null fiber procedure was accepted");

    void *probe = CreateFiberEx(
        0, 64u * 1024u, 0, probe_proc,
        (void *)(uintptr_t)0xc3u);
    if (!probe) {
        perror("CreateFiberEx probe");
        return 2;
    }
#if defined(__APPLE__) && defined(__aarch64__)
    if (!sr_test_arm64_preserved_registers(probe))
        fail("arm64 nonvolatile register state");
#else
    SwitchToFiber(probe);
#endif
    DeleteFiber(probe);

    s_fiber_a = CreateFiberEx(
        0, 64u * 1024u, 0, fiber_a_proc,
        (void *)(uintptr_t)0xa1u);
    s_fiber_b = CreateFiberEx(
        0, 64u * 1024u, 0, fiber_b_proc,
        (void *)(uintptr_t)0xb2u);
    if (!s_fiber_a || !s_fiber_b) {
        perror("CreateFiberEx nested");
        return 2;
    }

    SwitchToFiber(s_fiber_a);
    {
        static const int expected[] = { 1, 2, 3 };
        check_events(expected, 3);
    }
    SwitchToFiber(s_fiber_b);
    {
        static const int expected[] = { 1, 2, 3, 4 };
        check_events(expected, 4);
    }
    SwitchToFiber(s_fiber_a);
    {
        static const int expected[] = { 1, 2, 3, 4, 5, 6, 7 };
        check_events(expected, 7);
    }

    DeleteFiber(s_fiber_a);
    DeleteFiber(s_fiber_b);
    DeleteFiber(NULL);

    if (s_failures) {
        fprintf(stderr, "%d fiber test(s) failed\n", s_failures);
        return 1;
    }
    puts("fiber tests passed");
    return 0;
}

/* Sub-interrupt handlers (InterruptManager), ported from PPSSPP Core/HLE/sceKernelInterrupt.cpp.
 *
 * Each interrupt (vblank = 30, GE = 25, ...) has up to 32 sub-interrupt slots. A slot exists once
 * it is registered (or enabled before registration) and holds a handler address, its argument and
 * an enabled flag. Release removes the slot, which also disables it.
 *   - Vblank: every enabled slot with a handler runs, in slot order, with a0 = slot number and
 *     a1 = argument (IntrHandler::copyArgsToCPU).
 *   - GE: gelist.c looks the slot up directly and ignores the enabled flag, as PPSSPP's
 *     GeIntrHandler does. sceGeSetCallback registers its slots through sr_intr_set.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_INTR    67u   /* PSP_NUMBER_INTERRUPTS */
#define NUM_SUBINTR 32u   /* PSP_NUMBER_SUBINTERRUPTS */

#define ERR_ILLEGAL_INTRCODE  0x80020065u
#define ERR_FOUND_HANDLER     0x80020067u
#define ERR_NOTFOUND_HANDLER  0x80020068u

typedef struct {
    uint8_t  present, enabled;
    uint32_t addr, arg;
} SubIntr;

static SubIntr s_sub[NUM_INTR][NUM_SUBINTR];

int sr_intr_get(uint32_t intr, uint32_t sub, uint32_t *addr, uint32_t *arg) {
    if (intr >= NUM_INTR || sub >= NUM_SUBINTR || !s_sub[intr][sub].present) return 0;
    if (addr) *addr = s_sub[intr][sub].addr;
    if (arg) *arg = s_sub[intr][sub].arg;
    return 1;
}

/* __RegisterSubIntrHandler: fails with FOUND_HANDLER when a handler is already there. A slot
 * that exists without a handler (enabled first) keeps its enabled flag. */
static uint32_t register_sub(uint32_t intr, uint32_t sub, uint32_t addr, uint32_t arg) {
    SubIntr *h = &s_sub[intr][sub];
    if (h->present) {
        if (h->addr) return ERR_FOUND_HANDLER;
        h->addr = addr; h->arg = arg;
        return 0;
    }
    h->present = 1; h->enabled = 0; h->addr = addr; h->arg = arg;
    return 0;
}

void sr_intr_set(uint32_t intr, uint32_t sub, uint32_t addr, uint32_t arg, int enable) {
    if (intr >= NUM_INTR || sub >= NUM_SUBINTR) return;
    register_sub(intr, sub, addr, arg);
    if (enable) { s_sub[intr][sub].present = 1; s_sub[intr][sub].enabled = 1; }
}

void sr_intr_release(uint32_t intr, uint32_t sub) {
    if (intr < NUM_INTR && sub < NUM_SUBINTR) memset(&s_sub[intr][sub], 0, sizeof(SubIntr));
}

void sr_intr_run_vblank(CpuState *s) {
    for (uint32_t sub = 0; sub < NUM_SUBINTR; sub++) {
        SubIntr *h = &s_sub[SR_INTR_VBLANK][sub];
        if (h->present && h->enabled && h->addr)
            sched_run_interrupt(s, h->addr, sub, h->arg, 0);
    }
}

#define A0 (s->r[4])
#define A1 (s->r[5])
#define A2 (s->r[6])
#define A3 (s->r[7])

static uint32_t h_RegisterSubIntrHandler(CpuState *s) {
    if (A0 >= NUM_INTR || A1 >= NUM_SUBINTR) return ERR_ILLEGAL_INTRCODE;
    return register_sub(A0, A1, A2, A3);
}

static uint32_t h_ReleaseSubIntrHandler(CpuState *s) {
    if (A0 >= NUM_INTR || A1 >= NUM_SUBINTR) return ERR_ILLEGAL_INTRCODE;
    SubIntr *h = &s_sub[A0][A1];
    if (!h->present || !h->addr) return ERR_NOTFOUND_HANDLER;
    memset(h, 0, sizeof(*h));
    return 0;
}

static uint32_t h_EnableSubIntr(CpuState *s) {
    if (A0 >= NUM_INTR || A1 >= NUM_SUBINTR) return ERR_ILLEGAL_INTRCODE;
    SubIntr *h = &s_sub[A0][A1];
    if (!h->present) register_sub(A0, A1, 0, 0);   /* enabling before registering works */
    h->enabled = 1;
    return 0;
}

static uint32_t h_DisableSubIntr(CpuState *s) {
    if (A0 >= NUM_INTR || A1 >= NUM_SUBINTR) return ERR_ILLEGAL_INTRCODE;
    if (s_sub[A0][A1].present) s_sub[A0][A1].enabled = 0;
    return 0;
}

void sr_intr_register(void) {
    sr_hle_register(0xca04a2b9, "sceKernelRegisterSubIntrHandler", h_RegisterSubIntrHandler);
    sr_hle_register(0xd61e6961, "sceKernelReleaseSubIntrHandler", h_ReleaseSubIntrHandler);
    sr_hle_register(0xfb8e22ec, "sceKernelEnableSubIntr", h_EnableSubIntr);
    sr_hle_register(0x8a389411, "sceKernelDisableSubIntr", h_DisableSubIntr);
}

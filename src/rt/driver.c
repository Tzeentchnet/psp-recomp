/* Driver for differential traces and standalone recompiled-image boot.
 *
 * Differential mode seeds the entry state from a PPSSPP trace. Standalone mode maps a
 * pre-relocated image and creates the module_start thread with the state BOOT.BIN and the PSP
 * kernel would provide.
 *
 * Usage: driver <elf> <ref-trace> <out-trace>
 *        driver --image <image.bin> <base-hex> <entry-hex> <ref-trace> <out-trace>
 *        driver --boot <image.bin> <base-hex> <entry-hex> <out-trace>
 */

#define _CRT_SECURE_NO_WARNINGS
#include "graphics_config.h"
#include "graphics_settings.h"
#include "input_config.h"
#include "input_settings.h"
#include "platform.h"
#include "recomp.h"
#include "startup_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifndef SR_GAME_TITLE
#define SR_GAME_TITLE "PSP Recomp"
#endif
#ifndef SR_BOOT_ARG
#define SR_BOOT_ARG "modulename"
#endif
#ifndef SR_BOOT_NEXT_UID
#define SR_BOOT_NEXT_UID 0x11cu
#endif
#if defined(SR_CONFIG_BOOT_IMAGE) && \
    (!defined(SR_CONFIG_BOOT_BASE) || !defined(SR_CONFIG_BOOT_ENTRY))
#error SR_CONFIG_BOOT_IMAGE requires SR_CONFIG_BOOT_BASE and SR_CONFIG_BOOT_ENTRY
#endif

void sr_register_all(void);
#ifdef SR_MODULE_POLICY_INIT
void SR_MODULE_POLICY_INIT(void);
#endif

/* Last-resort crash reporter: a silent host fault (access violation etc.) otherwise kills
 * the process with nothing in the log. Maps a faulting address inside the guest arena back
 * to its guest address. */
#ifdef _WIN32
static LONG WINAPI sr_crash_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    fprintf(stderr, "HOST CRASH: exception 0x%08lx at host %p\n",
            er->ExceptionCode, er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const uint8_t *fa = (const uint8_t *)er->ExceptionInformation[1];
        fprintf(stderr, "  %s of host %p", er->ExceptionInformation[0] ? "write" : "read", fa);
        if (g_mem && fa >= g_mem - 0x04000000 && fa < g_mem + 0x04000000)
            fprintf(stderr, " (guest 0x%08x)", (uint32_t)(fa - g_mem) + 0x08000000u);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;   /* still crash (after reporting) */
}
#endif

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static uint8_t *read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(2); }
    fclose(f);
    *out_len = n;
    return d;
}

static uint32_t load_elf(const uint8_t *elf) {
    if (memcmp(elf, "\x7f""ELF", 4) != 0) { fprintf(stderr, "not an ELF\n"); exit(2); }
    uint32_t e_entry = rd32(elf + 24);
    uint32_t e_phoff = rd32(elf + 28);
    uint16_t phentsize = rd16(elf + 42);
    uint16_t phnum = rd16(elf + 44);
    sr_mem_init();
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = elf + e_phoff + (uint32_t)i * phentsize;
        uint32_t p_type = rd32(ph + 0), p_off = rd32(ph + 4), p_vaddr = rd32(ph + 8);
        uint32_t p_filesz = rd32(ph + 16), p_memsz = rd32(ph + 20);
        if (p_type == 1 && p_filesz)  /* PT_LOAD */
            sr_load_segment(p_vaddr, elf + p_off, p_filesz);
        if (p_type == 1 && p_memsz)   /* BSS is part of the module's memory block */
            sr_note_loaded(p_vaddr, p_memsz);
    }
    return e_entry;
}

static int hexval(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Reads the reference trace's header: "# init" (register state at the first traced step: r, hi,
 * lo, f, fcr31, fpcond, v, vc) and any "# mem <addr> <hex bytes>" lines (guest memory the oracle's
 * kernel prepared, e.g. the start thread's stack and argp copy). Must run after the image is
 * loaded, since the memory lines are written into guest RAM. Stops at the first step line. */
static void seed_from_init(const char *trace_path, CpuState *s) {
    FILE *f = fopen(trace_path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", trace_path); exit(2); }
    static char buf[16384];
    int found = 0, mem_lines = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (buf[0] != '#')
            break;
        if (strncmp(buf, "# mem ", 6) == 0) {
            char *p = buf + 6;
            uint32_t addr = (uint32_t)strtoul(p, &p, 16);
            while (*p == ' ') p++;
            for (; hexval(p[0]) >= 0 && hexval(p[1]) >= 0; p += 2, addr++)
                sr_w8(addr, (uint8_t)(hexval(p[0]) * 16 + hexval(p[1])));
            mem_lines++;
            continue;
        }
        if (strncmp(buf, "# kernel next_uid=", 18) == 0) {
            sr_set_next_uid((uint32_t)strtoul(buf + 18, NULL, 16));
            continue;
        }
        if (strncmp(buf, "# ge next_list_id=", 18) == 0) {
            sr_ge_set_next_list_id((uint32_t)strtoul(buf + 18, NULL, 0));
            continue;
        }
        if (strncmp(buf, "# init", 6) != 0 || found)
            continue;
        found = 1;
        char *tok = strtok(buf + 6, " \t\r\n");
        while (tok) {
            char *eq = strchr(tok, '=');
            if (eq) {
                *eq = 0;
                uint32_t val = (uint32_t)strtoul(eq + 1, NULL, 16);
                if (tok[0] == 'r') s->r[atoi(tok + 1) & 31] = val;
                else if (tok[0] == 'f' && tok[1] >= '0' && tok[1] <= '9') s->fi[atoi(tok + 1) & 31] = val;
                else if (tok[0] == 'v' && tok[1] == 'c') s->vfpuCtrl[atoi(tok + 2) & 15] = val;
                else if (tok[0] == 'v' && tok[1] >= '0' && tok[1] <= '9') s->vi[atoi(tok + 1) & 127] = val;
                else if (strcmp(tok, "hi") == 0) s->hi = val;
                else if (strcmp(tok, "lo") == 0) s->lo = val;
                else if (strcmp(tok, "fcr31") == 0) s->fcr31 = val;
                else if (strcmp(tok, "fpcond") == 0) s->fpcond = val;
            }
            tok = strtok(NULL, " \t\r\n");
        }
    }
    fclose(f);
    if (!found) { fprintf(stderr, "no '# init' in %s\n", trace_path); exit(2); }
    if (mem_lines) fprintf(stderr, "seed: %d memory lines from %s\n", mem_lines, trace_path);
}

#ifdef SR_CONFIG_BOOT_IMAGE
static int executable_sibling_path(
    const char *name, char *path, size_t path_size) {
    char *slash;
    char *forward_slash;
    size_t prefix_length;
    size_t name_length;

    if (!sr_executable_path(path, path_size)) return 0;
    slash = strrchr(path, '\\');
    forward_slash = strrchr(path, '/');
    if (!slash || (forward_slash && forward_slash > slash))
        slash = forward_slash;
    if (!slash) return 0;
    prefix_length = (size_t)(slash - path) + 1;
    name_length = strlen(name);
    if (prefix_length + name_length >= path_size) return 0;
    memcpy(path + prefix_length, name, name_length + 1);
    return 1;
}
#endif

static void launch_error(const char *message) {
#ifdef _WIN32
    MessageBoxA(
        NULL, message, "Cannot Launch Game",
        MB_OK | MB_ICONERROR);
#else
    fprintf(stderr, "Cannot Launch Game: %s\n", message);
#endif
}

int main(int argc, char **argv) {
    uint32_t entry;
    uint32_t boot_arglen = 0, boot_argp = 0;
    const char *ref_trace = NULL, *out;
    int standalone = 0;
    int launch_from_config = 0;

#ifdef _WIN32
    SetUnhandledExceptionFilter(sr_crash_filter);
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--configure-input") == 0) {
            int result;
            if (!sr_input_init()) return 2;
            result = sr_input_settings_show(
                SR_GAME_TITLE " Controller", 0);
            return result < 0 ? 2 : 0;
        }
        if (strcmp(argv[i], "--configure") == 0) {
            int result;
            if (!sr_graphics_config_init() || !sr_input_init()) return 2;
            result = sr_graphics_settings_show(
                SR_GAME_TITLE " Graphics");
            if (result <= 0) return result < 0 ? 2 : 0;
            result = sr_input_settings_show(
                SR_GAME_TITLE " Controller", 1);
            if (result < 0) return 2;
            if (result != SR_INPUT_SETTINGS_LAUNCH) return 0;
#ifdef SR_CONFIG_BOOT_IMAGE
            char image_path[MAX_PATH];
            long len;
            uint8_t *img;
            if (!executable_sibling_path(
                    SR_CONFIG_BOOT_IMAGE,
                    image_path,
                    sizeof(image_path))) {
                launch_error(
                    "Cannot locate the game image beside the executable.");
                return 2;
            }
            if (!sr_startup_validate()) {
                launch_error(
                    "The supported game ISO could not be validated. "
                    "Set PSP_ISO to your disc image and try again.");
                return 2;
            }
            img = read_file(image_path, &len);
            entry = SR_CONFIG_BOOT_ENTRY;
            out = "none";
            standalone = 1;
            launch_from_config = 1;
            sr_mem_init();
            sr_load_segment(SR_CONFIG_BOOT_BASE, img, (uint32_t)len);
            goto have_image;
#else
            launch_error(
                "This build does not define a game image for the launcher.");
            return 2;
#endif
        }
    }

    /* Normal runtime path: the generated image is already rebased and relocated by prxload.py. */
    if (argc >= 6 && strcmp(argv[1], "--boot") == 0) {
        if (!sr_startup_validate()) return 2;
        long len;
        uint8_t *img = read_file(argv[2], &len);
        uint32_t base = (uint32_t)strtoul(argv[3], NULL, 16);
        entry = (uint32_t)strtoul(argv[4], NULL, 16);
        out = argv[5];
        standalone = 1;
        sr_mem_init();
        sr_load_segment(base, img, (uint32_t)len);
        goto have_image;
    }

    /* Image mode: a pre-relocated flat image (e.g. a rebased PRX from tools/prxload.py) is
     * loaded at <base> and run from <entry>. Required for relocatable PRXs, which must be
     * rebased + relocated before they have concrete addresses.
     *   driver --image <image.bin> <base-hex> <entry-hex> <ref-trace> <out-trace>  */
    if (argc >= 7 && strcmp(argv[1], "--image") == 0) {
        long len;
        uint8_t *img = read_file(argv[2], &len);
        uint32_t base = (uint32_t)strtoul(argv[3], NULL, 16);
        entry = (uint32_t)strtoul(argv[4], NULL, 16);
        ref_trace = argv[5];
        out = argv[6];
        sr_mem_init();
        sr_load_segment(base, img, (uint32_t)len);
        goto have_image;
    }
    if (argc < 4) {
        fprintf(stderr, "usage: driver <elf> <ref-trace> <out-trace>\n"
                        "       driver --image <image.bin> <base-hex> <entry-hex> <ref-trace> <out-trace>\n"
                        "       driver --boot <image.bin> <base-hex> <entry-hex> <out-trace>\n"
                        "       driver --configure\n");
        return 2;
    }
    {
        long len;
        uint8_t *elf = read_file(argv[1], &len);
        entry = load_elf(elf);
    }
    ref_trace = argv[2];
    out = argv[3];
have_image:;

    CpuState s;
    memset(&s, 0, sizeof(s));
    s.vfpuCtrl[0] = 0xe4;  /* SPREFIX = identity */
    s.vfpuCtrl[1] = 0xe4;  /* TPREFIX = identity */
    s.vfpuCtrl[2] = 0;     /* DPREFIX = none */
    if (standalone) {
        static const char boot_arg[] = SR_BOOT_ARG;
        boot_arglen = (uint32_t)sizeof(boot_arg);
        boot_argp = 0x08800000u;  /* reserved usersystemlib range; copied before guest code runs */
        for (uint32_t i = 0; i < boot_arglen; i++) sr_w8(boot_argp + i, (uint8_t)boot_arg[i]);
        sr_set_next_uid(SR_BOOT_NEXT_UID);
#ifdef SR_INITIAL_GE_LIST_ID
        sr_ge_set_next_list_id(SR_INITIAL_GE_LIST_ID);
#endif
    } else {
#ifdef SR_INITIAL_GE_LIST_ID
        sr_ge_set_next_list_id(SR_INITIAL_GE_LIST_ID);
#endif
        seed_from_init(ref_trace, &s);
        boot_arglen = s.r[4];
        boot_argp = s.r[5];
    }
    s.pc = entry;

#ifdef SR_MODULE_POLICY_INIT
    SR_MODULE_POLICY_INIT();
#endif
    sr_register_all();
    /* "none" as the out path disables tracing -- much faster for HLE bring-up, where the goal
     * is to reach the next unimplemented import rather than diff a trace. */
    uint32_t trace_start = entry;
    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], "--trace-start=", 14) == 0)
            trace_start = (uint32_t)strtoul(argv[i] + 14, NULL, 16);
    if (strcmp(out, "none") != 0 &&
        sr_trace_open(out, "cgtest", trace_start) != 0) {
        fprintf(stderr, "cannot open out trace %s\n", out);
        return 2;
    }
    RecompFn fn = sr_lookup(entry);
    if (!fn) { fprintf(stderr, "no recompiled function at entry 0x%08x\n", entry); return 2; }

    int use_sched = standalone;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--sched") == 0) use_sched = 1;
#ifdef SR_BOOT_UID_GAP_ENTRY
    if (use_sched && entry == SR_BOOT_UID_GAP_ENTRY)
        sr_set_boot_uid_gap(SR_BOOT_UID_GAP_COUNT);
#endif
    if (launch_from_config) {
        if (!gui_init(SR_GAME_TITLE)) return 2;
        use_sched = 1;
    }
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--gui") == 0) {
        if (!gui_init(SR_GAME_TITLE)) return 2;
        use_sched = 1;
    }

    if (use_sched) {
        /* Run with the cooperative scheduler so the game's threads interleave (the boot busy-
         * waits on a sibling). sched_run uses s as the live register file for whichever thread
         * runs; it returns when no thread is runnable, or the process exits at an unimplemented
         * import inside a thread fiber. */
        sched_init(&s);
        sched_run(entry, boot_arglen, boot_argp, standalone);
    } else if (setjmp(g_hle_jmp) == 0) {
        fn(&s);
    }
    sr_trace_close();
    fprintf(stderr, "done (hit_hle=%d)\n", sr_hit_hle);
    return 0;
}

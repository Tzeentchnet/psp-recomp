/* Interactive front-end: presents through the selected GE backend and feeds keyboard/gamepad
 * input to sceCtrl. The dependency-free Win32/GDI path remains the software fallback. */

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#ifdef SR_SDL3VK
#include "gpu_sdl3vk/sdl3vk.h"
#include "gpu_sdl3vk/ge_gpu.h"
#endif
#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define PSP_W 480
#define PSP_H 272

static int      s_on = 0;
static int      s_sdl3 = 0;            /* SDL3+Vulkan presenter active (src/rt/gpu_sdl3vk) */
#ifdef SR_SDL3VK
static const GeBackend *s_ge_backend;
#endif
static HWND     s_hwnd;
static uint32_t *s_px;                 /* PSP_W*PSP_H BGRA for StretchDIBits */
static BITMAPINFO s_bmi;
static uint32_t s_buttons = 0;
static LARGE_INTEGER s_freq, s_last;   /* frame pacing to ~60 Hz */

/* PSP button bits (sceCtrl): SELECT 0x1, START 0x8, UP 0x10, RIGHT 0x20, DOWN 0x40, LEFT 0x80,
 * LTRIG 0x100, RTRIG 0x200, TRIANGLE 0x1000, CIRCLE 0x2000, CROSS 0x4000, SQUARE 0x8000. */
static uint32_t read_keys(void) {
    uint32_t b = 0;
    #define K(vk,bit) do { if (GetAsyncKeyState(vk) & 0x8000) b |= (bit); } while (0)
    K(VK_RETURN, 0x0008);              /* Enter  -> START  */
    K(VK_RSHIFT, 0x0001); K(VK_LSHIFT, 0x0001); /* Shift -> SELECT */
    K('X', 0x4000);                    /* X -> CROSS   (confirm) */
    K('Z', 0x2000);                    /* Z -> CIRCLE  (back)    */
    K('A', 0x8000);                    /* A -> SQUARE  */
    K('S', 0x1000);                    /* S -> TRIANGLE */
    K('Q', 0x0100);                    /* Q -> L */
    K('W', 0x0200);                    /* W -> R */
    K(VK_UP, 0x0010); K(VK_DOWN, 0x0040); K(VK_LEFT, 0x0080); K(VK_RIGHT, 0x0020);
    #undef K
    return b;
}

/* ---- Gamepad support (XInput + DirectInput) ---------------------------------------------------
 * Generic controller support without external deps. XInput (loaded dynamically) covers Xbox pads
 * and the many third-party / PS-style pads that present an XInput interface (incl. PS4 via Steam
 * Input or DS4Windows). DirectInput covers a DualShock 4 / generic HID gamepad connected directly.
 * Both feed the same PSP pad mask plus the analog left stick (Lx/Ly, 0..255, 128=centre). */
static uint8_t s_lx = 128, s_ly = 128;     /* live left-stick, latched each present */
static int s_pad_present = 0;               /* a controller is currently connected */

/* XInput (dynamically loaded so we don't hard-depend on a specific xinput*.dll at link time). */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XIGAMEPAD;
typedef struct { DWORD dwPacketNumber; XIGAMEPAD Gamepad; } XISTATE;
typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XISTATE *);
static PFN_XInputGetState s_xiGetState;

static IDirectInput8A     *s_di;
static IDirectInputDevice8A *s_dijoy;

static BOOL CALLBACK di_enum_cb(const DIDEVICEINSTANCEA *inst, void *ctx) {
    (void)ctx;
    if (FAILED(IDirectInput8_CreateDevice(s_di, &inst->guidInstance, &s_dijoy, NULL)))
        return DIENUM_CONTINUE;
    return DIENUM_STOP;   /* take the first attached game controller */
}

static void pad_init(void) {
    const char *dll[3] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !s_xiGetState; i++) {
        HMODULE h = LoadLibraryA(dll[i]);
        if (h) s_xiGetState = (PFN_XInputGetState)(void *)GetProcAddress(h, "XInputGetState");
    }
    if (SUCCEEDED(DirectInput8Create(GetModuleHandleA(0), DIRECTINPUT_VERSION, &IID_IDirectInput8A,
                                     (void **)&s_di, NULL))) {
        IDirectInput8_EnumDevices(s_di, DI8DEVCLASS_GAMECTRL, di_enum_cb, NULL, DIEDFL_ATTACHEDONLY);
        if (s_dijoy) {
            IDirectInputDevice8_SetDataFormat(s_dijoy, &c_dfDIJoystick2);
            IDirectInputDevice8_SetCooperativeLevel(s_dijoy, s_hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
            /* Normalise the main axes to 0..65535 so centre is ~32767. */
            for (int axis = 0; axis < 2; axis++) {
                DIPROPRANGE pr; pr.diph.dwSize = sizeof(pr); pr.diph.dwHeaderSize = sizeof(pr.diph);
                pr.diph.dwObj = (DWORD)(axis == 0 ? DIJOFS_X : DIJOFS_Y); pr.diph.dwHow = DIPH_BYOFFSET;
                pr.lMin = 0; pr.lMax = 65535;
                IDirectInputDevice8_SetProperty(s_dijoy, DIPROP_RANGE, &pr.diph);
            }
            IDirectInputDevice8_Acquire(s_dijoy);
        }
    }
    fprintf(stderr, "pad_init: XInput=%s DirectInput=%s\n",
            s_xiGetState ? "ok" : "none", s_dijoy ? "ok" : "none");
}

static uint8_t axis_to_psp(int centred /* -32768..32767 */, int deadzone) {
    if (centred > -deadzone && centred < deadzone) return 128;
    int u = (centred + 32768) * 255 / 65535;
    return (uint8_t)(u < 0 ? 0 : u > 255 ? 255 : u);
}

/* Poll connected pads. Returns the PSP button mask and (via lx/ly) the analog left stick. */
static uint32_t read_pad(uint8_t *lx, uint8_t *ly) {
    uint32_t b = 0; int have_stick = 0; int present = 0;
    if (s_xiGetState) {
        XISTATE st;
        if (s_xiGetState(0, &st) == 0 /* ERROR_SUCCESS */) {
            present = 1;
            WORD w = st.Gamepad.wButtons;
            if (w & 0x1000) b |= 0x4000;   /* A -> CROSS    */
            if (w & 0x2000) b |= 0x2000;   /* B -> CIRCLE   */
            if (w & 0x4000) b |= 0x8000;   /* X -> SQUARE   */
            if (w & 0x8000) b |= 0x1000;   /* Y -> TRIANGLE */
            if (w & 0x0010) b |= 0x0008;   /* Start -> START  */
            if (w & 0x0020) b |= 0x0001;   /* Back  -> SELECT */
            if (w & 0x0100) b |= 0x0100;   /* LB -> L */
            if (w & 0x0200) b |= 0x0200;   /* RB -> R */
            if (st.Gamepad.bLeftTrigger  > 64) b |= 0x0100;
            if (st.Gamepad.bRightTrigger > 64) b |= 0x0200;
            if (w & 0x0001) b |= 0x0010;   /* DPad up    */
            if (w & 0x0002) b |= 0x0040;   /* DPad down  */
            if (w & 0x0004) b |= 0x0080;   /* DPad left  */
            if (w & 0x0008) b |= 0x0020;   /* DPad right */
            *lx = axis_to_psp(st.Gamepad.sThumbLX, 7849);
            *ly = axis_to_psp(-st.Gamepad.sThumbLY, 7849);   /* PSP Y: up=0, down=255 */
            have_stick = 1;
        }
    }
    if (s_dijoy) {
        DIJOYSTATE2 js;
        HRESULT hr = IDirectInputDevice8_Poll(s_dijoy);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) IDirectInputDevice8_Acquire(s_dijoy);
        if (SUCCEEDED(IDirectInputDevice8_GetDeviceState(s_dijoy, sizeof(js), &js))) {
            present = 1;
            const unsigned char *bt = js.rgbButtons;     /* DualShock4 / generic HID order */
            if (bt[1] & 0x80) b |= 0x4000;   /* Cross    -> CROSS    */
            if (bt[2] & 0x80) b |= 0x2000;   /* Circle   -> CIRCLE   */
            if (bt[0] & 0x80) b |= 0x8000;   /* Square   -> SQUARE   */
            if (bt[3] & 0x80) b |= 0x1000;   /* Triangle -> TRIANGLE */
            if (bt[4] & 0x80) b |= 0x0100;   /* L1 -> L */
            if (bt[5] & 0x80) b |= 0x0200;   /* R1 -> R */
            if (bt[6] & 0x80) b |= 0x0100;   /* L2 -> L */
            if (bt[7] & 0x80) b |= 0x0200;   /* R2 -> R */
            if (bt[8] & 0x80) b |= 0x0001;   /* Share/Select -> SELECT */
            if (bt[9] & 0x80) b |= 0x0008;   /* Options/Start -> START */
            DWORD pov = js.rgdwPOV[0];
            if ((pov & 0xFFFF) != 0xFFFF) {  /* hat: centidegrees, 0=up clockwise */
                if (pov > 27000 || pov < 9000)  b |= 0x0010;   /* up    */
                if (pov > 9000  && pov < 27000) b |= 0x0040;   /* down  */
                if (pov > 18000)                 b |= 0x0080;   /* left  */
                if (pov > 0 && pov < 18000)      b |= 0x0020;   /* right */
            }
            if (!have_stick) {               /* prefer XInput stick if both present */
                *lx = axis_to_psp((int)js.lX - 32768, 6000);
                *ly = axis_to_psp((int)js.lY - 32768, 6000);
            }
        }
    }
    s_pad_present = present;
    return b;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

int gui_init(const char *title) {
    const char *requested = getenv("SR_GPU");
    int explicit_gpu = requested && requested[0];
    int want_vk = 0;
    if (explicit_gpu) {
        if (strcmp(requested, "vk") == 0) {
            want_vk = 1;
        } else if (strcmp(requested, "sw") == 0) {
            want_vk = 0;
        } else if (strcmp(requested, "d3d12") == 0) {
            fprintf(stderr, "gui_init: SR_GPU=d3d12 is not available in this build\n");
            return 0;
        } else {
            fprintf(stderr, "gui_init: invalid SR_GPU=%s (expected d3d12, vk, or sw)\n", requested);
            return 0;
        }
    } else {
        const char *legacy = getenv("SR_GPU_GE");
        want_vk = legacy && legacy[0] && strcmp(legacy, "0") != 0;
    }

#ifdef SR_SDL3VK
    /* SDL3+Vulkan presentation remains the default in this build even for SR_GPU=sw.
     * SR_VIDEO=gdi is retained as a legacy presenter override. */
    {
        const char *v = getenv("SR_VIDEO");
        if (want_vk && v && strcmp(v, "gdi") == 0) {
            fprintf(stderr, "gui_init: SR_GPU=vk cannot be combined with SR_VIDEO=gdi\n");
            return 0;
        }
        if (!v || strcmp(v, "gdi") != 0) {
            if (sdl3vk_init(title)) {
                s_sdl3 = 1;
                s_px = (uint32_t *)malloc(PSP_W * PSP_H * 4);
                if (!s_px) {
                    fprintf(stderr, "gui_init: framebuffer conversion buffer allocation failed\n");
                    sdl3vk_shutdown();
                    s_sdl3 = 0;
                    return 0;
                }
                QueryPerformanceFrequency(&s_freq);
                QueryPerformanceCounter(&s_last);
                s_on = 1;
                if (want_vk) {
                    s_ge_backend = gegpu_backend();
                    if (!s_ge_backend || !s_ge_backend->init || !s_ge_backend->init()) {
                        fprintf(stderr, "gui_init: Vulkan GE backend initialization failed\n");
                        sdl3vk_shutdown();
                        s_sdl3 = 0;
                        s_on = 0;
                        free(s_px);
                        s_px = NULL;
                        s_ge_backend = NULL;
                        return 0;
                    }
                    ge_set_backend(s_ge_backend);
                }
                fprintf(stderr, "gui_init: GE backend %s\n",
                        s_ge_backend ? s_ge_backend->name : "sw");
                return 1;
            }
            if (want_vk) {
                fprintf(stderr, "gui_init: SR_GPU=vk requires a working SDL3/Vulkan presenter\n");
                return 0;
            }
            fprintf(stderr, "gui_init: SDL3/Vulkan init failed; falling back to GDI\n");
        }
    }
#else
    if (want_vk) {
        fprintf(stderr, "gui_init: SR_GPU=vk is not available in this build\n");
        return 0;
    }
#endif
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "psp_recomp";
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, PSP_W * 2, PSP_H * 2};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s_hwnd = CreateWindowA("psp_recomp", title ? title : "PSP Recomp",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, 0, 0, GetModuleHandleA(0), 0);
    s_px = (uint32_t *)malloc(PSP_W * PSP_H * 4);
    if (!s_hwnd || !s_px) {
        fprintf(stderr, "gui_init: GDI window initialization failed\n");
        if (s_hwnd) DestroyWindow(s_hwnd);
        free(s_px);
        s_px = NULL;
        return 0;
    }
    s_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    s_bmi.bmiHeader.biWidth = PSP_W;
    s_bmi.bmiHeader.biHeight = -PSP_H;          /* top-down */
    s_bmi.bmiHeader.biPlanes = 1;
    s_bmi.bmiHeader.biBitCount = 32;
    s_bmi.bmiHeader.biCompression = BI_RGB;
    QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&s_last);
    s_on = 1;
    pad_init();                            /* XInput / DirectInput controllers */
    fprintf(stderr, "gui_init: GE backend sw\n");
    return 1;
}

int gui_on(void) { return s_on; }
uint32_t gui_buttons(void) { return s_buttons; }
void gui_analog(uint8_t *lx, uint8_t *ly) { if (lx) *lx = s_lx; if (ly) *ly = s_ly; }
int gui_pad_present(void) { return s_pad_present; }

/* Present a framebuffer at guest address fbaddr. fmt: 0=5650, 1=5551, 2=4444, 3=8888.
 * stride is in pixels (PSP buffer width, typically 512). */
/* Convert the guest framebuffer to the BGRA words both presenters consume. */
static void convert_fb(uint32_t fbaddr, int fmt, uint32_t stride) {
    for (int y = 0; y < PSP_H; y++) {
        for (int x = 0; x < PSP_W; x++) {
            uint32_t i = (uint32_t)(y * (int)stride + x);
            int rr, gg, bb;
            if (fmt == 3) {
                uint32_t p = sr_r32(fbaddr + i * 4);
                rr = p & 0xFF; gg = (p >> 8) & 0xFF; bb = (p >> 16) & 0xFF;
            } else {
                uint16_t p = sr_r16(fbaddr + i * 2);
                if (fmt == 1) {            /* 5551 */
                    rr = (p & 0x1F) << 3; gg = ((p >> 5) & 0x1F) << 3; bb = ((p >> 10) & 0x1F) << 3;
                } else if (fmt == 2) {     /* 4444 */
                    rr = (p & 0xF) << 4; gg = ((p >> 4) & 0xF) << 4; bb = ((p >> 8) & 0xF) << 4;
                } else {                   /* 5650 */
                    rr = (p & 0x1F) << 3; gg = ((p >> 5) & 0x3F) << 2; bb = ((p >> 11) & 0x1F) << 3;
                }
            }
            s_px[y * PSP_W + x] = ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
        }
    }
}

void gui_present(uint32_t fbaddr, int fmt, uint32_t stride) {
    if (!s_on) return;
    if (stride == 0) stride = 512;

#ifdef SR_SDL3VK
    if (s_sdl3) {
        /* GPU-resident framebuffer: blit straight from the GE's target image. Falls back
         * to the guest-VRAM convert for CPU-written frames (movies) or pure software GE. */
        int shown = s_ge_backend && s_ge_backend->present
            ? s_ge_backend->present(fbaddr, fmt, stride) : -1;
        if (shown < 0) {
            convert_fb(fbaddr, fmt, stride);
            shown = sdl3vk_present_rgba(s_px);
        }
        if (shown == 0) {
            if (s_ge_backend && s_ge_backend->shutdown) s_ge_backend->shutdown();
            ge_set_backend(NULL);
            s_ge_backend = NULL;
            sdl3vk_shutdown();
            _Exit(0);
        }
        s_buttons = sdl3vk_buttons();
        sdl3vk_analog(&s_lx, &s_ly);
        s_pad_present = sdl3vk_pad_present();
        goto pace;
    }
#endif

    MSG msg;
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { _Exit(0); }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    uint8_t lx = 128, ly = 128;
    s_buttons = read_keys() | read_pad(&lx, &ly);
    s_lx = lx; s_ly = ly;

    convert_fb(fbaddr, fmt, stride);
    HDC dc = GetDC(s_hwnd);
    RECT cr; GetClientRect(s_hwnd, &cr);
    StretchDIBits(dc, 0, 0, cr.right, cr.bottom, 0, 0, PSP_W, PSP_H,
                  s_px, &s_bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(s_hwnd, dc);

#ifdef SR_SDL3VK
pace:
#endif
    /* Pace to ~60 Hz so the intro/menus play at a watchable speed. Skipped when the scheduler
     * already paces vblanks to real time (the default): pacing twice on unaligned grids makes
     * frames miss their vblank and costs a whole extra period (30/20 fps quantization). */
    {
        extern int sched_vbl_paced(void);
        if (sched_vbl_paced()) { QueryPerformanceCounter(&s_last); return; }
    }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt = (double)(now.QuadPart - s_last.QuadPart) / (double)s_freq.QuadPart;
    double target = 1.0 / 60.0;
    if (dt < target) {
        DWORD ms = (DWORD)((target - dt) * 1000.0);
        if (ms > 0 && ms < 100) Sleep(ms);
    }
    QueryPerformanceCounter(&s_last);
}
#endif

/* Interactive front-end: presents through the selected GE backend and feeds keyboard, mouse,
 * and gamepad input to sceCtrl. The dependency-free Win32/GDI path remains the software fallback. */

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#include "graphics_config.h"
#include "graphics_settings.h"
#include "input_config.h"
#include "input_settings.h"
#include "fps_counter.h"
#ifdef SR_SDL3VK
#include "gpu_sdl3vk/sdl3vk.h"
#include "gpu_sdl3vk/ge_gpu.h"
#endif
#ifdef SR_D3D12
#include "gpu_d3d12/device.h"
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

#ifndef SR_GAME_TITLE
#define SR_GAME_TITLE "PSP Recomp"
#endif

static int      s_on = 0;
#ifdef SR_SDL3VK
static int      s_sdl3 = 0;            /* SDL3+Vulkan presenter active (src/rt/gpu_sdl3vk) */
#endif
#ifdef SR_D3D12
static int      s_d3d12 = 0;
#endif
#if defined(SR_SDL3VK) || defined(SR_D3D12)
static const GeBackend *s_ge_backend;
#endif
static HWND     s_hwnd;
static uint32_t *s_px;                 /* PSP_W*PSP_H BGRA for StretchDIBits */
static BITMAPINFO s_bmi;
static uint32_t s_buttons = 0;
static LARGE_INTEGER s_freq, s_last;   /* frame pacing to ~60 Hz */
static SrFpsCounter s_fps_counter;
static int s_fps_counter_enabled = -1;
static char s_window_title[128] = "PSP Recomp";

static void set_window_title(const char *title) {
#ifdef SR_D3D12
    if (s_d3d12) {
        ged3d12_set_title(title);
        return;
    }
#endif
#ifdef SR_SDL3VK
    if (s_sdl3) {
        sdl3vk_set_title(title);
        return;
    }
#endif
    if (s_hwnd) SetWindowTextA(s_hwnd, title);
}

static void update_fps_counter(void) {
    if (s_fps_counter_enabled < 0)
        s_fps_counter_enabled = getenv("SR_FPS_COUNTER") ? 1 : 0;
    if (!s_fps_counter_enabled || !s_freq.QuadPart) return;
    LARGE_INTEGER now;
    double fps;
    double frame_ms;
    QueryPerformanceCounter(&now);
    if (!sr_fps_counter_tick(
            &s_fps_counter, (uint64_t)now.QuadPart,
            (uint64_t)s_freq.QuadPart, 0.5, &fps, &frame_ms))
        return;
    char title[256];
    snprintf(
        title, sizeof(title), "%s | %.1f FPS | %.2f ms",
        s_window_title, fps, frame_ms);
    set_window_title(title);
}

static int win_key_code(int key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return key;
    switch (key) {
    case SR_KEY_ENTER: return VK_RETURN;
    case SR_KEY_LEFT_SHIFT: return VK_LSHIFT;
    case SR_KEY_RIGHT_SHIFT: return VK_RSHIFT;
    case SR_KEY_UP: return VK_UP;
    case SR_KEY_RIGHT: return VK_RIGHT;
    case SR_KEY_DOWN: return VK_DOWN;
    case SR_KEY_LEFT: return VK_LEFT;
    case SR_KEY_SPACE: return VK_SPACE;
    case SR_KEY_TAB: return VK_TAB;
    case SR_KEY_BACKSPACE: return VK_BACK;
    case SR_KEY_LEFT_CTRL: return VK_LCONTROL;
    case SR_KEY_RIGHT_CTRL: return VK_RCONTROL;
    default: return 0;
    }
}

static int win_key_down(void *context, int key) {
    (void)context;
    int vk = win_key_code(key);
    return vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static int win_mouse_down(void *context, int button) {
    (void)context;
    int vk = 0;
    switch (button) {
    case SR_MOUSE_LEFT: vk = VK_LBUTTON; break;
    case SR_MOUSE_RIGHT: vk = VK_RBUTTON; break;
    case SR_MOUSE_MIDDLE: vk = VK_MBUTTON; break;
    case SR_MOUSE_X1: vk = VK_XBUTTON1; break;
    case SR_MOUSE_X2: vk = VK_XBUTTON2; break;
    default: return 0;
    }
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/* ---- Gamepad support (XInput + DirectInput) ---------------------------------------------------
 * Generic controller support without external deps. XInput (loaded dynamically) covers Xbox pads
 * and the many third-party / PS-style pads that present an XInput interface (incl. PS4 via Steam
 * Input or DS4Windows). DirectInput covers a DualShock 4 / generic HID gamepad connected directly.
 * Both feed the same PSP pad mask plus movement and aim sticks. */
static uint8_t s_lx = 128, s_ly = 128;     /* live left-stick, latched each present */
static int s_pad_present = 0;               /* a controller is currently connected */

/* XInput (dynamically loaded so we don't hard-depend on a specific xinput*.dll at link time). */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XIGAMEPAD;
typedef struct { DWORD dwPacketNumber; XIGAMEPAD Gamepad; } XISTATE;
typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XISTATE *);
static PFN_XInputGetState s_xiGetState;

static IDirectInput8A     *s_di;
static IDirectInputDevice8A *s_dijoy;
static int s_di_aim_axes;

typedef struct PadState {
    uint8_t down[SR_PAD_RIGHT_STICK + 1];
    int movement_x, movement_y;
    int aim_x, aim_y;
    int has_movement_stick;
    int has_aim_stick;
    int present;
} PadState;

static int pad_down(void *context, int button) {
    PadState *state = (PadState *)context;
    return button > 0 && button <= SR_PAD_RIGHT_STICK && state->down[button];
}

static BOOL CALLBACK di_enum_cb(const DIDEVICEINSTANCEA *inst, void *ctx) {
    (void)ctx;
    if (FAILED(IDirectInput8_CreateDevice(s_di, &inst->guidInstance, &s_dijoy, NULL)))
        return DIENUM_CONTINUE;
    return DIENUM_STOP;   /* take the first attached game controller */
}

static int di_set_axis_range(DWORD offset) {
    DIPROPRANGE range;
    memset(&range, 0, sizeof(range));
    range.diph.dwSize = sizeof(range);
    range.diph.dwHeaderSize = sizeof(range.diph);
    range.diph.dwObj = offset;
    range.diph.dwHow = DIPH_BYOFFSET;
    range.lMin = 0;
    range.lMax = 65535;
    return SUCCEEDED(
        IDirectInputDevice8_SetProperty(
            s_dijoy, DIPROP_RANGE, &range.diph));
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
            di_set_axis_range(DIJOFS_X);
            di_set_axis_range(DIJOFS_Y);
            int has_z = di_set_axis_range(DIJOFS_Z);
            int has_rz = di_set_axis_range(DIJOFS_RZ);
            int has_rx = di_set_axis_range(DIJOFS_RX);
            int has_ry = di_set_axis_range(DIJOFS_RY);
            s_di_aim_axes =
                has_z && has_rz ? 1 : has_rx && has_ry ? 2 : 0;
            IDirectInputDevice8_Acquire(s_dijoy);
        }
    }
    fprintf(stderr, "pad_init: XInput=%s DirectInput=%s\n",
            s_xiGetState ? "ok" : "none", s_dijoy ? "ok" : "none");
}

static void read_pad(PadState *state) {
    memset(state, 0, sizeof(*state));
    if (s_xiGetState) {
        XISTATE st;
        if (s_xiGetState(0, &st) == 0 /* ERROR_SUCCESS */) {
            state->present = 1;
            WORD w = st.Gamepad.wButtons;
            state->down[SR_PAD_SOUTH] = (w & 0x1000) != 0;
            state->down[SR_PAD_EAST] = (w & 0x2000) != 0;
            state->down[SR_PAD_WEST] = (w & 0x4000) != 0;
            state->down[SR_PAD_NORTH] = (w & 0x8000) != 0;
            state->down[SR_PAD_START] = (w & 0x0010) != 0;
            state->down[SR_PAD_BACK] = (w & 0x0020) != 0;
            state->down[SR_PAD_LEFT_SHOULDER] = (w & 0x0100) != 0;
            state->down[SR_PAD_RIGHT_SHOULDER] = (w & 0x0200) != 0;
            state->down[SR_PAD_LEFT_TRIGGER] = st.Gamepad.bLeftTrigger > 64;
            state->down[SR_PAD_RIGHT_TRIGGER] = st.Gamepad.bRightTrigger > 64;
            state->down[SR_PAD_DPAD_UP] = (w & 0x0001) != 0;
            state->down[SR_PAD_DPAD_DOWN] = (w & 0x0002) != 0;
            state->down[SR_PAD_DPAD_LEFT] = (w & 0x0004) != 0;
            state->down[SR_PAD_DPAD_RIGHT] = (w & 0x0008) != 0;
            state->movement_x = st.Gamepad.sThumbLX;
            state->movement_y =
                st.Gamepad.sThumbLY == -32768
                    ? 32767
                    : -(int)st.Gamepad.sThumbLY;
            state->aim_x = st.Gamepad.sThumbRX;
            state->aim_y =
                st.Gamepad.sThumbRY == -32768
                    ? 32767
                    : -(int)st.Gamepad.sThumbRY;
            state->has_movement_stick = 1;
            state->has_aim_stick = 1;
        }
    }
    if (s_dijoy) {
        DIJOYSTATE2 js;
        HRESULT hr = IDirectInputDevice8_Poll(s_dijoy);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) IDirectInputDevice8_Acquire(s_dijoy);
        if (SUCCEEDED(IDirectInputDevice8_GetDeviceState(s_dijoy, sizeof(js), &js))) {
            state->present = 1;
            const unsigned char *bt = js.rgbButtons;     /* DualShock4 / generic HID order */
            state->down[SR_PAD_SOUTH] |= (bt[1] & 0x80) != 0;
            state->down[SR_PAD_EAST] |= (bt[2] & 0x80) != 0;
            state->down[SR_PAD_WEST] |= (bt[0] & 0x80) != 0;
            state->down[SR_PAD_NORTH] |= (bt[3] & 0x80) != 0;
            state->down[SR_PAD_LEFT_SHOULDER] |= (bt[4] & 0x80) != 0;
            state->down[SR_PAD_RIGHT_SHOULDER] |= (bt[5] & 0x80) != 0;
            state->down[SR_PAD_LEFT_TRIGGER] |= (bt[6] & 0x80) != 0;
            state->down[SR_PAD_RIGHT_TRIGGER] |= (bt[7] & 0x80) != 0;
            state->down[SR_PAD_BACK] |= (bt[8] & 0x80) != 0;
            state->down[SR_PAD_START] |= (bt[9] & 0x80) != 0;
            DWORD pov = js.rgdwPOV[0];
            if ((pov & 0xFFFF) != 0xFFFF) {  /* hat: centidegrees, 0=up clockwise */
                state->down[SR_PAD_DPAD_UP] |= pov >= 31500 || pov <= 4500;
                state->down[SR_PAD_DPAD_RIGHT] |= pov >= 4500 && pov <= 13500;
                state->down[SR_PAD_DPAD_DOWN] |= pov >= 13500 && pov <= 22500;
                state->down[SR_PAD_DPAD_LEFT] |= pov >= 22500 && pov <= 31500;
            }
            if (!state->has_movement_stick) {
                state->movement_x = (int)js.lX - 32768;
                state->movement_y = (int)js.lY - 32768;
                state->has_movement_stick = 1;
            }
            if (!state->has_aim_stick && s_di_aim_axes) {
                if (s_di_aim_axes == 1) {
                    state->aim_x = (int)js.lZ - 32768;
                    state->aim_y = (int)js.lRz - 32768;
                } else {
                    state->aim_x = (int)js.lRx - 32768;
                    state->aim_y = (int)js.lRy - 32768;
                }
                state->has_aim_stick = 1;
            }
        }
    }
    s_pad_present = state->present;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

int gui_init(const char *title) {
    const SrGraphicsConfig *graphics;
    const char *environment;
    const char *requested;
    int explicit_gpu;
    snprintf(
        s_window_title, sizeof(s_window_title), "%s",
        title && title[0] ? title : "PSP Recomp");
    sr_fps_counter_reset(&s_fps_counter);
    s_fps_counter_enabled = -1;
    if (!sr_graphics_config_init()) {
        fprintf(stderr, "gui_init: graphics configuration failed\n");
        return 0;
    }
    if (!sr_input_init()) {
        fprintf(stderr, "gui_init: input configuration failed\n");
        return 0;
    }
    if (sr_graphics_config_first_run()) {
        int result = sr_graphics_settings_show(
            SR_GAME_TITLE " Graphics");
        if (result <= 0) {
            if (!result)
                fprintf(stderr, "gui_init: first-run graphics setup cancelled\n");
            return 0;
        }
        result = sr_input_settings_show(
            SR_GAME_TITLE " Controller", 1);
        if (result <= 0) {
            if (!result)
                fprintf(
                    stderr,
                    "gui_init: first-run controller setup cancelled\n");
            return 0;
        }
    }
    if (!sr_input_start_hot_reload()) {
        fprintf(stderr, "gui_init: input hot reload watcher failed\n");
        return 0;
    }
    graphics = sr_graphics_config_get();
    environment = getenv("SR_GPU");
    requested = environment;
    explicit_gpu = requested && requested[0];
    if (!explicit_gpu && graphics &&
        graphics->renderer != SR_GRAPHICS_RENDERER_AUTO) {
        requested = sr_graphics_renderer_name(graphics->renderer);
        explicit_gpu = requested && requested[0];
    }
    int want_vk = 0;
    int want_d3d12 = 0;
    if (explicit_gpu) {
        if (strcmp(requested, "vk") == 0) {
            want_vk = 1;
        } else if (strcmp(requested, "sw") == 0) {
            want_vk = 0;
        } else if (strcmp(requested, "d3d12") == 0) {
            want_d3d12 = 1;
        } else {
            fprintf(
                stderr,
                "gui_init: invalid renderer %s "
                "(expected d3d12, vk, or sw)\n",
                requested);
            return 0;
        }
    } else {
        const char *legacy = getenv("SR_GPU_GE");
        if (legacy) {
            want_vk = legacy[0] && strcmp(legacy, "0") != 0;
#ifdef SR_D3D12
        } else {
            want_d3d12 = 1;
#endif
        }
    }

#ifdef SR_D3D12
    if (want_d3d12) {
        const char *video = getenv("SR_VIDEO");
        if (video && strcmp(video, "gdi") == 0) {
            fprintf(stderr, "gui_init: SR_GPU=d3d12 cannot be combined with SR_VIDEO=gdi\n");
            return 0;
        }
        if (!ged3d12_window_init(title)) {
            fprintf(stderr, "gui_init: D3D12 SDL3 window initialization failed\n");
            return 0;
        }
        s_ge_backend = ged3d12_backend();
        if (!s_ge_backend || !s_ge_backend->init || !s_ge_backend->init()) {
            fprintf(stderr, "gui_init: D3D12 GE backend initialization failed\n");
            if (s_ge_backend && s_ge_backend->shutdown) s_ge_backend->shutdown();
            else ged3d12_window_shutdown();
            s_ge_backend = NULL;
            return 0;
        }
        ge_set_backend(s_ge_backend);
        s_d3d12 = 1;
        QueryPerformanceFrequency(&s_freq);
        QueryPerformanceCounter(&s_last);
        s_on = 1;
        fprintf(stderr, "gui_init: GE backend %s\n", s_ge_backend->name);
        return 1;
    }
#else
    if (want_d3d12) {
        fprintf(stderr, "gui_init: SR_GPU=d3d12 is not available in this build\n");
        return 0;
    }
#endif

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

void gui_shutdown(void) {
    sr_input_stop_hot_reload();
#ifdef SR_D3D12
    if (s_d3d12) {
        ge_set_backend(NULL);
        if (s_ge_backend && s_ge_backend->shutdown)
            s_ge_backend->shutdown();
        else
            ged3d12_window_shutdown();
        s_ge_backend = NULL;
        s_d3d12 = 0;
    }
#endif
#ifdef SR_SDL3VK
    if (s_sdl3) {
        ge_set_backend(NULL);
        if (s_ge_backend && s_ge_backend->shutdown)
            s_ge_backend->shutdown();
        s_ge_backend = NULL;
        sdl3vk_shutdown();
        s_sdl3 = 0;
    }
#endif
    if (s_dijoy) {
        IDirectInputDevice8_Unacquire(s_dijoy);
        IDirectInputDevice8_Release(s_dijoy);
        s_dijoy = NULL;
    }
    if (s_di) {
        IDirectInput8_Release(s_di);
        s_di = NULL;
    }
    if (s_hwnd) {
        DestroyWindow(s_hwnd);
        s_hwnd = NULL;
    }
    free(s_px);
    s_px = NULL;
    s_buttons = 0;
    s_lx = s_ly = 128;
    s_pad_present = 0;
    s_on = 0;
    s_fps_counter_enabled = -1;
    sr_fps_counter_reset(&s_fps_counter);
}

int gui_on(void) { return s_on; }
uint32_t gui_buttons(void) { return s_buttons; }
void gui_analog(uint8_t *lx, uint8_t *ly) { if (lx) *lx = s_lx; if (ly) *ly = s_ly; }
int gui_pad_present(void) { return s_pad_present; }
int gui_message(
    const char *text, int yes_no, int allow_cancel, int default_no) {
    int fallback = yes_no && default_no ? 2 : 1;
    if (!s_on || getenv("SR_NO_DIALOGS")) return fallback;
    UINT flags = MB_ICONINFORMATION | MB_SETFOREGROUND;
    if (yes_no)
        flags |= allow_cancel ? MB_YESNOCANCEL : MB_YESNO;
    else
        flags |= MB_OK;
    if (yes_no && default_no) flags |= MB_DEFBUTTON2;
    int result = MessageBoxA(
        s_hwnd, text && text[0] ? text : SR_GAME_TITLE,
        SR_GAME_TITLE, flags);
    if (result == IDNO) return 2;
    if (result == IDCANCEL) return 3;
    return 1;
}
int gui_capture_next(const char *path) {
#ifdef SR_D3D12
    if (s_on && s_d3d12) return ged3d12_capture_next(path);
#else
    (void)path;
#endif
    return 0;
}
double gui_last_gpu_ms(void) {
#ifdef SR_D3D12
    if (s_on && s_d3d12) return ged3d12_last_gpu_ms();
#endif
    return -1.0;
}
int gui_last_gpu_sample(uint64_t *flip, double *milliseconds) {
#ifdef SR_D3D12
    if (s_on && s_d3d12)
        return ged3d12_last_gpu_sample(flip, milliseconds);
#else
    (void)flip;
    (void)milliseconds;
#endif
    return 0;
}
int gui_last_backend_timing(SrBackendTiming *timing) {
#ifdef SR_D3D12
    if (s_on && s_d3d12 && timing) {
        GeD3D12BackendTiming source;
        if (!ged3d12_last_backend_timing(&source)) return 0;
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
#else
    (void)timing;
#endif
    return 0;
}
int gui_framebuffer_stats(SrFramebufferStats *stats) {
#ifdef SR_D3D12
    if (s_on && s_d3d12 && stats) {
        GeD3D12FramebufferStats source;
        if (!ged3d12_framebuffer_stats(&source)) return 0;
        stats->readbacks = source.readbacks;
        stats->aliases = source.aliases;
        stats->feedback_snapshots = source.feedback_snapshots;
        stats->feedback_snapshot_pixels =
            source.feedback_snapshot_pixels;
        stats->feedback_reuses = source.feedback_reuses;
        return 1;
    }
#else
    (void)stats;
#endif
    return 0;
}

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
    sr_input_poll_reload();
    if (stride == 0) stride = 512;

#ifdef SR_D3D12
    if (s_d3d12) {
        int shown = s_ge_backend->present(fbaddr, fmt, stride);
        if (shown < 0) {
            fprintf(stderr, "gui_present: D3D12 backend failed\n");
            if (s_ge_backend->shutdown) s_ge_backend->shutdown();
            ge_set_backend(NULL);
            s_ge_backend = NULL;
            _Exit(2);
        }
        if (shown == 0) {
            if (s_ge_backend->shutdown) s_ge_backend->shutdown();
            ge_set_backend(NULL);
            s_ge_backend = NULL;
            _Exit(0);
        }
        s_buttons = ged3d12_buttons();
        ged3d12_analog(&s_lx, &s_ly);
        s_pad_present = ged3d12_pad_present();
        goto pace;
    }
#endif

#ifdef SR_SDL3VK
    if (s_sdl3) {
        /* GPU-resident framebuffer: blit straight from the GE's target image. Falls back
         * to the guest-VRAM convert for CPU-written frames (movies) or pure software GE. */
        int shown = s_ge_backend && s_ge_backend->present
            ? s_ge_backend->present(fbaddr, fmt, stride) : -1;
        if (shown < -1) {
            fprintf(stderr, "gui_present: Vulkan backend failed\n");
            if (s_ge_backend && s_ge_backend->shutdown) s_ge_backend->shutdown();
            ge_set_backend(NULL);
            s_ge_backend = NULL;
            sdl3vk_shutdown();
            _Exit(2);
        }
        if (shown == -1) {
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
    PadState pad;
    read_pad(&pad);
    s_buttons = sr_input_buttons(
        win_key_down, NULL, win_mouse_down, NULL,
        pad.present ? pad_down : NULL, &pad);
    int key_x = 0, key_y = 0;
    sr_input_keyboard_axis(win_key_down, NULL, &key_x, &key_y);
    int raw_x =
        key_x || key_y
            ? key_x
            : pad.has_movement_stick ? pad.movement_x : 0;
    int raw_y =
        key_x || key_y
            ? key_y
            : pad.has_movement_stick ? pad.movement_y : 0;
    if (pad.present) {
        SrInputSticks sticks = {
            raw_x,
            raw_y,
            pad.has_aim_stick ? pad.aim_x : 0,
            pad.has_aim_stick ? pad.aim_y : 0,
        };
        s_buttons =
            sr_input_gamepad_sticks(s_buttons, &sticks, &s_lx, &s_ly);
    } else {
        sr_input_gamepad_disconnected();
        sr_input_analog(raw_x, raw_y, &s_lx, &s_ly);
    }
    if (getenv("SR_INPUT_LOG")) {
        static uint32_t previous = UINT32_MAX;
        if (s_buttons != previous) {
            fprintf(
                stderr,
                "INPUT present=%u buttons=0x%04x pad=%s analog=%u,%u\n",
                sr_present_count(), s_buttons,
                s_pad_present ? "connected" : "none", s_lx, s_ly);
            previous = s_buttons;
        }
    }

    convert_fb(fbaddr, fmt, stride);
    HDC dc = GetDC(s_hwnd);
    RECT cr; GetClientRect(s_hwnd, &cr);
    StretchDIBits(dc, 0, 0, cr.right, cr.bottom, 0, 0, PSP_W, PSP_H,
                  s_px, &s_bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(s_hwnd, dc);

#if defined(SR_SDL3VK) || defined(SR_D3D12)
pace:
#endif
    update_fps_counter();
    /* Pace to ~60 Hz so the intro/menus play at a watchable speed. Skipped when the scheduler
     * already paces vblanks to real time (the default): pacing twice on unaligned grids makes
     * frames miss their vblank and costs a whole extra period (30/20 fps quantization). */
    {
        if (getenv("SR_NOVBPACE") ||
            sched_vbl_paced() || sched_fast_forwarding()) {
            QueryPerformanceCounter(&s_last);
            return;
        }
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

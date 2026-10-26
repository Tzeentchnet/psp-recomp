#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "graphics_settings.h"

#include "graphics_config.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#define IDC_RENDERER 1001
#define IDC_RESOLUTION 1002
#define IDC_TEXTURE_SCALE 1003
#define IDC_D3D12_SHADING 1004
#define IDC_ANTI_ALIASING 1005

typedef struct GraphicsDialogContext {
    SrGraphicsConfig config;
    const char *title;
} GraphicsDialogContext;

#pragma pack(push, 2)
typedef struct EmptyDialogTemplate {
    DLGTEMPLATE dialog;
    WORD menu;
    WORD window_class;
    WCHAR title;
} EmptyDialogTemplate;
#pragma pack(pop)

static void set_control_font(HWND control) {
    SendMessage(
        control, WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

static HWND add_control(
    HWND dialog, DWORD extended_style, const char *class_name,
    const char *text, DWORD style, int x, int y, int width, int height,
    int identifier) {
    HWND control = CreateWindowExA(
        extended_style, class_name, text,
        WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, dialog,
        (HMENU)(INT_PTR)identifier, GetModuleHandleA(NULL), NULL);
    if (control) set_control_font(control);
    return control;
}

static void add_combo_item(
    HWND combo, const char *label, LPARAM value, int *selected,
    int current) {
    LRESULT index = SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)label);
    if (index == CB_ERR || index == CB_ERRSPACE) return;
    SendMessage(combo, CB_SETITEMDATA, (WPARAM)index, value);
    if ((int)value == current) *selected = (int)index;
}

static void center_dialog(HWND dialog) {
    RECT bounds;
    int width;
    int height;
    GetWindowRect(dialog, &bounds);
    width = bounds.right - bounds.left;
    height = bounds.bottom - bounds.top;
    SetWindowPos(
        dialog, NULL,
        (GetSystemMetrics(SM_CXSCREEN) - width) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - height) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

static INT_PTR CALLBACK graphics_dialog_proc(
    HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    GraphicsDialogContext *context =
        (GraphicsDialogContext *)GetWindowLongPtr(
            dialog, GWLP_USERDATA);
    if (message == WM_INITDIALOG) {
        HWND renderer;
        HWND resolution;
        HWND texture_scale;
        HWND anti_aliasing;
        HWND d3d12_shading;
        int selected_renderer = 0;
        int selected_resolution = 0;
        int selected_texture_scale = 0;
        int selected_anti_aliasing = 0;
        int selected_d3d12_shading = 0;
        char label[64];

        context = (GraphicsDialogContext *)lparam;
        SetWindowLongPtr(
            dialog, GWLP_USERDATA, (LONG_PTR)context);
        SetWindowTextA(
            dialog,
            context->title && context->title[0]
                ? context->title : "Graphics Settings");

        add_control(
            dialog, 0, "STATIC", "Renderer:", 0,
            16, 17, 112, 20, 0);
        renderer = add_control(
            dialog, WS_EX_CLIENTEDGE, "COMBOBOX", "",
            WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            132, 13, 210, 120, IDC_RENDERER);
        add_combo_item(
            renderer, "Automatic", SR_GRAPHICS_RENDERER_AUTO,
            &selected_renderer, context->config.renderer);
#ifdef SR_D3D12
        add_combo_item(
            renderer, "D3D12 hardware", SR_GRAPHICS_RENDERER_D3D12,
            &selected_renderer, context->config.renderer);
#endif
        add_combo_item(
            renderer, "Software reference", SR_GRAPHICS_RENDERER_SOFTWARE,
            &selected_renderer, context->config.renderer);
#ifdef SR_SDL3VK
        add_combo_item(
            renderer, "Vulkan", SR_GRAPHICS_RENDERER_VULKAN,
            &selected_renderer, context->config.renderer);
#endif
        SendMessage(
            renderer, CB_SETCURSEL, (WPARAM)selected_renderer, 0);

        add_control(
            dialog, 0, "STATIC", "Internal resolution:", 0,
            16, 55, 112, 20, 0);
        resolution = add_control(
            dialog, WS_EX_CLIENTEDGE, "COMBOBOX", "",
            WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            132, 51, 210, 180, IDC_RESOLUTION);
        for (int scale = 1; scale <= 8; scale++) {
            int index;
            snprintf(
                label, sizeof(label), "%dx (%dx%d)",
                scale, 480 * scale, 272 * scale);
            index = (int)SendMessageA(
                resolution, CB_ADDSTRING, 0, (LPARAM)label);
            if (index < 0) continue;
            SendMessage(
                resolution, CB_SETITEMDATA,
                (WPARAM)index, (LPARAM)scale);
            if ((uint32_t)scale == context->config.internal_resolution)
                selected_resolution = index;
        }
        SendMessage(
            resolution, CB_SETCURSEL,
            (WPARAM)selected_resolution, 0);

        add_control(
            dialog, 0, "STATIC", "Texture upscaling:", 0,
            16, 93, 112, 20, 0);
        texture_scale = add_control(
            dialog, WS_EX_CLIENTEDGE, "COMBOBOX", "",
            WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            132, 89, 210, 150, IDC_TEXTURE_SCALE);
        add_combo_item(
            texture_scale, "Off (native textures)", 1,
            &selected_texture_scale, (int)context->config.texture_scale);
        for (int scale = 2; scale <= 5; scale++) {
            snprintf(label, sizeof(label), "%dx xBRZ", scale);
            add_combo_item(
                texture_scale, label, scale,
                &selected_texture_scale,
                (int)context->config.texture_scale);
        }
        SendMessage(
            texture_scale, CB_SETCURSEL,
            (WPARAM)selected_texture_scale, 0);

        add_control(
            dialog, 0, "STATIC", "Anti-aliasing:", 0,
            16, 131, 112, 20, 0);
        anti_aliasing = add_control(
            dialog, WS_EX_CLIENTEDGE, "COMBOBOX", "",
            WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            132, 127, 210, 100, IDC_ANTI_ALIASING);
        add_combo_item(
            anti_aliasing, "Off", SR_GRAPHICS_ANTI_ALIASING_OFF,
            &selected_anti_aliasing,
            context->config.anti_aliasing);
#ifdef SR_D3D12
        add_combo_item(
            anti_aliasing, "SMAA 1x High (D3D12)",
            SR_GRAPHICS_ANTI_ALIASING_SMAA,
            &selected_anti_aliasing,
            context->config.anti_aliasing);
#endif
        SendMessage(
            anti_aliasing, CB_SETCURSEL,
            (WPARAM)selected_anti_aliasing, 0);

        add_control(
            dialog, 0, "STATIC", "D3D12 shading:", 0,
            16, 169, 112, 20, 0);
        d3d12_shading = add_control(
            dialog, WS_EX_CLIENTEDGE, "COMBOBOX", "",
            WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            132, 165, 210, 100, IDC_D3D12_SHADING);
        add_combo_item(
            d3d12_shading, "Balanced (variable rate)", 0,
            &selected_d3d12_shading,
            (int)context->config.d3d12_full_rate);
        add_combo_item(
            d3d12_shading, "Full resolution (best quality)", 1,
            &selected_d3d12_shading,
            (int)context->config.d3d12_full_rate);
        SendMessage(
            d3d12_shading, CB_SETCURSEL,
            (WPARAM)selected_d3d12_shading, 0);

        add_control(
            dialog, 0, "STATIC",
            "Internal resolution improves geometry and edges. xBRZ improves game textures "
            "on hardware renderers. SMAA smooths the final D3D12 image.",
            0, 16, 202, 326, 44, 0);
        add_control(
            dialog, 0, "BUTTON", "Save",
            WS_TABSTOP | BS_DEFPUSHBUTTON,
            176, 252, 80, 27, IDOK);
        add_control(
            dialog, 0, "BUTTON", "Cancel",
            WS_TABSTOP | BS_PUSHBUTTON,
            262, 252, 80, 27, IDCANCEL);
        center_dialog(dialog);
        return TRUE;
    }
    if (message == WM_COMMAND && context) {
        int command = LOWORD(wparam);
        if (command == IDOK) {
            HWND renderer = GetDlgItem(dialog, IDC_RENDERER);
            HWND resolution = GetDlgItem(dialog, IDC_RESOLUTION);
            HWND texture_scale = GetDlgItem(dialog, IDC_TEXTURE_SCALE);
            HWND anti_aliasing =
                GetDlgItem(dialog, IDC_ANTI_ALIASING);
            HWND d3d12_shading =
                GetDlgItem(dialog, IDC_D3D12_SHADING);
            LRESULT renderer_index =
                SendMessage(renderer, CB_GETCURSEL, 0, 0);
            LRESULT resolution_index =
                SendMessage(resolution, CB_GETCURSEL, 0, 0);
            LRESULT texture_scale_index =
                SendMessage(texture_scale, CB_GETCURSEL, 0, 0);
            LRESULT anti_aliasing_index =
                SendMessage(anti_aliasing, CB_GETCURSEL, 0, 0);
            LRESULT d3d12_shading_index =
                SendMessage(d3d12_shading, CB_GETCURSEL, 0, 0);
            char error[512];
            if (renderer_index == CB_ERR ||
                resolution_index == CB_ERR ||
                texture_scale_index == CB_ERR ||
                anti_aliasing_index == CB_ERR ||
                d3d12_shading_index == CB_ERR) {
                MessageBoxA(
                    dialog, "Select all graphics options.",
                    "Graphics Settings", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            context->config.renderer = (int)SendMessage(
                renderer, CB_GETITEMDATA,
                (WPARAM)renderer_index, 0);
            context->config.internal_resolution =
                (uint32_t)SendMessage(
                    resolution, CB_GETITEMDATA,
                    (WPARAM)resolution_index, 0);
            context->config.texture_scale =
                (uint32_t)SendMessage(
                    texture_scale, CB_GETITEMDATA,
                    (WPARAM)texture_scale_index, 0);
            context->config.anti_aliasing =
                (int)SendMessage(
                    anti_aliasing, CB_GETITEMDATA,
                    (WPARAM)anti_aliasing_index, 0);
            context->config.d3d12_full_rate =
                (uint32_t)SendMessage(
                    d3d12_shading, CB_GETITEMDATA,
                    (WPARAM)d3d12_shading_index, 0);
            if (context->config.anti_aliasing ==
                    SR_GRAPHICS_ANTI_ALIASING_SMAA &&
                context->config.renderer !=
                    SR_GRAPHICS_RENDERER_AUTO &&
                context->config.renderer !=
                    SR_GRAPHICS_RENDERER_D3D12) {
                MessageBoxA(
                    dialog,
                    "SMAA 1x currently requires the D3D12 renderer.",
                    "Graphics Settings", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            if (!sr_graphics_config_save(
                    &context->config, error, sizeof(error))) {
                MessageBoxA(
                    dialog, error, "Cannot Save Graphics Settings",
                    MB_OK | MB_ICONERROR);
                return TRUE;
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (command == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
    }
    if (message == WM_CLOSE) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

int sr_graphics_settings_show(const char *title) {
    const SrGraphicsConfig *current = sr_graphics_config_get();
    GraphicsDialogContext context;
    EmptyDialogTemplate dialog_template;
    INT_PTR result;

    if (!current) {
        fprintf(stderr, "graphics: settings dialog requires initialized config\n");
        return -1;
    }
    context.config = *current;
    context.title = title;
    memset(&dialog_template, 0, sizeof(dialog_template));
    dialog_template.dialog.style =
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    dialog_template.dialog.cx = 236;
    dialog_template.dialog.cy = 198;
    result = DialogBoxIndirectParamW(
        GetModuleHandleW(NULL), &dialog_template.dialog, NULL,
        graphics_dialog_proc, (LPARAM)&context);
    if (result == -1) {
        fprintf(
            stderr, "graphics: cannot open settings dialog (error %lu)\n",
            (unsigned long)GetLastError());
        return -1;
    }
    return result == IDOK ? 1 : 0;
}

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
        int selected_renderer = 0;
        int selected_resolution = 0;
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
            dialog, 0, "STATIC",
            "Higher resolutions improve geometry and edge quality but use more GPU memory.",
            0, 16, 88, 326, 34, 0);
        add_control(
            dialog, 0, "BUTTON", "Save",
            WS_TABSTOP | BS_DEFPUSHBUTTON,
            176, 128, 80, 27, IDOK);
        add_control(
            dialog, 0, "BUTTON", "Cancel",
            WS_TABSTOP | BS_PUSHBUTTON,
            262, 128, 80, 27, IDCANCEL);
        center_dialog(dialog);
        return TRUE;
    }
    if (message == WM_COMMAND && context) {
        int command = LOWORD(wparam);
        if (command == IDOK) {
            HWND renderer = GetDlgItem(dialog, IDC_RENDERER);
            HWND resolution = GetDlgItem(dialog, IDC_RESOLUTION);
            LRESULT renderer_index =
                SendMessage(renderer, CB_GETCURSEL, 0, 0);
            LRESULT resolution_index =
                SendMessage(resolution, CB_GETCURSEL, 0, 0);
            char error[512];
            if (renderer_index == CB_ERR ||
                resolution_index == CB_ERR) {
                MessageBoxA(
                    dialog, "Select both graphics options.",
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
    dialog_template.dialog.cy = 112;
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

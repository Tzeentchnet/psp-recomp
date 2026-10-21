#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "input_settings.h"

#include "input_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define IDC_TWIN_STICK 1200
#define IDC_AIM_INVERT_X 1201
#define IDC_AIM_INVERT_Y 1202
#define IDC_GAMEPAD_BINDING_BASE 1300

typedef struct InputDialogContext {
    SrInputConfig config;
    const char *title;
} InputDialogContext;

#pragma pack(push, 2)
typedef struct EmptyDialogTemplate {
    DLGTEMPLATE dialog;
    WORD menu;
    WORD window_class;
    WCHAR title;
} EmptyDialogTemplate;
#pragma pack(pop)

static const char *const s_action_labels[SR_INPUT_ACTIONS] = {
    "Select",
    "Start",
    "D-pad Up",
    "D-pad Right",
    "D-pad Down",
    "D-pad Left",
    "PSP L / Strafe",
    "PSP R / Aim",
    "Triangle / Reload",
    "Circle / Grenade",
    "Cross / Interact",
    "Square / Fire",
};

static void set_control_font(HWND control) {
    SendMessage(
        control,
        WM_SETFONT,
        (WPARAM)GetStockObject(DEFAULT_GUI_FONT),
        TRUE);
}

static HWND add_control(
    HWND dialog,
    DWORD extended_style,
    const char *class_name,
    const char *text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    int identifier) {
    HWND control = CreateWindowExA(
        extended_style,
        class_name,
        text,
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        width,
        height,
        dialog,
        (HMENU)(INT_PTR)identifier,
        GetModuleHandleA(NULL),
        NULL);
    if (control) set_control_font(control);
    return control;
}

static void center_dialog(HWND dialog) {
    RECT bounds;
    int width;
    int height;
    GetWindowRect(dialog, &bounds);
    width = bounds.right - bounds.left;
    height = bounds.bottom - bounds.top;
    SetWindowPos(
        dialog,
        NULL,
        (GetSystemMetrics(SM_CXSCREEN) - width) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - height) / 2,
        0,
        0,
        SWP_NOSIZE | SWP_NOZORDER);
}

static int append_text(
    char *text, size_t text_size, size_t *used,
    const char *format, ...) {
    va_list arguments;
    int written;
    if (*used >= text_size) return 0;
    va_start(arguments, format);
    written = vsnprintf(
        text + *used, text_size - *used, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= text_size - *used) return 0;
    *used += (size_t)written;
    return 1;
}

static void set_binding_text(
    HWND dialog, const SrInputConfig *config, int action) {
    char binding[128];
    if (sr_input_config_format_gamepad_binding(
            config, action, binding, sizeof(binding))) {
        SetDlgItemTextA(
            dialog, IDC_GAMEPAD_BINDING_BASE + action, binding);
    }
}

static INT_PTR CALLBACK input_dialog_proc(
    HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    InputDialogContext *context =
        (InputDialogContext *)GetWindowLongPtr(dialog, GWLP_USERDATA);
    if (message == WM_INITDIALOG) {
        context = (InputDialogContext *)lparam;
        SetWindowLongPtr(
            dialog, GWLP_USERDATA, (LONG_PTR)context);
        SetWindowTextA(
            dialog,
            context->title && context->title[0]
                ? context->title
                : "Controller Settings");

        add_control(
            dialog, 0, "BUTTON", "Enable experimental twin-stick controls",
            WS_TABSTOP | BS_AUTOCHECKBOX,
            16, 16, 245, 22, IDC_TWIN_STICK);
        add_control(
            dialog, 0, "BUTTON", "Reverse aim horizontally",
            WS_TABSTOP | BS_AUTOCHECKBOX,
            278, 16, 150, 22, IDC_AIM_INVERT_X);
        add_control(
            dialog, 0, "BUTTON", "Reverse aim vertically",
            WS_TABSTOP | BS_AUTOCHECKBOX,
            444, 16, 140, 22, IDC_AIM_INVERT_Y);
        CheckDlgButton(
            dialog,
            IDC_TWIN_STICK,
            context->config.twin_stick_enabled
                ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(
            dialog,
            IDC_AIM_INVERT_X,
            context->config.aim_invert_x
                ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(
            dialog,
            IDC_AIM_INVERT_Y,
            context->config.aim_invert_y
                ? BST_CHECKED : BST_UNCHECKED);

        add_control(
            dialog, 0, "STATIC",
            "Gamepad bindings (comma-separated; use none to clear):",
            0, 16, 52, 420, 20, 0);
        for (int action = 0; action < SR_INPUT_ACTIONS; action++) {
            int column = action / 6;
            int row = action % 6;
            int label_x = column ? 310 : 16;
            int edit_x = column ? 414 : 120;
            int y = 80 + row * 34;
            char binding[128];
            if (!sr_input_config_format_gamepad_binding(
                    &context->config,
                    action,
                    binding,
                    sizeof(binding))) {
                strcpy(binding, "none");
            }
            add_control(
                dialog, 0, "STATIC", s_action_labels[action], 0,
                label_x, y + 4, 98, 20, 0);
            add_control(
                dialog, WS_EX_CLIENTEDGE, "EDIT", binding,
                WS_TABSTOP | ES_AUTOHSCROLL,
                edit_x, y, 170, 24,
                IDC_GAMEPAD_BINDING_BASE + action);
        }

        add_control(
            dialog, 0, "STATIC",
            "Examples: right_trigger, left_shoulder, west, south. "
            "All valid names remain documented in input.ini.",
            0, 16, 292, 568, 34, 0);
        add_control(
            dialog, 0, "BUTTON", "Save",
            WS_TABSTOP | BS_DEFPUSHBUTTON,
            414, 334, 80, 27, IDOK);
        add_control(
            dialog, 0, "BUTTON", "Close",
            WS_TABSTOP | BS_PUSHBUTTON,
            504, 334, 80, 27, IDCANCEL);
        center_dialog(dialog);
        return TRUE;
    }
    if (message == WM_COMMAND && context) {
        int command = LOWORD(wparam);
        if (command == IDC_TWIN_STICK &&
            HIWORD(wparam) == BN_CLICKED &&
            IsDlgButtonChecked(dialog, IDC_TWIN_STICK) ==
                BST_CHECKED) {
            sr_input_config_apply_twin_stick_defaults(
                &context->config);
            CheckDlgButton(
                dialog,
                IDC_AIM_INVERT_X,
                context->config.aim_invert_x
                    ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(
                dialog,
                IDC_AIM_INVERT_Y,
                context->config.aim_invert_y
                    ? BST_CHECKED : BST_UNCHECKED);
            set_binding_text(
                dialog, &context->config, SR_INPUT_L);
            set_binding_text(
                dialog, &context->config, SR_INPUT_R);
            set_binding_text(
                dialog, &context->config, SR_INPUT_SQUARE);
            return TRUE;
        }
        if (command == IDOK) {
            SrInputConfig parsed = context->config;
            char settings[4096];
            char value[128];
            char error[512];
            size_t used = 0;
            settings[0] = 0;
            for (int action = 0; action < SR_INPUT_ACTIONS; action++) {
                HWND edit = GetDlgItem(
                    dialog, IDC_GAMEPAD_BINDING_BASE + action);
                if (!edit ||
                    !GetWindowTextA(edit, value, sizeof(value)) ||
                    !append_text(
                        settings,
                        sizeof(settings),
                        &used,
                        "gamepad.%s = %s\n",
                        sr_input_action_name(action),
                        value)) {
                    MessageBoxA(
                        dialog,
                        "Every gamepad action needs a valid binding.",
                        "Controller Settings",
                        MB_OK | MB_ICONERROR);
                    return TRUE;
                }
            }
            if (!append_text(
                    settings,
                    sizeof(settings),
                    &used,
                    "twin_stick.enabled = %s\n"
                    "aim.invert_x = %s\n"
                    "aim.invert_y = %s\n",
                    IsDlgButtonChecked(dialog, IDC_TWIN_STICK) ==
                            BST_CHECKED
                        ? "true" : "false",
                    IsDlgButtonChecked(dialog, IDC_AIM_INVERT_X) ==
                            BST_CHECKED
                        ? "true" : "false",
                    IsDlgButtonChecked(dialog, IDC_AIM_INVERT_Y) ==
                            BST_CHECKED
                        ? "true" : "false")) {
                MessageBoxA(
                    dialog,
                    "Controller settings are too large.",
                    "Controller Settings",
                    MB_OK | MB_ICONERROR);
                return TRUE;
            }
            if (!sr_input_config_parse(
                    &parsed, settings, error, sizeof(error))) {
                MessageBoxA(
                    dialog,
                    error,
                    "Invalid Controller Settings",
                    MB_OK | MB_ICONERROR);
                return TRUE;
            }
            if (!sr_input_config_save(
                    &parsed, error, sizeof(error))) {
                MessageBoxA(
                    dialog,
                    error,
                    "Cannot Save Controller Settings",
                    MB_OK | MB_ICONERROR);
                return TRUE;
            }
            context->config = parsed;
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

int sr_input_settings_show(const char *title) {
    const SrInputConfig *current = sr_input_config_get();
    InputDialogContext context;
    EmptyDialogTemplate dialog_template;
    INT_PTR result;

    if (!current) {
        fprintf(
            stderr,
            "input: settings dialog requires initialized config\n");
        return -1;
    }
    context.config = *current;
    context.title = title;
    memset(&dialog_template, 0, sizeof(dialog_template));
    dialog_template.dialog.style =
        WS_POPUP | WS_CAPTION | WS_SYSMENU |
        DS_MODALFRAME | DS_CENTER;
    dialog_template.dialog.cx = 404;
    dialog_template.dialog.cy = 266;
    result = DialogBoxIndirectParamW(
        GetModuleHandleW(NULL),
        &dialog_template.dialog,
        NULL,
        input_dialog_proc,
        (LPARAM)&context);
    if (result == -1) {
        fprintf(
            stderr,
            "input: cannot open settings dialog (error %lu)\n",
            (unsigned long)GetLastError());
        return -1;
    }
    return result == IDOK ? 1 : 0;
}

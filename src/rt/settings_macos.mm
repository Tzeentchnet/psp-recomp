#import <AppKit/AppKit.h>

#include "graphics_settings.h"
#include "input_settings.h"

#include "graphics_config.h"
#include "input_config.h"
#include "macos_app.h"

#include <stdio.h>
#include <string>

#ifndef SR_ENABLE_KZL_TWIN_STICK
#define SR_ENABLE_KZL_TWIN_STICK 1
#endif

static NSString *sr_settings_string(const char *value) {
    if (!value) return @"";
    NSString *string = [NSString stringWithUTF8String:value];
    return string ?: @"";
}

static void sr_prepare_modal_ui(void) {
    NSApplication *application = [NSApplication sharedApplication];
    [application setActivationPolicy:NSApplicationActivationPolicyRegular];
    [application activateIgnoringOtherApps:YES];
}

static NSTextField *sr_label(
    NSString *text, NSRect frame, NSView *view) {
    NSTextField *label = [NSTextField labelWithString:text];
    label.frame = frame;
    label.alignment = NSTextAlignmentRight;
    [view addSubview:label];
    return label;
}

static void sr_add_popup_item(
    NSPopUpButton *popup, NSString *title, NSInteger value) {
    [popup addItemWithTitle:title];
    popup.lastItem.representedObject = @(value);
}

static void sr_select_popup_value(
    NSPopUpButton *popup, NSInteger value) {
    for (NSMenuItem *item in popup.itemArray) {
        if ([item.representedObject integerValue] == value) {
            [popup selectItem:item];
            return;
        }
    }
}

int sr_graphics_settings_show(const char *title) {
    @autoreleasepool {
        const SrGraphicsConfig *current = sr_graphics_config_get();
        if (!current) {
            fprintf(
                stderr,
                "graphics: settings dialog requires initialized config\n");
            return -1;
        }

        sr_prepare_modal_ui();
        NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 460, 228)];
        NSPopUpButton *quality =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(178, 188, 260, 26)];
        NSPopUpButton *renderer =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(178, 148, 260, 26)];
        NSPopUpButton *resolution =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(178, 108, 260, 26)];
        NSPopUpButton *texture =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(178, 68, 260, 26)];
        NSPopUpButton *antiAliasing =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(178, 28, 260, 26)];

        sr_label(@"Quality:", NSMakeRect(8, 192, 158, 20), view);
        sr_label(@"Renderer:", NSMakeRect(8, 152, 158, 20), view);
        sr_label(@"Internal resolution:", NSMakeRect(8, 112, 158, 20), view);
        sr_label(@"Texture upscaling:", NSMakeRect(8, 72, 158, 20), view);
        sr_label(@"Anti-aliasing:", NSMakeRect(8, 32, 158, 20), view);

        sr_add_popup_item(
            quality, @"Automatic (recommended)",
            SR_GRAPHICS_QUALITY_AUTO);
        sr_add_popup_item(
            quality, @"Manual", SR_GRAPHICS_QUALITY_MANUAL);
        sr_select_popup_value(quality, current->quality_mode);

        sr_add_popup_item(
            renderer, @"Automatic", SR_GRAPHICS_RENDERER_AUTO);
#ifdef SR_SDL3VK
        sr_add_popup_item(
            renderer, @"Vulkan through Metal", SR_GRAPHICS_RENDERER_VULKAN);
#endif
        sr_add_popup_item(
            renderer, @"Software reference", SR_GRAPHICS_RENDERER_SOFTWARE);
        sr_select_popup_value(renderer, current->renderer);

        for (NSInteger scale = SR_GRAPHICS_SCALE_MIN;
             scale <= SR_GRAPHICS_SCALE_MAX; scale++) {
            NSString *label = [NSString stringWithFormat:
                @"%ldx (%ldx%ld)", (long)scale,
                (long)(480 * scale), (long)(272 * scale)];
            sr_add_popup_item(resolution, label, scale);
        }
        sr_select_popup_value(
            resolution, current->internal_resolution);

        sr_add_popup_item(texture, @"Off (native textures)", 1);
        for (NSInteger scale = 2; scale <= SR_TEXTURE_SCALE_MAX; scale++) {
            sr_add_popup_item(
                texture,
                [NSString stringWithFormat:@"%ldx xBRZ", (long)scale],
                scale);
        }
        sr_select_popup_value(texture, current->texture_scale);

        sr_add_popup_item(
            antiAliasing, @"Off", SR_GRAPHICS_ANTI_ALIASING_OFF);
#ifdef SR_SDL3VK_SMAA
        sr_add_popup_item(
            antiAliasing, @"SMAA 1x High", SR_GRAPHICS_ANTI_ALIASING_SMAA);
#endif
        sr_select_popup_value(
            antiAliasing, current->anti_aliasing);

        [view addSubview:quality];
        [view addSubview:renderer];
        [view addSubview:resolution];
        [view addSubview:texture];
        [view addSubview:antiAliasing];

        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText =
            title && title[0]
                ? sr_settings_string(title)
                : @"Graphics Settings";
        alert.informativeText =
            @"Automatic quality selects conservative settings for the Mac "
             "and display at launch. Manual values are retained. Vulkan is "
             "translated to Metal by MoltenVK.";
        alert.accessoryView = view;
        [alert addButtonWithTitle:@"Save"];
        [alert addButtonWithTitle:@"Cancel"];
        if ([alert runModal] != NSAlertFirstButtonReturn)
            return 0;

        SrGraphicsConfig saved = *current;
        saved.quality_mode =
            [quality.selectedItem.representedObject intValue];
        saved.renderer =
            [renderer.selectedItem.representedObject intValue];
        saved.internal_resolution =
            [resolution.selectedItem.representedObject unsignedIntValue];
        saved.texture_scale =
            [texture.selectedItem.representedObject unsignedIntValue];
        saved.anti_aliasing =
            [antiAliasing.selectedItem.representedObject intValue];
        if (saved.quality_mode == SR_GRAPHICS_QUALITY_MANUAL &&
            saved.anti_aliasing ==
                SR_GRAPHICS_ANTI_ALIASING_SMAA &&
            saved.renderer == SR_GRAPHICS_RENDERER_SOFTWARE) {
            sr_macos_show_error(
                "Unsupported Graphics Settings",
                "SMAA 1x High requires Vulkan through Metal.");
            return -1;
        }
        char error[512];
        if (!sr_graphics_config_save(
                &saved, error, sizeof(error))) {
            sr_macos_show_error(
                "Cannot Save Graphics Settings", error);
            return -1;
        }
        return 1;
    }
}

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

@interface SrInputDialogController : NSObject {
    SrInputConfig _config;
    NSView *_view;
    NSButton *_twinStick;
    NSButton *_invertX;
    NSButton *_invertY;
    NSMutableArray<NSTextField *> *_bindings;
}
- (instancetype)initWithConfig:(const SrInputConfig *)config;
- (NSView *)view;
- (BOOL)saveWithError:(char *)error size:(size_t)errorSize;
@end

@implementation SrInputDialogController

- (instancetype)initWithConfig:(const SrInputConfig *)config {
    self = [super init];
    if (!self) return nil;
    _config = *config;
    _bindings = [NSMutableArray arrayWithCapacity:SR_INPUT_ACTIONS];
    _view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 680, 346)];

#if SR_ENABLE_KZL_TWIN_STICK
    _twinStick = [NSButton checkboxWithTitle:
        @"Enable experimental twin-stick controls"
        target:self
        action:@selector(twinStickChanged:)];
    _twinStick.frame = NSMakeRect(12, 314, 270, 24);
    _twinStick.state =
        _config.twin_stick_enabled ? NSControlStateValueOn
                                   : NSControlStateValueOff;
    [_view addSubview:_twinStick];

    _invertX = [NSButton checkboxWithTitle:
        @"Reverse aim horizontally" target:nil action:nil];
    _invertX.frame = NSMakeRect(300, 314, 180, 24);
    _invertX.state =
        _config.aim_invert_x ? NSControlStateValueOn
                             : NSControlStateValueOff;
    [_view addSubview:_invertX];

    _invertY = [NSButton checkboxWithTitle:
        @"Reverse aim vertically" target:nil action:nil];
    _invertY.frame = NSMakeRect(492, 314, 176, 24);
    _invertY.state =
        _config.aim_invert_y ? NSControlStateValueOn
                             : NSControlStateValueOff;
    [_view addSubview:_invertY];
#endif

    NSTextField *heading = [NSTextField labelWithString:
        @"Gamepad bindings (comma-separated; use none to clear):"];
    heading.frame = NSMakeRect(12, 282, 500, 20);
    [_view addSubview:heading];

    for (int action = 0; action < SR_INPUT_ACTIONS; action++) {
        int column = action / 6;
        int row = action % 6;
        CGFloat x = column ? 350 : 12;
        CGFloat y = 246 - row * 40;
        char binding[128] = "none";
        sr_input_config_format_gamepad_binding(
            &_config, action, binding, sizeof(binding));

        sr_label(
            sr_settings_string(s_action_labels[action]),
            NSMakeRect(x, y + 3, 132, 20),
            _view);
        NSTextField *field = [[NSTextField alloc]
            initWithFrame:NSMakeRect(x + 140, y, 178, 25)];
        field.stringValue = sr_settings_string(binding);
        field.tag = action;
        [_view addSubview:field];
        [_bindings addObject:field];
    }

    NSTextField *examples = [NSTextField labelWithString:
        @"Examples: right_trigger, left_shoulder, west, south. "
         "Every action accepts up to four inputs."];
    examples.frame = NSMakeRect(12, 0, 656, 34);
    examples.maximumNumberOfLines = 2;
    [_view addSubview:examples];
    return self;
}

- (void)twinStickChanged:(id)sender {
    (void)sender;
#if SR_ENABLE_KZL_TWIN_STICK
    if (_twinStick.state != NSControlStateValueOn)
        return;
    sr_input_config_apply_twin_stick_defaults(&_config);
    _invertX.state =
        _config.aim_invert_x ? NSControlStateValueOn
                             : NSControlStateValueOff;
    _invertY.state =
        _config.aim_invert_y ? NSControlStateValueOn
                             : NSControlStateValueOff;
    for (NSTextField *field in _bindings) {
        char binding[128] = "none";
        sr_input_config_format_gamepad_binding(
            &_config, (int)field.tag, binding, sizeof(binding));
        field.stringValue = sr_settings_string(binding);
    }
#endif
}

- (NSView *)view {
    return _view;
}

- (BOOL)saveWithError:(char *)error size:(size_t)errorSize {
    SrInputConfig parsed = _config;
    std::string settings;
    char line[256];

    for (NSTextField *field in _bindings) {
        const char *value = field.stringValue.UTF8String;
        int length;
        if (!value) {
            snprintf(error, errorSize, "A binding is not valid UTF-8.");
            return NO;
        }
        length = snprintf(
            line, sizeof(line), "gamepad.%s = %s\n",
            sr_input_action_name((int)field.tag), value);
        if (length < 0 || (size_t)length >= sizeof(line)) {
            snprintf(error, errorSize, "A controller binding is too long.");
            return NO;
        }
        settings.append(line, (size_t)length);
    }
#if SR_ENABLE_KZL_TWIN_STICK
    {
        int length = snprintf(
            line, sizeof(line),
            "twin_stick.enabled = %s\n"
            "aim.invert_x = %s\n"
            "aim.invert_y = %s\n",
            _twinStick.state == NSControlStateValueOn ? "true" : "false",
            _invertX.state == NSControlStateValueOn ? "true" : "false",
            _invertY.state == NSControlStateValueOn ? "true" : "false");
        if (length < 0 || (size_t)length >= sizeof(line)) {
            snprintf(error, errorSize, "Controller settings are too large.");
            return NO;
        }
        settings.append(line, (size_t)length);
    }
#endif
    if (!sr_input_config_parse(
            &parsed, settings.c_str(), error, errorSize) ||
        !sr_input_config_save(
            &parsed, error, errorSize)) {
        return NO;
    }
    _config = parsed;
    return YES;
}

@end

int sr_input_settings_show(const char *title, int allow_launch) {
    @autoreleasepool {
        const SrInputConfig *current = sr_input_config_get();
        if (!current) {
            fprintf(
                stderr,
                "input: settings dialog requires initialized config\n");
            return SR_INPUT_SETTINGS_ERROR;
        }

        sr_prepare_modal_ui();
        SrInputDialogController *controller =
            [[SrInputDialogController alloc] initWithConfig:current];
        int saved = 0;
        for (;;) {
            NSAlert *alert = [[NSAlert alloc] init];
            alert.messageText =
                title && title[0]
                    ? sr_settings_string(title)
                    : @"Controller Settings";
            alert.informativeText = saved
                ? @"Controller settings saved. Changes hot-reload in a running game."
                : @"Edit SDL gamepad bindings and the optional twin-stick preset.";
            alert.accessoryView = controller.view;
            [alert addButtonWithTitle:@"Save"];
            [alert addButtonWithTitle:
                allow_launch ? @"Play Game" : @"Close"];
            if (allow_launch)
                [alert addButtonWithTitle:@"Cancel"];

            NSModalResponse response = [alert runModal];
            if (response == NSAlertFirstButtonReturn) {
                char error[512];
                if (![controller saveWithError:error size:sizeof(error)]) {
                    sr_macos_show_error(
                        "Invalid Controller Settings", error);
                } else {
                    saved = 1;
                }
                continue;
            }
            if (response == NSAlertSecondButtonReturn) {
                if (!allow_launch)
                    return SR_INPUT_SETTINGS_CLOSED;
                char error[512];
                if (![controller saveWithError:error size:sizeof(error)]) {
                    sr_macos_show_error(
                        "Invalid Controller Settings", error);
                    continue;
                }
                return SR_INPUT_SETTINGS_LAUNCH;
            }
            return SR_INPUT_SETTINGS_CLOSED;
        }
    }
}

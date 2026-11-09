#import <AppKit/AppKit.h>

#include "audio.h"
#include "gpu_sdl3vk/sdl3vk.h"
#include "macos_app.h"
#include "platform.h"
#include "settings_path.h"
#include "startup_check.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef SR_ISO_PICKER_TITLE
#define SR_ISO_PICKER_TITLE "Choose Game ISO"
#endif

static id s_sleep_observer;
static id s_wake_observer;

static void sr_macos_install_lifecycle_notifications(void) {
    if (s_sleep_observer || s_wake_observer) return;
    NSNotificationCenter *center =
        [NSWorkspace sharedWorkspace].notificationCenter;
    s_sleep_observer = [center
        addObserverForName:NSWorkspaceWillSleepNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification *notification) {
                    (void)notification;
                    fprintf(stderr, "macOS runtime: system will sleep\n");
                    sdl3vk_set_system_suspended(1);
                    sr_audio_set_suspended(1);
                }];
    s_wake_observer = [center
        addObserverForName:NSWorkspaceDidWakeNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification *notification) {
                    (void)notification;
                    sdl3vk_set_system_suspended(0);
                    sr_audio_set_suspended(0);
                    fprintf(stderr, "macOS runtime: system did wake\n");
                }];
    if (!s_sleep_observer || !s_wake_observer)
        fprintf(
            stderr,
            "macOS runtime: cannot install sleep/wake notifications\n");
}

static NSString *sr_string(const char *value) {
    if (!value) return @"";
    NSString *string = [NSString stringWithUTF8String:value];
    return string ?: @"";
}

static void sr_macos_activate(void) {
    NSApplication *application = [NSApplication sharedApplication];
    [application setActivationPolicy:NSApplicationActivationPolicyRegular];
    [application activateIgnoringOtherApps:YES];
}

void sr_macos_show_error(const char *title, const char *message) {
    @autoreleasepool {
        sr_macos_activate();
        NSAlert *alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleCritical;
        alert.messageText =
            title && title[0] ? sr_string(title) : @"Cannot Launch Game";
        alert.informativeText = sr_string(message);
        [alert addButtonWithTitle:@"OK"];
        [alert runModal];
    }
}

void sr_macos_prepare_runtime(void) {
    char path[SR_PATH_MAX];
    char error[256];
    FILE *file;

    @autoreleasepool {
        sr_macos_install_lifecycle_notifications();
    }
    if (getenv("VK_ICD_FILENAMES")) return;
    if (!sr_resource_path(
            "vulkan/icd.d/MoltenVK_icd.json",
            path, sizeof(path), error, sizeof(error))) {
        fprintf(stderr, "macOS runtime: %s\n", error);
        return;
    }
    file = fopen(path, "rb");
    if (!file) return;
    fclose(file);
    if (setenv("VK_ICD_FILENAMES", path, 0) != 0)
        fprintf(stderr, "macOS runtime: cannot select bundled MoltenVK ICD\n");
}

int sr_macos_thermal_state(void) {
    @autoreleasepool {
        return (int)[NSProcessInfo processInfo].thermalState;
    }
}

int sr_macos_ensure_iso(const char *title) {
    @autoreleasepool {
        char path[SR_PATH_MAX];
        char error[512];
        const char *override = getenv("PSP_ISO");

        sr_macos_activate();
        if (override && override[0]) {
            if (sr_startup_validate_iso_path(
                    override, error, sizeof(error))) {
                return 1;
            }
            sr_macos_show_error(title, error);
            return -1;
        }

        if (sr_startup_resolve_iso_path(
                path, sizeof(path), error, sizeof(error)) &&
            sr_startup_validate_iso_path(
                path, error, sizeof(error))) {
            return 1;
        }

        for (;;) {
            NSOpenPanel *panel = [NSOpenPanel openPanel];
            panel.title = sr_string(SR_ISO_PICKER_TITLE);
            panel.message =
                @"Select your legally dumped supported UMD ISO. "
                 "The game is verified in place and is not copied.";
            panel.prompt = @"Choose ISO";
            panel.canChooseFiles = YES;
            panel.canChooseDirectories = NO;
            panel.allowsMultipleSelection = NO;
            panel.resolvesAliases = YES;

            if ([panel runModal] != NSModalResponseOK)
                return 0;
            const char *selected =
                panel.URL.fileSystemRepresentation;
            if (selected &&
                sr_startup_save_iso_path(
                    selected, error, sizeof(error))) {
                return 1;
            }
            sr_macos_show_error(
                "Unsupported Game Image",
                error[0] ? error : "The selected file could not be used.");
        }
    }
}

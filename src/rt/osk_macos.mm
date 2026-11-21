#import <AppKit/AppKit.h>

#include <string.h>
#include <wchar.h>

static NSString *sr_string_from_wide(const wchar_t *value) {
    if (!value || !value[0]) return @"";
    return [[NSString alloc]
        initWithBytes:value
        length:wcslen(value) * sizeof(wchar_t)
        encoding:NSUTF32LittleEndianStringEncoding];
}

static int sr_copy_wide(
    NSString *value, wchar_t *out, int capacity) {
    if (!value || !out || capacity < 2) return 0;
    NSString *candidate = value;
    NSData *data = [candidate
        dataUsingEncoding:NSUTF32LittleEndianStringEncoding
        allowLossyConversion:NO];
    while (data && data.length >
               (size_t)(capacity - 1) * sizeof(wchar_t) &&
           candidate.length) {
        NSRange sequence = [candidate
            rangeOfComposedCharacterSequenceAtIndex:candidate.length - 1u];
        candidate = [candidate substringToIndex:sequence.location];
        data = [candidate
            dataUsingEncoding:NSUTF32LittleEndianStringEncoding
            allowLossyConversion:NO];
    }
    if (!data ||
        data.length > (size_t)(capacity - 1) * sizeof(wchar_t)) {
        return 0;
    }
    memcpy(out, data.bytes, data.length);
    out[data.length / sizeof(wchar_t)] = 0;
    return 1;
}

extern "C" int sr_osk_input(
    const wchar_t *description, const wchar_t *initial,
    wchar_t *out, int capacity) {
    @autoreleasepool {
        if (!out || capacity < 2) return 0;
        NSApplication *application = [NSApplication sharedApplication];
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        [application activateIgnoringOtherApps:YES];

        NSTextField *field = [[NSTextField alloc]
            initWithFrame:NSMakeRect(0, 0, 420, 26)];
        field.stringValue = sr_string_from_wide(initial) ?: @"";

        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"Text Entry";
        NSString *prompt = sr_string_from_wide(description);
        alert.informativeText =
            prompt.length ? prompt : @"Enter text";
        alert.accessoryView = field;
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];
        [alert.window setInitialFirstResponder:field];

        if ([alert runModal] != NSAlertFirstButtonReturn)
            return 0;
        return sr_copy_wide(field.stringValue, out, capacity);
    }
}

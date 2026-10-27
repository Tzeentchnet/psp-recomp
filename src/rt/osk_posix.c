#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

int sr_osk_input(
    const wchar_t *description, const wchar_t *initial,
    wchar_t *out, int capacity) {
    char prompt[512] = "Enter text";
    char line[1024];
    if (!out || capacity < 2) return 0;
    if (!isatty(STDIN_FILENO)) {
        fprintf(
            stderr,
            "osk: text input requires an interactive terminal; "
            "cancelling the request\n");
        return 0;
    }
    if (description && description[0])
        wcstombs(prompt, description, sizeof(prompt) - 1u);
    prompt[sizeof(prompt) - 1u] = 0;
    fprintf(stderr, "%s", prompt);
    if (initial && initial[0]) {
        char current[512] = {0};
        wcstombs(current, initial, sizeof(current) - 1u);
        fprintf(stderr, " [%s]", current);
    }
    fprintf(stderr, ": ");
    fflush(stderr);
    if (!fgets(line, sizeof(line), stdin)) return 0;
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0] && initial) {
        wcsncpy(out, initial, (size_t)capacity - 1u);
        out[capacity - 1] = 0;
        return 1;
    }
    size_t converted = mbstowcs(out, line, (size_t)capacity - 1u);
    if (converted == (size_t)-1) {
        fprintf(stderr, "osk: entered text is not valid in the current locale\n");
        return 0;
    }
    out[converted] = 0;
    return 1;
}

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "input_script.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANALOG_SCRIPT_HEADER "# psp-recomp analog script v1"
#define ANALOG_SCRIPT_MAX_BYTES (64u * 1024u)

static void set_error(
    char *error, size_t error_size, const char *format, ...) {
    va_list args;
    if (!error || !error_size) return;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

static int parse_values(const char *line, uint32_t values[4]) {
    const char *cursor = line;
    for (int i = 0; i < 4; i++) {
        char *end = NULL;
        unsigned long parsed;
        while (isspace((unsigned char)*cursor)) cursor++;
        if (!isdigit((unsigned char)*cursor)) return 0;
        errno = 0;
        parsed = strtoul(cursor, &end, 10);
        if (errno == ERANGE || end == cursor || parsed > UINT32_MAX) return 0;
        values[i] = (uint32_t)parsed;
        cursor = end;
    }
    while (isspace((unsigned char)*cursor)) cursor++;
    return *cursor == '\0';
}

int sr_analog_script_parse(
    SrAnalogScript *script,
    const char *text,
    char *error,
    size_t error_size) {
    const char *cursor;
    uint32_t line_number = 0;
    int saw_header = 0;

    if (!script || !text) {
        set_error(error, error_size, "missing script or text");
        return 0;
    }
    memset(script, 0, sizeof(*script));
    cursor = text;
    while (*cursor) {
        const char *newline = strchr(cursor, '\n');
        size_t raw_length = newline ? (size_t)(newline - cursor) : strlen(cursor);
        size_t length = raw_length;
        char line[160];
        char *content;
        char *end;
        uint32_t values[4];
        SrAnalogScriptEvent *event;

        line_number++;
        if (length && cursor[length - 1] == '\r') length--;
        if (length >= sizeof(line)) {
            set_error(
                error, error_size, "line %u is too long", line_number);
            return 0;
        }
        memcpy(line, cursor, length);
        line[length] = '\0';
        cursor = newline ? newline + 1 : cursor + raw_length;

        content = line;
        while (isspace((unsigned char)*content)) content++;
        end = content + strlen(content);
        while (end > content && isspace((unsigned char)end[-1])) end--;
        *end = '\0';
        if (!content[0]) continue;
        if (content[0] == '#') {
            if (strcmp(content, ANALOG_SCRIPT_HEADER) == 0) saw_header = 1;
            continue;
        }
        if (!parse_values(content, values)) {
            set_error(
                error, error_size,
                "line %u: expected <present> <duration> <lx> <ly>",
                line_number);
            return 0;
        }
        if (!values[1]) {
            set_error(
                error, error_size,
                "line %u: duration must be positive", line_number);
            return 0;
        }
        if (values[1] > UINT32_MAX - values[0]) {
            set_error(
                error, error_size,
                "line %u: presentation range overflows", line_number);
            return 0;
        }
        if (values[2] > 255u || values[3] > 255u) {
            set_error(
                error, error_size,
                "line %u: analog values must be in 0..255", line_number);
            return 0;
        }
        if (script->count == SR_ANALOG_SCRIPT_MAX_EVENTS) {
            set_error(
                error, error_size,
                "script exceeds %u events",
                (unsigned)SR_ANALOG_SCRIPT_MAX_EVENTS);
            return 0;
        }
        if (script->count &&
            values[0] < script->events[script->count - 1].end_present) {
            set_error(
                error, error_size,
                "line %u: events must be ordered and non-overlapping",
                line_number);
            return 0;
        }
        event = &script->events[script->count++];
        event->first_present = values[0];
        event->end_present = values[0] + values[1];
        event->lx = (uint8_t)values[2];
        event->ly = (uint8_t)values[3];
    }
    if (!saw_header) {
        set_error(
            error, error_size, "missing '%s' header", ANALOG_SCRIPT_HEADER);
        return 0;
    }
    if (!script->count) {
        set_error(error, error_size, "script has no events");
        return 0;
    }
    if (error && error_size) error[0] = '\0';
    return 1;
}

int sr_analog_script_load(
    SrAnalogScript *script,
    const char *path,
    char *error,
    size_t error_size) {
    FILE *file;
    char *text;
    size_t bytes = 0;
    int result;

    if (!path || !path[0]) {
        set_error(error, error_size, "missing script path");
        return 0;
    }
    file = fopen(path, "rb");
    if (!file) {
        set_error(error, error_size, "cannot open %s", path);
        return 0;
    }
    text = (char *)malloc(ANALOG_SCRIPT_MAX_BYTES + 2u);
    if (!text) {
        fclose(file);
        set_error(error, error_size, "out of memory reading %s", path);
        return 0;
    }
    while (bytes < ANALOG_SCRIPT_MAX_BYTES + 1u) {
        size_t read = fread(
            text + bytes, 1,
            ANALOG_SCRIPT_MAX_BYTES + 1u - bytes, file);
        bytes += read;
        if (!read) break;
    }
    if (ferror(file)) {
        free(text);
        fclose(file);
        set_error(error, error_size, "cannot read %s", path);
        return 0;
    }
    fclose(file);
    if (bytes > ANALOG_SCRIPT_MAX_BYTES) {
        free(text);
        set_error(
            error, error_size, "%s exceeds %u bytes",
            path, (unsigned)ANALOG_SCRIPT_MAX_BYTES);
        return 0;
    }
    if (memchr(text, '\0', bytes)) {
        free(text);
        set_error(error, error_size, "%s contains a NUL byte", path);
        return 0;
    }
    text[bytes] = '\0';
    result = sr_analog_script_parse(script, text, error, error_size);
    free(text);
    return result;
}

int sr_analog_script_sample(
    const SrAnalogScript *script,
    uint32_t present,
    uint8_t *lx,
    uint8_t *ly) {
    if (!script) return 0;
    for (uint32_t i = 0; i < script->count; i++) {
        const SrAnalogScriptEvent *event = &script->events[i];
        if (present < event->first_present) return 0;
        if (present < event->end_present) {
            if (lx) *lx = event->lx;
            if (ly) *ly = event->ly;
            return 1;
        }
    }
    return 0;
}

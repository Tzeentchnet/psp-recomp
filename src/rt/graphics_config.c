#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "graphics_config.h"

#include "settings_path.h"

#include <ctype.h>
#include <errno.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static SrGraphicsConfig s_config;
static char s_config_path[1024];
static int s_initialized;
static int s_first_run;

static int text_equal(const char *left, const char *right) {
    while (*left && *right) {
        if (tolower((unsigned char)*left) !=
            tolower((unsigned char)*right)) {
            return 0;
        }
        left++;
        right++;
    }
    return !*left && !*right;
}

static char *trim(char *text) {
    char *end;
    while (*text && isspace((unsigned char)*text)) text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) end--;
    *end = 0;
    return text;
}

static void set_error(
    char *error, size_t error_size, int line,
    const char *message, const char *detail) {
    if (!error || !error_size) return;
    if (line > 0 && detail)
        snprintf(
            error, error_size, "line %d: %s: %s",
            line, message, detail);
    else if (line > 0)
        snprintf(error, error_size, "line %d: %s", line, message);
    else if (detail)
        snprintf(error, error_size, "%s: %s", message, detail);
    else
        snprintf(error, error_size, "%s", message);
}

static int renderer_from_name(const char *name) {
    if (text_equal(name, "auto")) return SR_GRAPHICS_RENDERER_AUTO;
    if (text_equal(name, "d3d12")) return SR_GRAPHICS_RENDERER_D3D12;
    if (text_equal(name, "sw") || text_equal(name, "software"))
        return SR_GRAPHICS_RENDERER_SOFTWARE;
    if (text_equal(name, "vk") || text_equal(name, "vulkan"))
        return SR_GRAPHICS_RENDERER_VULKAN;
    return -1;
}

static int anti_aliasing_from_name(const char *name) {
    if (text_equal(name, "off") || text_equal(name, "none"))
        return SR_GRAPHICS_ANTI_ALIASING_OFF;
    if (text_equal(name, "smaa"))
        return SR_GRAPHICS_ANTI_ALIASING_SMAA;
    return -1;
}

void sr_graphics_config_defaults(SrGraphicsConfig *config) {
    if (!config) return;
    config->renderer = SR_GRAPHICS_RENDERER_D3D12;
    config->internal_resolution = 2u;
    config->texture_scale = 1u;
    config->d3d12_full_rate = 0u;
    config->anti_aliasing = SR_GRAPHICS_ANTI_ALIASING_OFF;
}

const char *sr_graphics_renderer_name(int renderer) {
    switch (renderer) {
        case SR_GRAPHICS_RENDERER_AUTO: return "auto";
        case SR_GRAPHICS_RENDERER_D3D12: return "d3d12";
        case SR_GRAPHICS_RENDERER_SOFTWARE: return "sw";
        case SR_GRAPHICS_RENDERER_VULKAN: return "vk";
        default: return NULL;
    }
}

const char *sr_graphics_anti_aliasing_name(int anti_aliasing) {
    switch (anti_aliasing) {
        case SR_GRAPHICS_ANTI_ALIASING_OFF: return "off";
        case SR_GRAPHICS_ANTI_ALIASING_SMAA: return "smaa";
        default: return NULL;
    }
}

int sr_graphics_config_parse(
    SrGraphicsConfig *config, const char *text,
    char *error, size_t error_size) {
    unsigned seen = 0;
    int line_number = 0;
    const char *cursor = text;

    if (error && error_size) error[0] = 0;
    if (!config || !text) {
        set_error(error, error_size, 0, "invalid graphics config", NULL);
        return 0;
    }

    while (*cursor) {
        char line[512];
        size_t length = 0;
        char *entry;
        char *equals;
        char *name;
        char *value;
        unsigned setting;

        while (cursor[length] &&
               cursor[length] != '\n' &&
               cursor[length] != '\r') {
            length++;
        }
        line_number++;
        if (length >= sizeof(line)) {
            set_error(
                error, error_size, line_number,
                "line is too long", NULL);
            return 0;
        }
        memcpy(line, cursor, length);
        line[length] = 0;
        cursor += length;
        while (*cursor == '\r' || *cursor == '\n') cursor++;

        entry = trim(line);
        if (!entry[0] || entry[0] == '#' || entry[0] == ';') continue;
        equals = strchr(entry, '=');
        if (!equals) {
            set_error(
                error, error_size, line_number,
                "missing '='", NULL);
            return 0;
        }
        *equals = 0;
        name = trim(entry);
        value = trim(equals + 1);
        if (!name[0] || !value[0]) {
            set_error(
                error, error_size, line_number,
                "empty name or value", NULL);
            return 0;
        }

        if (text_equal(name, "renderer")) {
            int renderer = renderer_from_name(value);
            setting = 1u;
            if (renderer < 0) {
                set_error(
                    error, error_size, line_number,
                    "expected auto, d3d12, sw, or vk", value);
                return 0;
            }
            if (seen & setting) {
                set_error(
                    error, error_size, line_number,
                    "duplicate setting", name);
                return 0;
            }
            seen |= setting;
            config->renderer = renderer;
        } else if (text_equal(name, "internal_resolution")) {
            char *end = NULL;
            unsigned long parsed;
            setting = 2u;
            if (seen & setting) {
                set_error(
                    error, error_size, line_number,
                    "duplicate setting", name);
                return 0;
            }
            errno = 0;
            parsed = strtoul(value, &end, 10);
            if (errno || !end || *trim(end) ||
                parsed < SR_GRAPHICS_SCALE_MIN ||
                parsed > SR_GRAPHICS_SCALE_MAX) {
                set_error(
                    error, error_size, line_number,
                    "internal_resolution must be an integer from 1 to 8",
                    value);
                return 0;
            }
            seen |= setting;
            config->internal_resolution = (uint32_t)parsed;
        } else if (text_equal(name, "texture_scale")) {
            char *end = NULL;
            unsigned long parsed;
            setting = 4u;
            if (seen & setting) {
                set_error(
                    error, error_size, line_number,
                    "duplicate setting", name);
                return 0;
            }
            errno = 0;
            parsed = strtoul(value, &end, 10);
            if (errno || !end || *trim(end) ||
                parsed < SR_TEXTURE_SCALE_MIN ||
                parsed > SR_TEXTURE_SCALE_MAX) {
                set_error(
                    error, error_size, line_number,
                    "texture_scale must be an integer from 1 to 5",
                    value);
                return 0;
            }
            seen |= setting;
            config->texture_scale = (uint32_t)parsed;
        } else if (text_equal(name, "d3d12_full_rate")) {
            char *end = NULL;
            unsigned long parsed;
            setting = 8u;
            if (seen & setting) {
                set_error(
                    error, error_size, line_number,
                    "duplicate setting", name);
                return 0;
            }
            errno = 0;
            parsed = strtoul(value, &end, 10);
            if (errno || !end || *trim(end) || parsed > 1u) {
                set_error(
                    error, error_size, line_number,
                    "d3d12_full_rate must be 0 or 1", value);
                return 0;
            }
            seen |= setting;
            config->d3d12_full_rate = (uint32_t)parsed;
        } else if (text_equal(name, "anti_aliasing")) {
            int anti_aliasing = anti_aliasing_from_name(value);
            setting = 16u;
            if (anti_aliasing < 0) {
                set_error(
                    error, error_size, line_number,
                    "expected off or smaa", value);
                return 0;
            }
            if (seen & setting) {
                set_error(
                    error, error_size, line_number,
                    "duplicate setting", name);
                return 0;
            }
            seen |= setting;
            config->anti_aliasing = anti_aliasing;
        } else {
            set_error(
                error, error_size, line_number,
                "unknown setting", name);
            return 0;
        }
    }
    return 1;
}

static char *read_config(
    const char *path, char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    long size;
    char *text;
    if (!file) {
        set_error(error, error_size, 0, "cannot open graphics config", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0 ||
        (size = ftell(file)) < 0 ||
        size > 65536 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        set_error(error, error_size, 0, "cannot size graphics config", path);
        return NULL;
    }
    text = (char *)malloc((size_t)size + 1u);
    if (!text) {
        fclose(file);
        set_error(error, error_size, 0, "out of memory reading", path);
        return NULL;
    }
    if (fread(text, 1, (size_t)size, file) != (size_t)size ||
        ferror(file)) {
        free(text);
        fclose(file);
        set_error(error, error_size, 0, "cannot read graphics config", path);
        return NULL;
    }
    text[size] = 0;
    fclose(file);
    return text;
}

int sr_graphics_config_init(void) {
    const char *override;
    char error[512];
    FILE *existing;
    char *text;
    SrGraphicsConfig parsed;

    if (s_initialized) return 1;
    sr_graphics_config_defaults(&s_config);
    override = getenv("SR_GRAPHICS_CONFIG");
    if (override && strcmp(override, "0") == 0) {
        s_config_path[0] = 0;
        s_first_run = 0;
        s_initialized = 1;
        fprintf(
            stderr,
            "graphics: defaults (config disabled by SR_GRAPHICS_CONFIG=0)\n");
        return 1;
    }
    if (override && override[0]) {
        if (strlen(override) >= sizeof(s_config_path)) {
            fprintf(stderr, "graphics: SR_GRAPHICS_CONFIG path is too long\n");
            return 0;
        }
        strcpy(s_config_path, override);
    } else if (!sr_settings_path(
            "graphics.ini", s_config_path, sizeof(s_config_path),
            error, sizeof(error))) {
        fprintf(stderr, "graphics: %s\n", error);
        return 0;
    }

    errno = 0;
    existing = fopen(s_config_path, "rb");
    if (!existing) {
        if (errno != ENOENT) {
            fprintf(
                stderr, "graphics: cannot open %s: %s\n",
                s_config_path, strerror(errno));
            return 0;
        }
        s_first_run = 1;
        s_initialized = 1;
        fprintf(
            stderr, "graphics: first run; settings will be written to %s\n",
            s_config_path);
        return 1;
    }
    fclose(existing);

    text = read_config(s_config_path, error, sizeof(error));
    if (!text) {
        fprintf(stderr, "graphics: %s\n", error);
        return 0;
    }
    sr_graphics_config_defaults(&parsed);
    if (!sr_graphics_config_parse(
            &parsed, text, error, sizeof(error))) {
        free(text);
        fprintf(
            stderr, "graphics: invalid %s: %s\n",
            s_config_path, error);
        return 0;
    }
    free(text);
    s_config = parsed;
    s_first_run = 0;
    s_initialized = 1;
    fprintf(
        stderr,
        "graphics: loaded %s (renderer %s, internal resolution %ux, "
        "texture scale %ux, D3D12 full-rate %s, anti-aliasing %s)\n",
        s_config_path, sr_graphics_renderer_name(s_config.renderer),
        s_config.internal_resolution, s_config.texture_scale,
        s_config.d3d12_full_rate ? "on" : "off",
        sr_graphics_anti_aliasing_name(s_config.anti_aliasing));
    return 1;
}

int sr_graphics_config_first_run(void) {
    return s_first_run;
}

const SrGraphicsConfig *sr_graphics_config_get(void) {
    return s_initialized ? &s_config : NULL;
}

const char *sr_graphics_config_path(void) {
    return s_config_path;
}

int sr_graphics_config_save(
    const SrGraphicsConfig *config, char *error, size_t error_size) {
    char temporary[1100];
    const char *renderer;
    const char *anti_aliasing;
    FILE *file;
    int length;
    int ok;

    if (error && error_size) error[0] = 0;
    if (!s_initialized || !config || !s_config_path[0]) {
        set_error(
            error, error_size, 0,
            "graphics config cannot be saved", NULL);
        return 0;
    }
    renderer = sr_graphics_renderer_name(config->renderer);
    anti_aliasing =
        sr_graphics_anti_aliasing_name(config->anti_aliasing);
    if (!renderer || !anti_aliasing ||
        config->internal_resolution < SR_GRAPHICS_SCALE_MIN ||
        config->internal_resolution > SR_GRAPHICS_SCALE_MAX ||
        config->texture_scale < SR_TEXTURE_SCALE_MIN ||
        config->texture_scale > SR_TEXTURE_SCALE_MAX ||
        config->d3d12_full_rate > 1u) {
        set_error(
            error, error_size, 0,
            "graphics config contains invalid values", NULL);
        return 0;
    }
    length = snprintf(
        temporary, sizeof(temporary), "%s.tmp", s_config_path);
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        set_error(
            error, error_size, 0,
            "temporary graphics config path is too long", NULL);
        return 0;
    }
    file = fopen(temporary, "wb");
    if (!file) {
        set_error(
            error, error_size, 0,
            "cannot create temporary graphics config", temporary);
        return 0;
    }
    ok = fprintf(
            file,
        "# PSP recomp graphics config v3\n"
        "renderer = %s\n"
        "internal_resolution = %u\n"
        "texture_scale = %u\n"
        "d3d12_full_rate = %u\n"
        "anti_aliasing = %s\n",
        renderer, config->internal_resolution,
        config->texture_scale, config->d3d12_full_rate,
        anti_aliasing) > 0 &&
         fflush(file) == 0 &&
         _commit(_fileno(file)) == 0;
    if (fclose(file) != 0) ok = 0;
    if (!ok) {
        DeleteFileA(temporary);
        set_error(
            error, error_size, 0,
            "cannot write temporary graphics config", temporary);
        return 0;
    }
    if (!MoveFileExA(
            temporary, s_config_path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        char detail[256];
        snprintf(
            detail, sizeof(detail), "%s (error %lu)",
            s_config_path, (unsigned long)GetLastError());
        DeleteFileA(temporary);
        set_error(
            error, error_size, 0,
            "cannot publish graphics config", detail);
        return 0;
    }
    s_config = *config;
    s_first_run = 0;
    fprintf(
        stderr,
        "graphics: saved %s (renderer %s, internal resolution %ux, "
        "texture scale %ux, D3D12 full-rate %s, anti-aliasing %s)\n",
        s_config_path, renderer, config->internal_resolution,
        config->texture_scale,
        config->d3d12_full_rate ? "on" : "off",
        anti_aliasing);
    return 1;
}

int sr_graphics_effective_scale(
    uint32_t *scale, char *error, size_t error_size) {
    const char *override;
    char *end = NULL;
    unsigned long parsed;
    if (error && error_size) error[0] = 0;
    if (!scale || !s_initialized) {
        set_error(
            error, error_size, 0,
            "graphics config is not initialized", NULL);
        return 0;
    }
    override = getenv("SR_D3D12_SCALE");
    if (!override || !override[0]) {
        *scale = s_config.internal_resolution;
        return 1;
    }
    errno = 0;
    parsed = strtoul(override, &end, 10);
    if (errno || !end || *end ||
        parsed < SR_GRAPHICS_SCALE_MIN ||
        parsed > SR_GRAPHICS_SCALE_MAX) {
        set_error(
            error, error_size, 0,
            "SR_D3D12_SCALE must be an integer from 1 to 8",
            override);
        return 0;
    }
    *scale = (uint32_t)parsed;
    return 1;
}

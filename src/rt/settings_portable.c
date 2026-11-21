#include "graphics_settings.h"
#include "input_settings.h"

#include "graphics_config.h"
#include "input_config.h"

#include <stdio.h>

int sr_graphics_settings_show(const char *title) {
    (void)title;
    fprintf(
        stderr,
        "graphics: native settings dialog is unavailable on this platform; "
        "edit %s\n",
        sr_graphics_config_path()[0]
            ? sr_graphics_config_path() : "graphics.ini");
    return 0;
}

int sr_input_settings_show(const char *title, int allow_launch) {
    (void)title;
    (void)allow_launch;
    fprintf(
        stderr,
        "input: native settings dialog is unavailable on this platform; "
        "edit %s\n",
        sr_input_config_path()[0]
            ? sr_input_config_path() : "input.ini");
    return 0;
}

/* Whether the game window starts full screen. */
#ifndef PSP_RECOMP_WINDOW_MODE_H
#define PSP_RECOMP_WINDOW_MODE_H

#include <stdlib.h>
#include <string.h>

/* SR_FULLSCREEN=1 (set by --fullscreen) or 0 decides. Otherwise the window starts full screen
 * when RecompLauncher runs in Big Picture mode (RECOMPLAUNCHER_MODE=bigpicture). F11 toggles
 * it in every backend. */
static inline int sr_start_fullscreen(void) {
    const char *value = getenv("SR_FULLSCREEN");
    if (value && value[0]) return strcmp(value, "0") != 0;
    value = getenv("RECOMPLAUNCHER_MODE");
    return value && _stricmp(value, "bigpicture") == 0;
}

#endif

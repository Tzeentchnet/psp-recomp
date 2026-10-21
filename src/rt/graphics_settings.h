#ifndef PSP_RECOMP_GRAPHICS_SETTINGS_H
#define PSP_RECOMP_GRAPHICS_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 after saving, 0 when cancelled, and -1 on an error. */
int sr_graphics_settings_show(const char *title);

#ifdef __cplusplus
}
#endif

#endif

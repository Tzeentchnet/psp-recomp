#ifndef PSP_RECOMP_MACOS_APP_H
#define PSP_RECOMP_MACOS_APP_H

#ifdef __cplusplus
extern "C" {
#endif

void sr_macos_show_error(const char *title, const char *message);
void sr_macos_prepare_runtime(void);
int sr_macos_ensure_iso(const char *title);
int sr_macos_thermal_state(void);

#ifdef __cplusplus
}
#endif

#endif

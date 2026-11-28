#ifndef SHIGUANG_DESKTOP_WIN_H
#define SHIGUANG_DESKTOP_WIN_H
#include <windows.h>
#include <stdint.h>

#define SG_RECORD_MESSAGE (WM_APP + 20)
#define SG_TRAY_MESSAGE (WM_APP + 21)
#define SG_UPDATE_MESSAGE (WM_APP + 22)

int sg_start(void);  // 0 started, 1 already running, -1 fatal error
void sg_stop(void);
int sg_bind_settings(HWND window);
void sg_settings_caption(int close);
void sg_style_settings(void);
int sg_settings_dark(void);
int sg_is_select_popup_dc(HDC dc);
uint32_t sg_settings_popup_color(int slot);
void sg_ready(int background);
void sg_update_set_available(int available);
void sg_update_notice(const wchar_t *message);
int sg_update_can_restart(void);
int sg_update_ui_ready(void);
void sg_update_exit(void);
int sg_startup_state(void);  // 0 absent, 1 ours, 2 other installation, 3 unreadable
int sg_set_startup(int enable);  // 1 success
void sg_after_dispatch(void);
uint32_t sg_hotkey(void);
int sg_record_hotkey(int recording);  // 0 if the foreground recorder could not start
int sg_set_hotkey(uint32_t chord);  // 0 OK, 1 occupied, 2 could not persist, 3 invalid, 4 other
void sg_exit(void);
void sg_fatal(const char *message);
#endif

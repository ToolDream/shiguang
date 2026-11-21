#ifndef SHIGUANG_DESKTOP_WIN_H
#define SHIGUANG_DESKTOP_WIN_H
#include <windows.h>
#include <stdint.h>

#define SG_RECORD_MESSAGE (WM_APP + 20)
#define SG_TRAY_MESSAGE (WM_APP + 21)

int sg_start(void);  // 0 started, 1 already running, -1 fatal error
void sg_stop(void);
int sg_bind_settings(HWND window);
void sg_ready(void);
void sg_after_dispatch(void);
uint32_t sg_hotkey(void);
int sg_record_hotkey(int recording);  // 0 if the foreground recorder could not start
int sg_set_hotkey(uint32_t chord);  // 0 OK, 1 unavailable, 2 could not persist
void sg_exit(void);
void sg_fatal(const char *message);
#endif

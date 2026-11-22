#define UNICODE
#include "desktop_win.h"
#include "export_win.h"
#include "editor_win.h"
#include <commctrl.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define APP_TITLE L"拾光截图"
#define REG_KEY L"Software\\Shiguang"
#define OWNER_CLASS L"Shiguang.Controller"
#define CAPTURE_CLASS L"Shiguang.FrozenMonitor"
enum { TRAY_MSG = WM_APP + 1, COMMAND_MSG, PREVIEW_WAKE, HOTKEY_A = 1, HOTKEY_B = 2, HOTKEY_CANCEL = 3 };
enum { CMD_CAPTURE = 1, CMD_COPY, CMD_SAVE, CMD_SETTINGS, CMD_EXIT };
enum Phase { IDLE, PREPARING, SELECTING, EDITING, OUTPUT, DRAINING };
enum { MAX_DISPLAYS = 64 };

typedef struct {
    HMONITOR monitor;
    Rect bounds;
} Monitor;

typedef struct {
    Monitor items[MAX_DISPLAYS];
    size_t count;
} Layout;

typedef struct {
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ previous;
    HWND window;
} Surface;

static HINSTANCE instance;
static HANDLE single_instance;
static HWND owner, settings;
static BOOL com, tray_ready, menu_open;
static UINT taskbar_created;
static uint32_t shortcut = HOTKEY_DEFAULT;
static int active_hotkey;
static BOOL recording_hotkey;
static HHOOK recording_hook;
static enum Phase phase;
static BOOL layout_invalid, dragging, escape_registered, preview_failed;
static unsigned paint_depth;
static BOOL wide_desktop;
static POINT anchor;
static Rect selection, desktop_bounds, adjust_area;
static POINT adjust_anchor;
static int adjust_mode;
static EditPoint mark_origin; // Fixed when the editor opens, independent of later cuts.
static Layout layout;
static Surface surfaces[MAX_DISPLAYS];
static Frame frames[MAX_DISPLAYS];
static Image recent, base;
static Canvas canvas, shade;
static History history;
static Mark draft;
static BOOL drawing, composition_dirty, ime_composing;
static HWND text_input;
static HFONT text_font;
static Panel toolbar, popup;
static Dock bar_dock;
static Rect bar_screen, escape_hint;
static size_t bar_monitor;
static int editor_gap, edit_color, popup_kind;
static Tool tool, shape;
static HICON small_icon, large_icon;

static BOOL capture_active(void) { return phase == SELECTING || phase == EDITING; }
static void editor_action(int action);
static void begin_editor(HWND focus);
static BOOL start_adjustment(HWND w, int mode, POINT p);
static void capture_escape(void);

static void drain_hotkeys(void) {
    MSG message;
    for (unsigned n = 0; n < 64; ++n) {
        if (!PeekMessageW(&message, owner, WM_HOTKEY, WM_HOTKEY, PM_REMOVE | PM_QS_POSTMESSAGE)) {
            phase = IDLE;
            return;
        }
        if (message.message == WM_QUIT) {
            PostQuitMessage((int)message.wParam);
            return;
        }
    }
    // At exactly the budget the queue may already be empty. Do not sacrifice
    // the first new hotkey just to wake an otherwise idle message loop.
    if (!PeekMessageW(&message, owner, WM_HOTKEY, WM_HOTKEY, PM_NOREMOVE | PM_QS_POSTMESSAGE))
        phase = IDLE;
}

static void idle(void) {
    phase = DRAINING;
    drain_hotkeys();
}

static void cancel_capture(void);

void sg_after_dispatch(void) {
    if (paint_depth) return;
    if (capture_active() && (preview_failed || layout_invalid)) cancel_capture();
    if (phase == DRAINING) drain_hotkeys();
}

static NOTIFYICONDATAW tray_data(void) {
    NOTIFYICONDATAW data = {.cbSize = sizeof data, .hWnd = owner, .uID = 1};
    return data;
}

static void notice(const wchar_t *message) {
    if (!tray_ready) return;
    NOTIFYICONDATAW data = tray_data();
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_WARNING;
    wcscpy(data.szInfoTitle, APP_TITLE);
    wcsncpy(data.szInfo, message, ARRAYSIZE(data.szInfo) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

static BOOL add_tray(void) {
    NOTIFYICONDATAW data = tray_data();
    data.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    data.uCallbackMessage = TRAY_MSG;
    data.hIcon = small_icon;
    wcscpy(data.szTip, APP_TITLE);
    if (!Shell_NotifyIconW(NIM_ADD, &data)) return FALSE;
    data.uVersion = NOTIFYICON_VERSION_4;
    if (Shell_NotifyIconW(NIM_SETVERSION, &data)) return TRUE;
    Shell_NotifyIconW(NIM_DELETE, &data);
    return FALSE;
}

static void invalidate_theme_windows(void) {
    if (settings) RedrawWindow(settings, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    if (toolbar.window) RedrawWindow(toolbar.window, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
    if (popup.window) RedrawWindow(popup.window, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
    if (text_input) InvalidateRect(text_input, NULL, TRUE);
    for (size_t i = 0; i < layout.count; ++i)
        if (surfaces[i].window) InvalidateRect(surfaces[i].window, NULL, FALSE);
}

static void sync_ui_theme(void) {
    if (!refresh_ui_palette()) return;
    HICON next_small = shiguang_icon(GetSystemMetrics(SM_CXSMICON));
    HICON next_large = shiguang_icon(GetSystemMetrics(SM_CXICON));
    if (next_small && next_large) {
        if (settings) {
            SendMessageW(settings, WM_SETICON, ICON_SMALL, (LPARAM)next_small);
            SendMessageW(settings, WM_SETICON, ICON_BIG, (LPARAM)next_large);
        }
        if (tray_ready) {
            NOTIFYICONDATAW data = tray_data();
            data.uFlags = NIF_ICON;
            data.hIcon = next_small;
            Shell_NotifyIconW(NIM_MODIFY, &data);
        }
        if (small_icon) DestroyIcon(small_icon);
        if (large_icon) DestroyIcon(large_icon);
        small_icon = next_small;
        large_icon = next_large;
    } else {
        if (next_small) DestroyIcon(next_small);
        if (next_large) DestroyIcon(next_large);
    }
    invalidate_theme_windows();
}

static BOOL register_hotkey(int id, uint32_t chord) {
    return RegisterHotKey(owner, id, (chord >> 8) | MOD_NOREPEAT, chord & 255);
}

uint32_t sg_hotkey(void) { return active_hotkey ? shortcut : 0; }

static uint32_t key_chord(unsigned key, BOOL asynchronous) {
    if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_LWIN || key == VK_RWIN ||
        (key >= VK_LSHIFT && key <= VK_RMENU)) return 0;
    SHORT (WINAPI *pressed)(int) = asynchronous ? GetAsyncKeyState : GetKeyState;
    UINT mods = (pressed(VK_SHIFT) < 0 ? MOD_SHIFT : 0) |
                (pressed(VK_CONTROL) < 0 ? MOD_CONTROL : 0) |
                (pressed(VK_MENU) < 0 ? MOD_ALT : 0) |
                (pressed(VK_LWIN) < 0 || pressed(VK_RWIN) < 0 ? MOD_WIN : 0);
    return (mods << 8) | key;
}

static LRESULT CALLBACK record_key(int code, WPARAM msg, LPARAM data) {
    // Windows routes an occupied chord to its WM_HOTKEY owner before normal
    // key messages. Intercept only the explicit, foreground recording gesture.
    if (code == HC_ACTION && recording_hotkey && settings &&
        GetForegroundWindow() == settings && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
        DWORD key = ((const KBDLLHOOKSTRUCT *)data)->vkCode;
        uint32_t chord = key_chord(key, TRUE);
        if ((key == VK_ESCAPE || chord) &&
            PostMessageW(settings, SG_RECORD_MESSAGE, key == VK_ESCAPE ? 0 : chord, 0)) {
            recording_hotkey = FALSE;
            return 1;
        }
    }
    return CallNextHookEx(NULL, code, msg, data);
}

int sg_record_hotkey(int recording) {
    if (!recording) {
        recording_hotkey = FALSE;
        if (recording_hook) UnhookWindowsHookEx(recording_hook);
        recording_hook = NULL;
        return 1;
    }
    if (!settings || GetForegroundWindow() != settings) return 0;
    if (!recording_hook)
        recording_hook = SetWindowsHookExW(WH_KEYBOARD_LL, record_key, instance, 0);
    recording_hotkey = recording_hook != NULL;
    return recording_hotkey;
}

int sg_set_hotkey(uint32_t chord) {
    if (!valid_hotkey(chord)) return 3;
    if (phase != IDLE) return 4;
    if (sg_hotkey() == chord) return 0;
    int next_id = active_hotkey == HOTKEY_A ? HOTKEY_B : HOTKEY_A;
    if (!register_hotkey(next_id, chord))
        return GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED ? 1 : 4;
    DWORD value = HOTKEY_TAG | chord;
    if (RegSetKeyValueW(HKEY_CURRENT_USER, REG_KEY, L"Hotkey", REG_DWORD,
                       &value, sizeof value) != ERROR_SUCCESS) {
        UnregisterHotKey(owner, next_id);
        return 2;
    }
    if (active_hotkey) UnregisterHotKey(owner, active_hotkey);
    active_hotkey = next_id;
    shortcut = chord;
    return 0;
}

// The app owns only an exact command for this executable, never Windows'
// StartupApproved state or another installation's Run entry.
#define STARTUP_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define STARTUP_NAME L"Shiguang.Screenshot"
enum StartupState { STARTUP_OFF, STARTUP_ON, STARTUP_FOREIGN, STARTUP_UNKNOWN };
enum { STARTUP_COMMAND_CAP = 260 };

static BOOL startup_command(const WCHAR *exe, WCHAR out[STARTUP_COMMAND_CAP]) {
    if (!*exe || wcschr(exe, L'"') || wcslen(exe) + ARRAYSIZE(L"\"\" --startup") > STARTUP_COMMAND_CAP)
        return FALSE;
    return swprintf(out, STARTUP_COMMAND_CAP, L"\"%ls\" --startup", exe) > 0;
}

static BOOL current_startup_command(WCHAR out[STARTUP_COMMAND_CAP]) {
    WCHAR exe[STARTUP_COMMAND_CAP];
    DWORD size = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    return size && size < ARRAYSIZE(exe) && startup_command(exe, out);
}

static enum StartupState startup_state(HKEY key, const WCHAR *command) {
    WCHAR stored[STARTUP_COMMAND_CAP];
    DWORD type = 0, bytes = sizeof stored;
    LSTATUS status = RegQueryValueExW(key, STARTUP_NAME, NULL, &type, (BYTE *)stored, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) return STARTUP_OFF;
    if (status == ERROR_MORE_DATA) return STARTUP_FOREIGN;
    if (status != ERROR_SUCCESS) return STARTUP_UNKNOWN;
    size_t expected = (wcslen(command) + 1) * sizeof(WCHAR);
    return type == REG_SZ && bytes == expected && !memcmp(stored, command, expected)
        ? STARTUP_ON : STARTUP_FOREIGN;
}

static LSTATUS startup_apply(HKEY key, const WCHAR *command, BOOL enable) {
    enum StartupState state = startup_state(key, command);
    if (state == STARTUP_UNKNOWN) return ERROR_READ_FAULT;
    if (state == STARTUP_FOREIGN) return ERROR_ALREADY_EXISTS;
    if ((state == STARTUP_ON) == enable) return ERROR_SUCCESS;
    if (!enable) return RegDeleteValueW(key, STARTUP_NAME);
    return RegSetValueExW(key, STARTUP_NAME, 0, REG_SZ, (const BYTE *)command,
                         (DWORD)((wcslen(command) + 1) * sizeof(WCHAR)));
}

int sg_startup_state(void) {
    WCHAR command[STARTUP_COMMAND_CAP];
    if (!current_startup_command(command)) return STARTUP_UNKNOWN;
    HKEY key;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_KEY, 0, KEY_QUERY_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND) return STARTUP_OFF;
    if (status != ERROR_SUCCESS) return STARTUP_UNKNOWN;
    int state = startup_state(key, command);
    RegCloseKey(key);
    return state;
}

int sg_set_startup(int enable) {
    WCHAR command[STARTUP_COMMAND_CAP];
    if (!current_startup_command(command)) return 0;
    HKEY key;
    REGSAM access = KEY_QUERY_VALUE | KEY_SET_VALUE;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_KEY, 0, access, &key);
    if (status == ERROR_FILE_NOT_FOUND) {
        if (!enable) return 1;
        status = RegCreateKeyExW(HKEY_CURRENT_USER, STARTUP_KEY, 0, NULL, REG_OPTION_NON_VOLATILE,
                                 access, NULL, &key, NULL);
    }
    if (status != ERROR_SUCCESS) return 0;
    status = startup_apply(key, command, enable != 0);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

static BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC ignored, LPRECT r, LPARAM arg) {
    (void)ignored;
    Layout *out = (Layout *)arg;
    if (!out || !r || out->count == MAX_DISPLAYS) return FALSE;
    if (r->right <= r->left || r->bottom <= r->top) return TRUE;
    out->items[out->count++] = (Monitor){monitor, {r->left, r->top, r->right, r->bottom}};
    return TRUE;
}

static BOOL read_layout(Layout *out) {
    out->count = 0;
    return EnumDisplayMonitors(NULL, NULL, collect_monitor, (LPARAM)out) && out->count != 0;
}

static BOOL same_rect(Rect a, Rect b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

static BOOL unchanged_layout(void) {
    Layout now;
    if (!read_layout(&now) || now.count != layout.count) return FALSE;
    for (size_t i = 0; i < layout.count; ++i) {
        BOOL found = FALSE;
        for (size_t j = 0; j < now.count; ++j)
            if (layout.items[i].monitor == now.items[j].monitor &&
                same_rect(layout.items[i].bounds, now.items[j].bounds)) found = TRUE;
        if (!found) return FALSE;
    }
    return TRUE;
}

static BITMAPINFO bitmap_info(int width, int height) {
    return (BITMAPINFO){.bmiHeader = {
        .biSize = sizeof(BITMAPINFOHEADER), .biWidth = width, .biHeight = -height,
        .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB}};
}

static BOOL create_surface(HDC screen, size_t i) {
    Rect b = layout.items[i].bounds;
    int64_t w = (int64_t)b.right - b.left, h = (int64_t)b.bottom - b.top;
    if (!image_bytes(w, h)) return FALSE;
    Surface *s = &surfaces[i];
    s->dc = CreateCompatibleDC(screen);
    if (!s->dc) return FALSE;
    BITMAPINFO info = bitmap_info((int)w, (int)h);
    void *pixels;
    s->bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!s->bitmap) return FALSE;
    s->previous = SelectObject(s->dc, s->bitmap);
    if (!s->previous) return FALSE;
    if (!BitBlt(s->dc, 0, 0, (int)w, (int)h, screen, b.left, b.top, SRCCOPY | CAPTUREBLT))
        return FALSE;
    frames[i] = (Frame){b, pixels};
    return TRUE;
}

static BOOL open_shade(void) {
    if (!canvas_open(&shade, &(Image){.width = 1, .height = 1})) {
        canvas_close(&shade);
        return FALSE;
    }
    memset(shade.image.bgra, 0, 4);
    return TRUE;
}

static BOOL freeze_desktop(void) {
    if (!read_layout(&layout)) return FALSE;
    Rect extent = layout.items[0].bounds;
    for (size_t i = 1; i < layout.count; ++i) {
        Rect b = layout.items[i].bounds;
        if (b.left < extent.left) extent.left = b.left;
        if (b.top < extent.top) extent.top = b.top;
        if (b.right > extent.right) extent.right = b.right;
        if (b.bottom > extent.bottom) extent.bottom = b.bottom;
    }
    int64_t w = (int64_t)extent.right - extent.left, h = (int64_t)extent.bottom - extent.top;
    if (w > INT_MAX || h > INT_MAX) return FALSE;
    desktop_bounds = extent;
    wide_desktop = w > INT16_MAX || h > INT16_MAX;
    HDC screen = GetDC(NULL);
    if (!screen) return FALSE;
    BOOL ok = TRUE;
    for (size_t i = 0; ok && i < layout.count; ++i) ok = create_surface(screen, i);
    ok = GdiFlush() && ok && open_shade();
    ReleaseDC(NULL, screen);
    return ok;
}

static void close_text(void) {
    HWND w = text_input;
    text_input = NULL;
    ime_composing = FALSE;
    if (w) DestroyWindow(w);
    if (text_font) DeleteObject(text_font);
    text_font = NULL;
}

static void close_overlays(void) {
    dragging = drawing = FALSE;
    adjust_mode = 0;
    close_text();
    close_panel(&popup);
    close_panel(&toolbar);
    popup_kind = 0;
    escape_hint = (Rect){0};
    canvas_close(&canvas);
    canvas_close(&shade);
    history_clear(&history);
    mark_clear(&draft);
    free(base.bgra);
    base = (Image){0};
    if (escape_registered) {
        UnregisterHotKey(owner, HOTKEY_CANCEL);
        escape_registered = FALSE;
    }
    HWND captured = GetCapture();
    for (size_t i = 0; captured && i < layout.count; ++i) {
        if (surfaces[i].window == captured) {
            ReleaseCapture();
            break;
        }
    }
    for (size_t i = 0; i < layout.count; ++i) {
        Surface *s = &surfaces[i];
        if (s->window) DestroyWindow(s->window);
        if (s->previous) SelectObject(s->dc, s->previous);
        if (s->bitmap) DeleteObject(s->bitmap);
        if (s->dc) DeleteDC(s->dc);
        *s = (Surface){0};
        frames[i] = (Frame){0};
    }
    layout.count = 0;
}

static void preview_failure(void) {
    if (!preview_failed) {
        preview_failed = TRUE;
        PostMessageW(owner, PREVIEW_WAKE, 0, 0);
    }
}

static void cancel_capture(void) {
    if (phase == PREPARING || paint_depth || (phase == OUTPUT && base.bgra)) {
        layout_invalid = TRUE;
        PostMessageW(owner, PREVIEW_WAKE, 0, 0);
        return;
    }
    if (!capture_active()) return;
    phase = OUTPUT; // ReleaseCapture/DestroyWindow can dispatch synchronous notifications.
    close_overlays();
    if (preview_failed) notice(L"无法绘制截图预览，已取消截图。");
    idle();
}

static BOOL flush_overlays(void) {
    // UpdateWindow sends WM_PAINT synchronously. Keep its surfaces alive through
    // the whole pass and re-check the latch before arming or committing.
    ++paint_depth;
    for (size_t i = 0; !preview_failed && !layout_invalid && i < layout.count; ++i)
        if (!UpdateWindow(surfaces[i].window)) preview_failure();
    --paint_depth;
    return !preview_failed && !layout_invalid;
}

static void copy_recent(void) {
    if (!publish_image(owner, &recent))
        notice(L"无法写入剪贴板。截图已保留，可从托盘重试复制或另存 PNG。");
}

static void invalidate_selection(Rect r) {
    for (size_t i = 0; i < layout.count; ++i) {
        Rect b = frames[i].bounds, clip = intersect_rect(r, b);
        if (rect_empty(clip)) continue;
        RECT local = {clip.left - b.left, clip.top - b.top, clip.right - b.left, clip.bottom - b.top};
        if (!InvalidateRect(surfaces[i].window, &local, FALSE)) preview_failure();
    }
}

static Rect visible_selection(void) {
    return adjust_mode ? adjust_area : selection;
}

static Rect grip_rect(EditPoint p) {
    return (Rect){p.x - GRIP_RADIUS, p.y - GRIP_RADIUS,
                  p.x + GRIP_RADIUS + 1, p.y + GRIP_RADIUS + 1};
}

static void invalidate_grips(Rect area) {
    EditPoint grips[SELECTION_GRIPS];
    selection_grips(area, desktop_bounds, grips);
    for (int i = 0; i < SELECTION_GRIPS; ++i) invalidate_selection(grip_rect(grips[i]));
}

static void position_hint(BOOL editing) {
    invalidate_selection(escape_hint);
    escape_hint = (Rect){0};
    Rect area = visible_selection();
    if (rect_empty(area)) return;
    for (size_t i = 0; i < layout.count; ++i) {
        Rect b = frames[i].bounds;
        if (area.left < b.left || area.left >= b.right || area.top > b.bottom) continue;
        int dpi = (int)GetDpiForWindow(surfaces[i].window);
        if (!dpi) dpi = 96;
        int w = MulDiv(editing ? 136 : 108, dpi, 96), h = MulDiv(24, dpi, 96);
        int gap = MulDiv(5, dpi, 96);
        int x = area.left, y = area.top - h - gap;
        if (b.right - b.left < w || b.bottom - b.top < h) continue;
        if (x > b.right - w) x = b.right - w;
        if (y < b.top) y = b.top + gap;
        if (y > b.bottom - h) continue;
        Rect hint = {x, y, x + w, y + h};
        if (editing && !rect_empty(intersect_rect(hint, bar_dock.bounds))) continue;
        escape_hint = hint;
        invalidate_selection(hint);
        break;
    }
}

static BOOL pointer_position(HWND window, LPARAM lp, POINT *p) {
    // Normally use the queued event point, not the cursor position after mouse release.
    // shortcut: for >32767 px extents Windows packs/truncates WM_MOUSE coordinates;
    // use the full-width live position there until raw pointer input is needed.
    if (wide_desktop) return GetCursorPos(p);
    *p = (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    return ClientToScreen(window, p);
}

static BOOL update_selection(HWND window, LPARAM lp) {
    POINT p;
    if (!pointer_position(window, lp, &p)) return FALSE;
    Rect next = drag_rect(anchor.x, anchor.y, p.x, p.y);
    if (!same_rect(next, selection)) {
        invalidate_selection(selection);
        selection = next;
        invalidate_selection(selection);
        position_hint(FALSE);
    }
    return TRUE;
}

static BOOL draw_selection_edges(HDC dc, Rect screen, Rect area, Rect dirty) {
    Rect edges[4];
    selection_edges(area, dirty, edges);
    int64_t width = (int64_t)area.right - area.left, height = (int64_t)area.bottom - area.top;
    if (width == 1 && height == 1) {
        int x = area.left - screen.left, y = area.top - screen.top;
        if (!PtVisible(dc, x, y)) return TRUE;
        COLORREF under = GetPixel(dc, x, y);
        // A single pixel cannot contain both colors. Contrast it with the preview.
        return under != CLR_INVALID && SetPixelV(dc, x, y,
            GetRValue(under) + GetGValue(under) + GetBValue(under) >= 384 ? RGB(0, 0, 0) : RGB(255, 255, 255));
    }
    // Anchor both brushes to the same desktop coordinates, not to each
    // monitor's clip rect or repaint origin. Draw only the actual perimeter.
    POINT previous;
    if (!SetBrushOrgEx(dc, (int)(((int64_t)area.left - screen.left) & 7),
                           (int)(((int64_t)area.top - screen.top) & 7), &previous)) return FALSE;
    BOOL ok = TRUE;
    for (int i = 0; i < 4 && ok; i += 2) {
        if (rect_empty(edges[i]) && rect_empty(edges[i + 1])) continue;
        struct { BITMAPINFOHEADER header; DWORD pixels[64]; } tile = {
            .header = bitmap_info(8, 8).bmiHeader
        };
        tile.header.biHeight = 8; // Bottom-up DIB for pattern brush realization.
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                tile.pixels[(7 - y) * 8 + x] =
                    selection_dash_white(i == 0 ? width : height, i == 0 ? x : y, i == 0 ? y : x)
                    ? 0xffffffu : 0;
        HBRUSH brush = CreateDIBPatternBrushPt(&tile, DIB_RGB_COLORS);
        if (!brush) {
            ok = FALSE;
            break;
        }
        for (int j = i; j < i + 2 && ok; ++j) {
            Rect edge = edges[j];
            if (!rect_empty(edge)) {
                RECT r = {edge.left - screen.left, edge.top - screen.top,
                          edge.right - screen.left, edge.bottom - screen.top};
                ok = FillRect(dc, &r, brush) != 0;
            }
        }
        DeleteObject(brush);
    }
    return SetBrushOrgEx(dc, previous.x, previous.y, NULL) && ok;
}

static BOOL shade_preview(HDC dc, const RECT *dirty, Rect screen, Rect selected) {
    const BLENDFUNCTION dim = {AC_SRC_OVER, 0, 51, 0}; // 20% black
    int w = dirty->right - dirty->left, h = dirty->bottom - dirty->top;
    if (rect_empty(selected))
        return AlphaBlend(dc, dirty->left, dirty->top, w, h, shade.dc, 0, 0, 1, 1, dim);
    RECT inside = {selected.left - screen.left, selected.top - screen.top,
                   selected.right - screen.left, selected.bottom - screen.top};
    if (EqualRect(&inside, dirty)) return TRUE;
    int saved = SaveDC(dc);
    if (!saved) return FALSE;
    int clip = ExcludeClipRect(dc, inside.left, inside.top, inside.right, inside.bottom);
    BOOL ok = clip != ERROR && (clip == NULLREGION ||
        AlphaBlend(dc, dirty->left, dirty->top, w, h, shade.dc, 0, 0, 1, 1, dim));
    return RestoreDC(dc, saved) && ok;
}

static BOOL draw_preview(HDC dc, const RECT *dirty, size_t i) {
    int x = dirty->left, y = dirty->top;
    int w = dirty->right - x, h = dirty->bottom - y;
    if (!w || !h) return TRUE;
    Rect b = frames[i].bounds;
    if (!BitBlt(dc, x, y, w, h, surfaces[i].dc, x, y, SRCCOPY)) {
        // A memory DC may not be compatible with a window on another adapter.
        BITMAPINFO info = bitmap_info(b.right - b.left, b.bottom - b.top);
        if (StretchDIBits(dc, x, y, w, h, x, y, w, h, frames[i].bgra,
                         &info, DIB_RGB_COLORS, SRCCOPY) <= 0) return FALSE;
    }

    Rect update = {b.left + dirty->left, b.top + dirty->top, b.left + dirty->right, b.top + dirty->bottom};
    Rect area = dragging || base.bgra ? visible_selection() : (Rect){0};
    Rect selected = intersect_rect(area, update);
    // Recompose from frozen pixels on every invalidation, then darken only
    // the dirty pixels outside the latest selection. Never touch base/canvas.
    if (!shade_preview(dc, dirty, b, selected)) return FALSE;
    if (rect_empty(selected)) return TRUE;

    if (base.bgra) {
        // Live bounds paint on the overlay only. Allocate/re-crop once on release.
        if (!adjust_mode && composition_dirty) {
            if (!canvas_render(&canvas, &base, &history, drawing ? &draft : NULL,
                (EditPoint){mark_origin.x - selection.left, mark_origin.y - selection.top})) return FALSE;
            composition_dirty = FALSE;
        }
        int l = selected.left - b.left, t = selected.top - b.top;
        int r = selected.right - b.left, bot = selected.bottom - b.top;
        int saved_clip = SaveDC(dc);
        if (!saved_clip) return FALSE;
        int clip = IntersectClipRect(dc, l, t, r, bot);
        BOOL ok = clip != ERROR;
        if (ok && clip != NULLREGION) {
            if (adjust_mode) {
                ok = paint_marks(dc, &history, NULL, (EditPoint){mark_origin.x - b.left, mark_origin.y - b.top});
            } else if (!BitBlt(dc, l, t, r - l, bot - t, canvas.dc,
                               selected.left - selection.left, selected.top - selection.top, SRCCOPY)) {
                BITMAPINFO info = bitmap_info(base.width, base.height);
                int drawn = StretchDIBits(dc, selection.left - b.left, selection.top - b.top,
                    base.width, base.height, 0, 0, base.width, base.height,
                    canvas.image.bgra, &info, DIB_RGB_COLORS, SRCCOPY);
                ok = drawn != (int)GDI_ERROR && drawn > 0;
            }
        }
        if (!RestoreDC(dc, saved_clip) || !ok) return FALSE;
    }
    // Composite/export pixels are finalized above; the outline only touches
    // the preview window DC, including after the selection has been locked.
    return draw_selection_edges(dc, b, area, selected);
}

static void paint_grips(HDC dc, Rect screen) {
    if (phase != EDITING || adjust_mode || !base.bgra) return;
    EditPoint grips[SELECTION_GRIPS];
    selection_grips(selection, desktop_bounds, grips);
    for (int i = 0; i < SELECTION_GRIPS; ++i) {
        Rect a = grip_rect(grips[i]);
        if (rect_empty(intersect_rect(a, screen))) continue;
        RECT r = {a.left - screen.left, a.top - screen.top, a.right - screen.left, a.bottom - screen.top};
        FillRect(dc, &r, GetStockObject(BLACK_BRUSH));
        InflateRect(&r, -1, -1);
        FillRect(dc, &r, GetStockObject(WHITE_BRUSH));
    }
}

static void paint_escape_hint(HDC dc, size_t i) {
    Rect b = frames[i].bounds;
    if (rect_empty(intersect_rect(b, escape_hint))) return;
    RECT box = {escape_hint.left - b.left, escape_hint.top - b.top,
                escape_hint.right - b.left, escape_hint.bottom - b.top};
    int saved = SaveDC(dc);
    if (!saved) return;
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, ui_colors.hint);
    SetDCPenColor(dc, ui_colors.high_contrast ? ui_colors.ink : ui_colors.hint);
    int radius = (box.bottom - box.top) / 3;
    RoundRect(dc, box.left, box.top, box.right, box.bottom, radius, radius);
    HFONT font = mark_font((box.bottom - box.top) / 2);
    if (font) SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ui_colors.hint_ink);
    wchar_t label[64];
    Rect area = visible_selection();
    if (swprintf(label, ARRAYSIZE(label), base.bgra ? L"%d × %d  ·  Esc" : L"%d × %d",
        area.right - area.left, area.bottom - area.top) > 0)
        DrawTextW(dc, label, -1, &box, DT_VCENTER | DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
    RestoreDC(dc, saved);
    if (font) DeleteObject(font);
}

static void paint_overlay(HWND window, size_t i) {
    ++paint_depth;
    PAINTSTRUCT ps = {0};
    HDC dc = BeginPaint(window, &ps);
    BOOL showing = phase == PREPARING || capture_active() || (phase == OUTPUT && base.bgra);
    if (showing && !preview_failed && !layout_invalid) {
        if (!dc || i >= layout.count || !draw_preview(dc, &ps.rcPaint, i)) preview_failure();
        else {
            paint_escape_hint(dc, i);
            paint_grips(dc, frames[i].bounds);
            if (!GdiFlush()) preview_failure();
        }
    }
    if (dc && !EndPaint(window, &ps) && showing) preview_failure();
    --paint_depth;
}

static void edit_changed(void) {
    composition_dirty = TRUE;
    invalidate_selection(selection);
}

static void sync_tools(void) {
    update_panel(&toolbar, tool, shape, edit_color, history.used != 0, history.used < history.total);
}

static int clamp_to(int n, int lo, int hi) {
    return n < lo ? lo : n > hi ? hi : n;
}

static void discard_draft(void) {
    drawing = FALSE;
    mark_clear(&draft);
    edit_changed();
}

static void commit_draft(void) {
    drawing = FALSE;
    if (!history_add(&history, &draft)) {
        mark_clear(&draft);
        notice(L"内存不足，未加入本次标注。");
    }
    edit_changed();
    sync_tools();
}

static BOOL commit_text(void) {
    if (!text_input) return TRUE;
    if (ime_composing) return FALSE;
    int n = GetWindowTextLengthW(text_input);
    if (!n) {
        HWND parent = GetParent(text_input);
        close_text();
        SetFocus(parent);
        mark_clear(&draft);
        return TRUE;
    }
    wchar_t *value = malloc(((size_t)n + 1) * sizeof *value);
    if (!value || GetWindowTextW(text_input, value, n + 1) != n) {
        free(value);
        notice(L"无法读取本次文字，请重试。");
        return FALSE;
    }
    draft.text = value;
    if (!history_add(&history, &draft)) {
        free(draft.text);
        draft.text = NULL;
        notice(L"内存不足，文字尚未加入，请重试。");
        return FALSE;
    }
    HWND parent = GetParent(text_input);
    close_text();
    SetFocus(parent);
    edit_changed();
    sync_tools();
    return TRUE;
}

static LRESULT CALLBACK text_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp,
                                 UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    if (msg == WM_IME_STARTCOMPOSITION) ime_composing = TRUE;
    if (msg == WM_IME_ENDCOMPOSITION) ime_composing = FALSE;
    if (msg == WM_LBUTTONDOWN && GetKeyState(VK_MENU) < 0) {
        POINT p;
        if (pointer_position(w, lp, &p) &&
            selection_hit(selection, desktop_bounds, (EditPoint){p.x, p.y}, TRUE) == SEL_MOVE) {
            start_adjustment(GetParent(w), SEL_MOVE, p);
            return 0;
        }
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wp == VK_ESCAPE) {
        BOOL repeat = (lp & (1L << 30)) != 0;
        if (adjust_mode || (!ime_composing && (msg == WM_KEYDOWN || repeat))) {
            if (!repeat) capture_escape();
            return 0;
        }
    }
    if (msg == WM_KEYDOWN && !ime_composing && wp == VK_RETURN) {
        if (!adjust_mode) commit_text();
        return 0;
    }
    if (((msg == WM_CHAR || msg == WM_SYSCHAR) && wp == 27) ||
        (msg == WM_CHAR && (wp == L'\r' || wp == 19))) return 0;
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(w, text_proc, id);
    return DefSubclassProc(w, msg, wp, lp);
}

static void start_mark(HWND w, POINT pos) {
    if (pos.x < selection.left || pos.x >= selection.right ||
        pos.y < selection.top || pos.y >= selection.bottom || !commit_text()) return;
    if (escape_registered && GetForegroundWindow() == w &&
        UnregisterHotKey(owner, HOTKEY_CANCEL)) escape_registered = FALSE;
    UINT dpi = GetDpiForWindow(w);
    if (!dpi) dpi = 96;
    int x = pos.x - mark_origin.x, y = pos.y - mark_origin.y;
    int width = MulDiv(tool == TEXT ? 22 : 3, (int)dpi, 96);
    draft = (Mark){.tool = tool, .color = palette[edit_color], .width = width > 0 ? width : 1,
                   .x = x, .y = y, .end_x = x, .end_y = y};
    if (tool == TEXT) {
        RECT r;
        if (!GetClientRect(w, &r) || !ScreenToClient(w, &pos)) return;
        int iw = clamp_to(MulDiv(300, (int)dpi, 96), 1, r.right);
        int ih = clamp_to(MulDiv(35, (int)dpi, 96), 1, r.bottom);
        text_font = mark_font(draft.width);
        text_input = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            clamp_to(pos.x, 0, r.right - iw), clamp_to(pos.y, 0, r.bottom - ih), iw, ih, w,
            NULL, instance, NULL);
        if (!text_font || !text_input || !SetWindowSubclass(text_input, text_proc, 1, 0)) {
            close_text();
            mark_clear(&draft);
            notice(L"无法打开文字输入框，请重试。");
            return;
        }
        SendMessageW(text_input, WM_SETFONT, (WPARAM)text_font, TRUE);
        SendMessageW(text_input, EM_SETLIMITTEXT, 1024, 0);
        SetFocus(text_input);
        return;
    }
    if (tool == PEN && !mark_point(&draft, x, y)) {
        discard_draft();
        notice(L"内存不足，未开始画笔标注。");
        return;
    }
    SetCapture(w);
    if (phase != EDITING) return;
    drawing = GetCapture() == w;
    if (drawing) edit_changed();
    else discard_draft();
}

static BOOL move_mark(POINT pos) {
    int x = clamp_to(pos.x, selection.left, selection.right - 1) - mark_origin.x;
    int y = clamp_to(pos.y, selection.top, selection.bottom - 1) - mark_origin.y;
    if (draft.tool == CIRCLE) {
        int dx = x - draft.x, dy = y - draft.y;
        int d = abs(dx) < abs(dy) ? abs(dx) : abs(dy);
        x = draft.x + (dx < 0 ? -d : d);
        y = draft.y + (dy < 0 ? -d : d);
    }
    if (draft.tool == PEN && !mark_point(&draft, x, y)) return FALSE;
    if (x != draft.end_x || y != draft.end_y) {
        draft.end_x = x;
        draft.end_y = y;
        edit_changed();
    }
    return TRUE;
}

static HWND foreground_overlay(void) {
    HWND w = GetForegroundWindow();
    if (w && (w == toolbar.window || w == popup.window)) return surfaces[bar_monitor].window;
    if (w) for (size_t i = 0; i < layout.count; ++i)
        if (surfaces[i].window == w) return w;
    return NULL;
}

static BOOL editor_save_allowed(void) {
    if (phase != OUTPUT || !base.bgra || layout_invalid || preview_failed) return FALSE;
    if (!unchanged_layout()) layout_invalid = TRUE;
    return !layout_invalid && !preview_failed;
}

static void editor_output(BOOL save) {
    if (drawing || adjust_mode || !commit_text()) return;
    if (paint_depth || !flush_overlays() || !unchanged_layout() || layout_invalid || preview_failed) {
        cancel_capture();
        return;
    }
    size_t bytes = image_bytes(base.width, base.height);
    Image snapshot = {base.width, base.height, malloc(bytes)};
    if (!snapshot.bgra) {
        notice(L"内存不足，编辑仍保留，请重试。");
        return;
    }
    memcpy(snapshot.bgra, canvas.image.bgra, bytes);
    phase = OUTPUT;
    HWND parent = foreground_overlay();
    if (!parent) parent = surfaces[bar_monitor].window;
    if (save) ShowWindow(toolbar.window, SW_HIDE);
    HRESULT result = save ? save_png_dialog(parent, &snapshot, editor_save_allowed) :
        publish_image(owner, &snapshot) ? S_OK : E_FAIL;
    if (result == S_OK) {
        free(recent.bgra);
        recent = snapshot;
        close_overlays();
        idle();
        return;
    }
    free(snapshot.bgra);
    phase = EDITING;
    if (layout_invalid || preview_failed || !unchanged_layout()) {
        cancel_capture();
        return;
    }
    if (save) {
        ShowWindow(toolbar.window, SW_SHOWNOACTIVATE);
        if (GetForegroundWindow() == parent) SetFocus(parent);
    }
    if (FAILED(result)) {
        if (!save) notice(L"无法写入剪贴板，编辑仍保留，可重试复制或保存。");
        else if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) ||
                 result == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS))
            notice(L"文件名已存在，未覆盖。编辑仍保留，请另选文件名。");
        else notice(L"无法保存 PNG，编辑仍保留，请重试。");
    }
}

static void dismiss_popup(void) {
    close_panel(&popup);
    popup_kind = 0;
}

static void editor_action(int action) {
    if (phase != EDITING || drawing || adjust_mode || !commit_text()) return;
    if (action == ACT_SHAPES || action == ACT_COLOR) {
        int prior = popup_kind;
        dismiss_popup();
        if (prior == action + 1) return;
        BOOL shapes = action == ACT_SHAPES;
        int count = shapes ? SHAPES : COLORS;
        Dock where = editor_popup(bar_dock, bar_screen, editor_gap, count);
        if (open_panel(&popup, instance, toolbar.window, where, shapes ? ACT_RECTANGLE : ACT_RED, count, editor_action)) {
            popup_kind = action + 1;
            update_panel(&popup, tool, shape, edit_color, FALSE, FALSE);
        } else notice(L"无法打开标注菜单，请重试。");
        return;
    }
    dismiss_popup();
    if (action == ACT_COPY || action == ACT_SAVE) {
        editor_output(action == ACT_SAVE);
        return;
    }
    switch (action) {
    case ACT_ARROW: tool = ARROW; break;
    case ACT_PEN: tool = PEN; break;
    case ACT_TEXT: tool = TEXT; break;
    case ACT_UNDO:
        if (!history_undo(&history)) return;
        edit_changed();
        break;
    case ACT_REDO:
        if (!history_redo(&history)) return;
        edit_changed();
        break;
    default:
        if (action >= ACT_RECTANGLE && action < ACT_RECTANGLE + SHAPES)
            tool = shape = (Tool)(action - ACT_RECTANGLE);
        else if (action >= ACT_RED && action < ACT_RED + COLORS) edit_color = action - ACT_RED;
        else return;
        break;
    }
    sync_tools();
}

static BOOL place_toolbar(Rect area) {
    Rect screens[MAX_DISPLAYS];
    for (size_t i = 0; i < layout.count; ++i) screens[i] = frames[i].bounds;
    size_t index = editor_screen(area, screens, layout.count);
    if (index == layout.count) return FALSE;
    UINT dpi = GetDpiForWindow(surfaces[index].window);
    if (!dpi) dpi = 96;
    int gap = MulDiv(8, (int)dpi, 96);
    Dock dock = editor_dock(area, screens[index], MulDiv(44, (int)dpi, 96), gap, BAR_BUTTONS);
    if (!dock.cell) return FALSE;
    if (toolbar.window && (!same_rect(bar_dock.bounds, dock.bounds) ||
        bar_dock.cell != dock.cell || bar_dock.cols != dock.cols) && !move_panel(&toolbar, bar_dock, dock)) {
        move_panel(&toolbar, dock, bar_dock);
        return FALSE;
    }
    bar_monitor = index;
    bar_screen = screens[index];
    editor_gap = gap;
    bar_dock = dock;
    return TRUE;
}

static int adjustment_at(POINT p) {
    return selection_hit(selection, desktop_bounds, (EditPoint){p.x, p.y}, GetKeyState(VK_MENU) < 0);
}

static HCURSOR adjustment_cursor(int mode) {
    LPCWSTR id = IDC_CROSS;
    switch (mode) {
    case SEL_MOVE: id = IDC_SIZEALL; break;
    case SEL_N | SEL_W:
    case SEL_S | SEL_E: id = IDC_SIZENWSE; break;
    case SEL_N | SEL_E:
    case SEL_S | SEL_W: id = IDC_SIZENESW; break;
    case SEL_N:
    case SEL_S: id = IDC_SIZENS; break;
    case SEL_E:
    case SEL_W: id = IDC_SIZEWE; break;
    }
    return LoadCursorW(NULL, id);
}

static int active_adjustment_direction(void) {
    int mode = adjust_mode;
    if (((mode & SEL_W) && adjust_area.left == selection.right) ||
        ((mode & SEL_E) && adjust_area.right == selection.left)) mode ^= SEL_W | SEL_E;
    if (((mode & SEL_N) && adjust_area.top == selection.bottom) ||
        ((mode & SEL_S) && adjust_area.bottom == selection.top)) mode ^= SEL_N | SEL_S;
    return mode;
}

static BOOL start_adjustment(HWND w, int mode, POINT p) {
    // Do not steal IME focus or commit a pending text edit (which would drop redo).
    if (phase != EDITING || !mode || drawing || adjust_mode || ime_composing) return FALSE;
    SetCapture(w);
    if (phase != EDITING || GetCapture() != w) return FALSE;
    adjust_anchor = p;
    adjust_area = selection;
    adjust_mode = mode;
    invalidate_grips(selection);
    SetCursor(adjustment_cursor(mode));
    return TRUE;
}

static void update_adjustment(POINT p) {
    Rect next = selection_adjust(selection, desktop_bounds, adjust_mode,
        (int64_t)p.x - adjust_anchor.x, (int64_t)p.y - adjust_anchor.y);
    if (!same_rect(next, adjust_area) &&
        image_bytes((int64_t)next.right - next.left, (int64_t)next.bottom - next.top) &&
        place_toolbar(next)) {
        invalidate_selection(adjust_area);
        adjust_area = next;
        invalidate_selection(adjust_area);
        position_hint(TRUE);
    }
    SetCursor(adjustment_cursor(active_adjustment_direction()));
}

static void finish_adjustment(HWND w, BOOL apply) {
    if (!adjust_mode) return;
    Rect next = adjust_area;
    adjust_mode = 0;
    if (apply && !same_rect(next, selection)) {
        Image new_base = {0};
        Canvas new_canvas = {0};
        if (crop_frozen(next, frames, layout.count, &new_base) == CROP_OK &&
            canvas_open(&new_canvas, &new_base) &&
            canvas_render(&new_canvas, &new_base, &history, NULL,
                (EditPoint){mark_origin.x - next.left, mark_origin.y - next.top})) {
            canvas_close(&canvas);
            free(base.bgra);
            canvas = new_canvas;
            base = new_base;
            selection = next;
            composition_dirty = FALSE;
        } else {
            canvas_close(&new_canvas);
            free(new_base.bgra);
            notice(L"无法调整截图区域，原选区与标注仍保留。");
        }
    }
    if (!same_rect(next, selection)) place_toolbar(selection);
    invalidate_selection(next);
    invalidate_selection(selection);
    position_hint(TRUE);
    invalidate_grips(selection);
    if (w && GetCapture() == w) ReleaseCapture();
}

static void capture_escape(void) {
    if (phase == EDITING && adjust_mode) finish_adjustment(GetCapture(), FALSE);
    else cancel_capture();
}

static void begin_editor(HWND focus) {
    CropResult status = crop_frozen(selection, frames, layout.count, &base);
    if (status != CROP_OK) {
        cancel_capture();
        if (status == CROP_ERROR) notice(L"无法裁切该区域，请缩小选区后重试。");
        return;
    }
    if (!canvas_open(&canvas, &base)) {
        preview_failure();
        cancel_capture();
        return;
    }
    mark_origin = (EditPoint){selection.left, selection.top};
    if (!place_toolbar(selection)) {
        cancel_capture();
        return;
    }
    drawing = dragging = FALSE;
    tool = shape = RECTANGLE;
    edit_color = 0;
    phase = EDITING;
    if (GetCapture()) ReleaseCapture();
    if (phase != EDITING) return;
    if (focus && GetForegroundWindow() == focus) {
        SetFocus(focus);
        if (escape_registered && UnregisterHotKey(owner, HOTKEY_CANCEL)) escape_registered = FALSE;
    }
    position_hint(TRUE);
    edit_changed();
    invalidate_grips(selection);
    if (!flush_overlays() ||
        !open_panel(&toolbar, instance, surfaces[bar_monitor].window, bar_dock, ACT_SHAPES, BAR_BUTTONS, editor_action)) {
        preview_failure();
        cancel_capture();
        return;
    }
    sync_tools();
}

static void finish_capture(void) {
    if (paint_depth || !flush_overlays() || !unchanged_layout() || layout_invalid || preview_failed) {
        cancel_capture();
        return;
    }
    begin_editor(GetCapture());
}

static LRESULT CALLBACK overlay_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        return TRUE;
    }
    if (msg == WM_PAINT) {
        paint_overlay(w, (size_t)GetWindowLongPtrW(w, GWLP_USERDATA));
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_CTLCOLOREDIT && (HWND)lp == text_input && !ui_colors.high_contrast) {
        HDC dc = (HDC)wp;
        SetTextColor(dc, ui_colors.ink);
        SetBkColor(dc, ui_colors.surface);
        SetDCBrushColor(dc, ui_colors.surface);
        return (LRESULT)GetStockObject(DC_BRUSH);
    }
    if (msg == WM_DPICHANGED || msg == WM_DISPLAYCHANGE || msg == WM_CLOSE) {
        cancel_capture();
        return 0;
    }
    if (phase == EDITING) {
        POINT p;
        switch (msg) {
        case WM_MOUSEACTIVATE:
            if (text_input && HIWORD(lp) == WM_LBUTTONDOWN &&
                GetCursorPos(&p) && adjustment_at(p)) return MA_NOACTIVATE;
            break;
        case WM_SETCURSOR:
            if ((HWND)wp != w || LOWORD(lp) != HTCLIENT) break;
            SetCursor(adjustment_cursor(adjust_mode ? active_adjustment_direction() :
                !drawing && GetCursorPos(&p) ? adjustment_at(p) : 0));
            return TRUE;
        case WM_LBUTTONDOWN:
            dismiss_popup();
            if (!drawing && !adjust_mode && pointer_position(w, lp, &p)) {
                int mode = adjustment_at(p);
                if (mode) start_adjustment(w, mode, p);
                else start_mark(w, p);
            }
            return 0;
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP:
            if (adjust_mode) {
                if (pointer_position(w, lp, &p)) {
                    update_adjustment(p);
                    if (msg == WM_LBUTTONUP) finish_adjustment(w, TRUE);
                } else finish_adjustment(w, FALSE);
            } else if (drawing) {
                if (!pointer_position(w, lp, &p) || !move_mark(p)) {
                    discard_draft();
                    if (GetCapture() == w) ReleaseCapture();
                    notice(L"无法继续本次标注，请重试。");
                } else if (msg == WM_LBUTTONUP) {
                    if (draft.tool == PEN || draft.x != draft.end_x || draft.y != draft.end_y)
                        commit_draft();
                    else discard_draft();
                    if (GetCapture() == w) ReleaseCapture();
                }
            }
            return 0;
        case WM_CANCELMODE:
        case WM_CAPTURECHANGED:
            if (adjust_mode) finish_adjustment(w, FALSE);
            else if (drawing) {
                discard_draft();
                if (msg == WM_CANCELMODE && GetCapture() == w) ReleaseCapture();
            }
            return 0;
        case WM_CHAR:
        case WM_SYSCHAR:
            if (wp == 27) return 0;
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp == VK_ESCAPE) {
                // A held key must not cancel the editor after it rolled back a drag.
                if (!(lp & (1L << 30))) capture_escape();
            } else if (msg == WM_KEYDOWN && !text_input && !(lp & (1L << 30))) {
                uint32_t chord = key_chord((unsigned)wp, FALSE);
                if (chord == VK_RETURN) editor_action(ACT_COPY);
                else if (chord == 0x0253u) editor_action(ACT_SAVE);
                else break;
            } else break;
            return 0;
        }
    }
    if (phase == SELECTING) {
        switch (msg) {
        case WM_LBUTTONDOWN:
            if (!dragging && pointer_position(w, lp, &anchor)) {
                SetCapture(w);
                if (phase != SELECTING) return 0;
                dragging = GetCapture() == w;
                if (!dragging) cancel_capture();
            }
            return 0;
        case WM_MOUSEMOVE:
            if (dragging && !update_selection(w, lp)) cancel_capture();
            return 0;
        case WM_LBUTTONUP:
            if (dragging) {
                if (!update_selection(w, lp)) cancel_capture();
                else finish_capture();
            }
            return 0;
        case WM_CANCELMODE:
        case WM_RBUTTONDOWN:
            cancel_capture();
            return 0;
        case WM_CAPTURECHANGED:
            if (dragging) cancel_capture();
            return 0;
        case WM_ACTIVATEAPP:
            if (!wp) cancel_capture();
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp == VK_ESCAPE) {
                cancel_capture();
                return 0;
            }
            break;
        }
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static void start_capture(void) {
    if (phase != IDLE || menu_open) return;
    sync_ui_theme();
    phase = PREPARING;
    layout_invalid = FALSE;
    preview_failed = FALSE;
    selection = (Rect){0};
    // Let the closed tray menu leave DWM before reading any pixels.
    DwmFlush();
    BOOL ok = freeze_desktop();
    HWND focus = NULL;
    POINT cursor;
    BOOL have_cursor = GetCursorPos(&cursor);
    for (size_t i = 0; ok && !layout_invalid && i < layout.count; ++i) {
        Rect b = frames[i].bounds;
        HWND w = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, CAPTURE_CLASS, L"",
            WS_POPUP, b.left, b.top, b.right - b.left, b.bottom - b.top,
            NULL, NULL, instance, (void *)(uintptr_t)i);
        surfaces[i].window = w;
        ok = w != NULL;
        if (!focus || (have_cursor && cursor.x >= b.left && cursor.x < b.right &&
                       cursor.y >= b.top && cursor.y < b.bottom)) focus = w;
    }
    for (size_t i = 0; ok && !layout_invalid && !preview_failed && i < layout.count; ++i)
        ok = SetWindowPos(surfaces[i].window, HWND_TOPMOST, 0, 0, 0, 0,
                          SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (ok && flush_overlays() && unchanged_layout() && !layout_invalid && !preview_failed) {
        SetForegroundWindow(focus);
        BOOL focused = GetForegroundWindow() == focus;
        if (focused) SetFocus(focus);
        // A background hotkey need not grant foreground rights; keep Esc available
        // until the user clicks an overlay. Release this binding with the overlays.
        if (!focused && !layout_invalid && !preview_failed)
            escape_registered = RegisterHotKey(owner, HOTKEY_CANCEL, MOD_NOREPEAT, VK_ESCAPE);
        if (!layout_invalid && !preview_failed && (focused || escape_registered)) {
            phase = SELECTING;
            return;
        }
        if (!layout_invalid && !preview_failed) notice(L"无法激活截图遮罩，请通过托盘菜单重试。");
    } else if (!layout_invalid && !preview_failed) {
        notice(L"无法冻结桌面，请检查显示器后重试。");
    }
    phase = OUTPUT;
    close_overlays();
    if (preview_failed) notice(L"无法绘制截图预览，已取消截图。");
    idle();
}

enum SettingsFace {
    SETTINGS_CHANGE = 1, SETTINGS_QUIT, SETTINGS_MIN, SETTINGS_CLOSE, SETTINGS_TITLE,
    SETTINGS_FEEDBACK, SETTINGS_SWITCH, SETTINGS_KEYCAP, SETTINGS_LABEL, SETTINGS_MUTED
};
static HWND settings_title;
static const WCHAR settings_face_key[] = L"Shiguang.SettingsFace";

static void paint_settings_face(HWND w, HDC dc, DWORD_PTR id) {
    RECT r;
    if (!GetClientRect(w, &r)) return;
    int saved = SaveDC(dc);
    if (!saved) return;
    const UiPalette *c = &ui_colors;
    BOOL hc = c->high_contrast;
    int dpi = (int)GetDpiForWindow(w);
    POINT p;
    BOOL hot = GetCursorPos(&p) && WindowFromPoint(p) == w;
    BOOL pressed = (SendMessageW(w, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
    BOOL disabled = !IsWindowEnabled(w);
    COLORREF ink = disabled ? c->muted : c->ink;
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, c->surface);
    FillRect(dc, &r, GetStockObject(DC_BRUSH));
    if (id == SETTINGS_CHANGE || id == SETTINGS_QUIT || id == SETTINGS_KEYCAP) {
        BOOL primary = id == SETTINGS_CHANGE;
        BOOL system_highlight = hc && !disabled && id != SETTINGS_KEYCAP && (hot || pressed);
        UiFace face = primary ? ui_primary_face(c, pressed || system_highlight) :
            (UiFace){system_highlight ? c->active :
                         !hc && (id == SETTINGS_KEYCAP || (hot && id == SETTINGS_QUIT))
                             ? c->hover : c->surface,
                     system_highlight ? c->active_ink : ink};
        if (disabled) face = (UiFace){c->surface, c->muted};
        SetDCBrushColor(dc, face.fill);
        SetDCPenColor(dc, primary && (hot || pressed) && !hc && !disabled ? c->focus : c->edge);
        int radius = MulDiv(14, dpi, 96);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
        ink = face.ink;
    }
    if (id == SETTINGS_MIN || id == SETTINGS_CLOSE) {
        if (hot || pressed) {
            SetDCBrushColor(dc, hc ? c->active : id == SETTINGS_CLOSE ? RGB(192, 38, 49) : c->hover);
            FillRect(dc, &r, GetStockObject(DC_BRUSH));
            if (hc) ink = c->active_ink;
            else if (id == SETTINGS_CLOSE) ink = RGB(255, 255, 255);
        }
        SetDCPenColor(dc, ink);
        int x = r.right / 2, y = r.bottom / 2, a = MulDiv(5, dpi, 96);
        if (id == SETTINGS_MIN)
            Polyline(dc, (POINT[]){{x - a, y}, {x + a + 1, y}}, 2);
        else {
            Polyline(dc, (POINT[]){{x-a,y-a},{x+a+1,y+a+1}}, 2);
            Polyline(dc, (POINT[]){{x+a,y-a},{x-a-1,y+a+1}}, 2);
        }
    } else {
        UINT align = DT_CENTER;
        if (id == SETTINGS_TITLE) {
            int inset = MulDiv(18, dpi, 96), side = MulDiv(21, dpi, 96);
            DrawIconEx(dc, inset, (r.bottom-side)/2, small_icon, side, side, 0, NULL, DI_NORMAL);
            r.left = inset + side + MulDiv(10, dpi, 96);
            align = DT_LEFT;
        } else if (id == SETTINGS_SWITCH) {
            BOOL on = SendMessageW(w, BM_GETCHECK, 0, 0) == BST_CHECKED;
            int h = MulDiv(18, dpi, 96), wide = MulDiv(34, dpi, 96);
            int x = MulDiv(2, dpi, 96), y = (r.bottom-h)/2, pad = MulDiv(3, dpi, 96);
            BOOL enabled_on = on && !disabled;
            COLORREF track = enabled_on ? c->active : hc ? c->surface : c->hover;
            COLORREF thumb_ink = enabled_on ? (hc ? c->active_ink : c->focus)
                : disabled || !hc ? c->muted : c->ink;
            SetDCBrushColor(dc, track);
            SetDCPenColor(dc, disabled ? c->muted : enabled_on && !hc ? c->focus : c->edge);
            RoundRect(dc, x, y, x+wide, y+h, h, h);
            SetDCBrushColor(dc, thumb_ink);
            SetDCPenColor(dc, thumb_ink);
            int thumb = on ? x+wide-h+pad : x+pad;
            Ellipse(dc, thumb, y+pad, thumb+h-2*pad, y+h-pad);
            r.left = x+wide+MulDiv(9, dpi, 96);
            align = DT_LEFT;
        } else if (id == SETTINGS_FEEDBACK || id == SETTINGS_LABEL || id == SETTINGS_MUTED) {
            if (id == SETTINGS_FEEDBACK) ink = c->warning;
            if (id == SETTINGS_MUTED && !hc) ink = c->muted;
            align = DT_LEFT;
        }
        HFONT font = (HFONT)SendMessageW(w, WM_GETFONT, 0, 0);
        if (font) SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ink);
        wchar_t caption[192];
        int n = GetWindowTextW(w, caption, ARRAYSIZE(caption));
        if (n) DrawTextW(dc, caption, n, &r, DT_NOPREFIX |
            (id == SETTINGS_FEEDBACK ? DT_WORDBREAK : DT_SINGLELINE | DT_VCENTER | align | DT_END_ELLIPSIS));
        else if (r.bottom <= MulDiv(2, dpi, 96)) {
            SetDCBrushColor(dc, c->edge);
            FillRect(dc, &r, GetStockObject(DC_BRUSH));
        }
    }
    if (GetFocus() == w && !(SendMessageW(w, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        GetClientRect(w, &r);
        InflateRect(&r, -2, -2);
        if (hc) DrawFocusRect(dc, &r);
        else {
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            SetDCPenColor(dc, c->focus);
            RoundRect(dc, r.left, r.top, r.right, r.bottom, 8, 8);
        }
    }
    RestoreDC(dc, saved);
}

static LRESULT CALLBACK settings_face_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp,
                                          UINT_PTR id, DWORD_PTR ref) {
    if (msg == WM_NCDESTROY) {
        if (ref == SETTINGS_TITLE) settings_title = NULL;
        RemovePropW(w, settings_face_key);
        RemoveWindowSubclass(w, settings_face_proc, id);
    }
    if (ref == SETTINGS_TITLE && msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps = {0};
        HDC dc = BeginPaint(w, &ps);
        if (dc) paint_settings_face(w, dc, ref);
        EndPaint(w, &ps);
        return 0;
    }
    if (msg == WM_PRINTCLIENT) {
        paint_settings_face(w, (HDC)wp, ref);
        return 0;
    }
    BOOL button = ref <= SETTINGS_CLOSE || ref == SETTINGS_SWITCH;
    if (button && msg == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track = {.cbSize = sizeof track, .dwFlags = TME_LEAVE, .hwndTrack = w};
        TrackMouseEvent(&track);
    }
    LRESULT result = DefSubclassProc(w, msg, wp, lp);
    // The stock Button class can repaint directly during state changes. Keep
    // its auto-check/accessibility semantics, but restore our face immediately.
    if (button && (msg == BM_SETCHECK || msg == BM_SETSTATE || msg == BM_CLICK || msg == WM_ENABLE ||
        msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_UPDATEUISTATE ||
        msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_CANCELMODE ||
        msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CAPTURECHANGED ||
        msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE))
        RedrawWindow(w, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    return result;
}

static LRESULT CALLBACK settings_draw_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp,
                                            UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(w, settings_draw_proc, id);
    if (msg == WM_NOTIFY && lp) {
        const NMHDR *note = (const NMHDR *)lp;
        if (note->code == NM_CUSTOMDRAW) {
            const NMCUSTOMDRAW *draw = (const NMCUSTOMDRAW *)lp;
            DWORD_PTR face = (DWORD_PTR)GetPropW(note->hwndFrom, settings_face_key);
            if (face && draw->dwDrawStage == CDDS_PREPAINT) {
                paint_settings_face(note->hwndFrom, draw->hdc, face);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    return DefSubclassProc(w, msg, wp, lp);
}

static BOOL CALLBACK style_settings_controls(HWND w, LPARAM unused) {
    (void)unused;
    wchar_t klass[16], label[192] = {0};
    if (!GetClassNameW(w, klass, ARRAYSIZE(klass))) return TRUE;
    int n = GetWindowTextW(w, label, ARRAYSIZE(label));
    UINT_PTR face = 0;
    if (!wcscmp(klass, L"Button")) {
        if (!wcscmp(label, L"更改") || !wcscmp(label, L"重新录入") || !wcscmp(label, L"取消录入"))
            face = SETTINGS_CHANGE;
        else if (!wcscmp(label, L"退出拾光")) face = SETTINGS_QUIT;
        else if (!wcscmp(label, L"最小化")) face = SETTINGS_MIN;
        else if (!wcscmp(label, L"关闭")) face = SETTINGS_CLOSE;
        else if (!wcscmp(label, L"登录 Windows 时启动拾光")) face = SETTINGS_SWITCH;
    } else if (!wcscmp(klass, L"Static")) {
        if (!wcscmp(label, L"拾光截图 / 设置")) {
            face = SETTINGS_TITLE;
            settings_title = w;
        } else if (label[0] == L'ⓘ' || !wcsncmp(label, L"无法", 2) || !wcsncmp(label, L"已有", 2))
            face = SETTINGS_FEEDBACK;
        else if ((n == 1 && ((label[0] >= L'A' && label[0] <= L'Z') ||
                  (label[0] >= L'0' && label[0] <= L'9') || wcschr(L"←↑→↓", label[0]))) ||
                 (n >= 2 && n <= 3 && label[0] == L'F' && label[1] >= L'1' && label[1] <= L'9') ||
                 !wcscmp(label, L"Ctrl") || !wcscmp(label, L"Alt") || !wcscmp(label, L"Shift") ||
                 !wcscmp(label, L"Win") || !wcscmp(label, L"Space") || !wcscmp(label, L"PgUp") ||
                 !wcscmp(label, L"PgDn") || !wcscmp(label, L"End") || !wcscmp(label, L"Home") ||
                 !wcscmp(label, L"Ins") || !wcscmp(label, L"Del")) face = SETTINGS_KEYCAP;
        else if (!wcscmp(label, L"区域截图快捷键") || !wcscmp(label, L"+") ||
                 !wcsncmp(label, L"请按", 2)) face = SETTINGS_LABEL;
        else face = SETTINGS_MUTED;
    }
    if (face && SetWindowSubclass(w, settings_face_proc, 1, face) &&
        (face <= SETTINGS_CLOSE || face == SETTINGS_SWITCH) &&
        SetPropW(w, settings_face_key, (HANDLE)face))
        SetWindowSubclass(GetParent(w), settings_draw_proc, 1, 0);
    return TRUE;
}

void sg_style_settings(void) {
    if (settings) {
        EnumChildWindows(settings, style_settings_controls, 0);
        RedrawWindow(settings, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
}

void sg_settings_caption(int close) {
    if (settings) PostMessageW(settings, close ? WM_CLOSE : WM_SYSCOMMAND, close ? 0 : SC_MINIMIZE, 0);
}

static LRESULT CALLBACK settings_subclass(HWND w, UINT msg, WPARAM wp, LPARAM lp,
                                           UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    if (recording_hook && (msg == WM_CLOSE || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE))) {
        BOOL cancel = recording_hotkey;
        sg_record_hotkey(0);
        if (cancel) PostMessageW(w, SG_RECORD_MESSAGE, 0, 0);
    }
    if (msg == WM_ERASEBKGND) {
        RECT r;
        GetClientRect(w, &r);
        HDC dc = (HDC)wp;
        COLORREF old = SetDCBrushColor(dc, ui_colors.surface);
        FillRect(dc, &r, GetStockObject(DC_BRUSH));
        SetDCBrushColor(dc, old);
        return 1;
    }
    if (msg == WM_NCHITTEST && settings_title && !IsIconic(w)) {
        LRESULT hit = DefSubclassProc(w, msg, wp, lp);
        RECT caption;
        if (hit == HTCLIENT && GetWindowRect(settings_title, &caption) &&
            PtInRect(&caption, (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) return HTCAPTION;
        return hit;
    }
    if (msg == WM_NCLBUTTONDBLCLK && wp == HTCAPTION) return 0;
    if (msg == WM_CLOSE) {
        if (tray_ready) ShowWindow(w, SW_HIDE);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        sg_record_hotkey(0);
        settings = NULL;
        RemoveWindowSubclass(w, settings_subclass, id);
    }
    return DefSubclassProc(w, msg, wp, lp);
}

int sg_bind_settings(HWND w) {
    if (!SetWindowSubclass(w, settings_subclass, 1, 0)) return 0;
    settings = w;
    // Win32 shows a disabled maximize button beside an enabled minimize button.
    // Use two native Button children in the caption instead, retaining system commands.
    LONG_PTR style = GetWindowLongPtrW(w, GWL_STYLE);
    DWORD border_style = (DWORD)((style & ~(WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_BORDER);
    RECT frame;
    if (!GetClientRect(w, &frame) || !AdjustWindowRectExForDpi(&frame, border_style, FALSE,
        (DWORD)GetWindowLongPtrW(w, GWL_EXSTYLE), GetDpiForWindow(w))) return 0;
    SetLastError(0);
    if (!SetWindowLongPtrW(w, GWL_STYLE, border_style) && GetLastError()) return 0;
    if (!SetWindowPos(w, NULL, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                      SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) return 0;
    DWORD rounded = 2; // DWMWCP_ROUND; on older Windows this preference is ignored.
    DwmSetWindowAttribute(w, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &rounded, sizeof rounded);
    SendMessageW(w, WM_SETICON, ICON_SMALL, (LPARAM)small_icon);
    SendMessageW(w, WM_SETICON, ICON_BIG, (LPARAM)large_icon);
    return 1;
}

static void show_settings(void) {
    if (!settings) return;
    sync_ui_theme();
    sg_style_settings();
    ShowWindow(settings, IsIconic(settings) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(settings);
    PostMessageW(settings, SG_TRAY_MESSAGE, (WPARAM)tray_ready, 0);
}

static void tray_menu(void) {
    if (phase != IDLE || menu_open) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    UINT has_image = recent.bgra ? MF_ENABLED : MF_GRAYED;
    AppendMenuW(menu, MF_STRING, CMD_CAPTURE, L"区域截图");
    AppendMenuW(menu, MF_STRING | has_image, CMD_COPY, L"复制上次截图");
    AppendMenuW(menu, MF_STRING | has_image, CMD_SAVE, L"另存上次截图为 PNG…");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CMD_SETTINGS, L"设置…");
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"退出");
    POINT p;
    int chosen = 0;
    menu_open = TRUE;
    if (GetCursorPos(&p)) {
        SetForegroundWindow(owner);
        chosen = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                     p.x, p.y, 0, owner, NULL);
    }
    menu_open = FALSE;
    DestroyMenu(menu);
    PostMessageW(owner, WM_NULL, 0, 0);
    if (chosen) PostMessageW(owner, COMMAND_MSG, (WPARAM)chosen, 0);
}

static LRESULT CALLBACK owner_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (taskbar_created && msg == taskbar_created) {
        if (settings) {
            tray_ready = add_tray();
            PostMessageW(settings, SG_TRAY_MESSAGE, (WPARAM)tray_ready, 0);
            if (!tray_ready && phase == IDLE) show_settings();
        }
        return 0;
    }
    switch (msg) {
    case PREVIEW_WAKE:
        return 0; // Leaf's post-dispatch hook handles the latched failure.
    case WM_HOTKEY:
        if (escape_registered && wp == HOTKEY_CANCEL) {
            capture_escape();
        } else if (active_hotkey && wp == (WPARAM)active_hotkey) {
            if (recording_hook && settings && GetForegroundWindow() == settings) return 0;
            if (phase == EDITING && shortcut == 0x0253u) {
                // The global registration can intercept the editor's Ctrl+S.
                if (!text_input && foreground_overlay()) editor_action(ACT_SAVE);
            } else start_capture();
        }
        return 0;
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        sync_ui_theme();
        break;
    case WM_DISPLAYCHANGE:
        cancel_capture();
        return 0;
    case TRAY_MSG:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU: tray_menu(); break;
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (phase == IDLE && !menu_open) show_settings();
            break;
        }
        return 0;
    case COMMAND_MSG:
        if (phase != IDLE || menu_open) return 0;
        switch (wp) {
        case CMD_CAPTURE: start_capture(); break;
        case CMD_SETTINGS: show_settings(); break;
        case CMD_COPY:
            if (recent.bgra) {
                phase = OUTPUT;
                copy_recent();
                idle();
            }
            break;
        case CMD_SAVE:
            if (recent.bgra) {
                phase = OUTPUT;
                HRESULT saved = save_png_dialog(w, &recent, NULL);
                if (saved == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) || saved == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS))
                    notice(L"文件名已存在，未覆盖。请另选文件名。");
                else if (FAILED(saved)) notice(L"无法保存 PNG，截图仍保留在内存中。");
                idle();
            }
            break;
        case CMD_EXIT: PostQuitMessage(0); break;
        }
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int sg_start(void) {
    // The Local namespace scopes the instance to the interactive Windows session.
    single_instance = CreateMutexW(NULL, FALSE, L"Local\\Shiguang.Screenshot.v1");
    if (!single_instance) return -1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 1;
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        return -1;
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return -1;
    com = TRUE;
    instance = GetModuleHandleW(NULL);
    INITCOMMONCONTROLSEX controls = {.dwSize = sizeof controls, .dwICC = ICC_WIN95_CLASSES};
    if (!InitCommonControlsEx(&controls)) return -1;
    if (!register_panel(instance)) return -1;
    small_icon = shiguang_icon(GetSystemMetrics(SM_CXSMICON));
    large_icon = shiguang_icon(GetSystemMetrics(SM_CXICON));
    if (!small_icon || !large_icon) return -1;
    taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW controller = {.lpfnWndProc = owner_proc, .hInstance = instance, .lpszClassName = OWNER_CLASS};
    WNDCLASSW overlay = {.lpfnWndProc = overlay_proc, .hInstance = instance, .lpszClassName = CAPTURE_CLASS,
                        .hCursor = LoadCursorW(NULL, IDC_CROSS)};
    if (!taskbar_created || !RegisterClassW(&controller) || !RegisterClassW(&overlay)) return -1;
    // A hidden top-level window (not HWND_MESSAGE) receives display and Explorer broadcasts.
    owner = CreateWindowExW(WS_EX_TOOLWINDOW, OWNER_CLASS, APP_TITLE, WS_POPUP,
                            0, 0, 0, 0, NULL, NULL, instance, NULL);
    if (!owner) return -1;
    DWORD saved = 0, size = sizeof saved;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"Hotkey", RRF_RT_REG_DWORD,
                     NULL, &saved, &size) == ERROR_SUCCESS) shortcut = decode_hotkey(saved);
    if (register_hotkey(HOTKEY_A, shortcut)) active_hotkey = HOTKEY_A;
    return 0;
}

void sg_ready(int background) {
    tray_ready = add_tray();
    PostMessageW(settings, SG_TRAY_MESSAGE, (WPARAM)tray_ready, 0);
    if (background) {
        if (!tray_ready) sg_exit();
        return;
    }
    if (!tray_ready) show_settings();
    else if (!active_hotkey) notice(L"截图快捷键已被占用。请在托盘设置里换一个，或从托盘截图。");
}

void sg_stop(void) {
    sg_record_hotkey(0);
    cancel_capture();
    if (active_hotkey) UnregisterHotKey(owner, active_hotkey);
    if (tray_ready) {
        NOTIFYICONDATAW data = tray_data();
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
    free(recent.bgra);
    if (owner) DestroyWindow(owner);
    if (small_icon) DestroyIcon(small_icon);
    if (large_icon) DestroyIcon(large_icon);
    if (com) CoUninitialize();
    if (single_instance) CloseHandle(single_instance);
}

void sg_exit(void) { PostMessageW(owner, COMMAND_MSG, CMD_EXIT, 0); }

void sg_fatal(const char *message) {
    wchar_t detail[512];
    if (!MultiByteToWideChar(CP_UTF8, 0, message, -1, detail, ARRAYSIZE(detail)))
        wcscpy(detail, L"无法启动拾光截图。");
    MessageBoxW(NULL, detail, APP_TITLE, MB_OK | MB_ICONERROR);
}

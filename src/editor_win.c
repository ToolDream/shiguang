#define UNICODE
#include "editor_win.h"
#include <commctrl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_CLASS L"Shiguang.EditorPanel"
const COLORREF palette[COLORS] = {
    RGB(233, 74, 68), RGB(28, 113, 208), RGB(32, 156, 105),
    RGB(255, 191, 38), RGB(36, 49, 65), RGB(255, 255, 255)
};

UiPalette ui_colors;

UiPalette ui_palette(BOOL dark, BOOL high_contrast) {
    if (high_contrast) return (UiPalette){
        .surface = GetSysColor(COLOR_WINDOW), .ink = GetSysColor(COLOR_WINDOWTEXT),
        .muted = GetSysColor(COLOR_GRAYTEXT), .edge = GetSysColor(COLOR_WINDOWTEXT),
        .hover = GetSysColor(COLOR_HOTLIGHT), .active = GetSysColor(COLOR_HIGHLIGHT),
        .active_ink = GetSysColor(COLOR_HIGHLIGHTTEXT), .pressed = GetSysColor(COLOR_HIGHLIGHT),
        .accent = GetSysColor(COLOR_HIGHLIGHT),
        .focus = GetSysColor(COLOR_HIGHLIGHT), .hint = GetSysColor(COLOR_WINDOW),
        .hint_ink = GetSysColor(COLOR_WINDOWTEXT), .warning = GetSysColor(COLOR_WINDOWTEXT),
        .high_contrast = TRUE
    };
    return ui_normal_palette(dark != FALSE);
}

BOOL refresh_ui_palette(void) {
    HIGHCONTRASTW contrast = {.cbSize = sizeof contrast};
    BOOL hc = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON);
    DWORD light = 1, bytes = sizeof light;
    BOOL dark = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &bytes) == ERROR_SUCCESS && !light;
    UiPalette next = ui_palette(dark, hc);
    BOOL changed = !ui_palette_equal(&ui_colors, &next);
    ui_colors = next;
    return changed;
}

int canvas_open(Canvas *c, const Image *base) {
    BITMAPINFO info = {.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER), .biWidth = base->width,
        .biHeight = -base->height, .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB}};
    void *bits = NULL;
    HDC screen = GetDC(NULL);
    if (!screen) return 0;
    c->dc = CreateCompatibleDC(screen);
    c->dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!c->dc || !c->dib || !bits || !(c->previous = SelectObject(c->dc, c->dib))) return 0;
    c->image = (Image){base->width, base->height, bits};
    return 1;
}

void canvas_close(Canvas *c) {
    if (c->previous) SelectObject(c->dc, c->previous);
    if (c->dib) DeleteObject(c->dib);
    if (c->dc) DeleteDC(c->dc);
    *c = (Canvas){0};
}

HFONT mark_font(int height) {
    return CreateFontW(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       0, 0, NONANTIALIASED_QUALITY, 0, L"Microsoft YaHei UI");
}

static BOOL draw_mark(HDC dc, const Mark *m) {
    if (m->tool == TEXT) {
        if (!m->text || !*m->text) return TRUE;
        HFONT font = mark_font(m->width);
        HGDIOBJ old = font ? SelectObject(dc, font) : NULL;
        BOOL ok = old && SetBkMode(dc, TRANSPARENT) && SetTextColor(dc, m->color) != CLR_INVALID &&
            TextOutW(dc, m->x, m->y, m->text, (int)wcslen(m->text));
        if (old) SelectObject(dc, old);
        if (font) DeleteObject(font);
        return ok;
    }
    HPEN pen = CreatePen(PS_SOLID, m->width, m->color);
    HGDIOBJ old = pen ? SelectObject(dc, pen) : NULL;
    HGDIOBJ brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    BOOL ok = old && brush;
    if (ok) {
        int x = m->x, y = m->y, ex = m->end_x, ey = m->end_y;
        Rect r = drag_rect(x, y, ex, ey);
        int l = r.left, t = r.top, right = r.right, b = r.bottom;
        int cx = l + (right - l) / 2, cy = t + (b - t) / 2;
        switch (m->tool) {
        case RECTANGLE:
            ok = Rectangle(dc, l, t, right + 1, b + 1);
            break;
        case CIRCLE:
        case ELLIPSE:
            ok = Ellipse(dc, l, t, right + 1, b + 1);
            break;
        case TRIANGLE:
            ok = Polygon(dc, (POINT[]){{cx, t}, {right, b}, {l, b}}, 3);
            break;
        case DIAMOND:
            ok = Polygon(dc, (POINT[]){{cx, t}, {right, cy}, {cx, b}, {l, cy}}, 4);
            break;
        case ARROW: {
            double dx = (double)ex - x, dy = (double)ey - y, len = hypot(dx, dy);
            if (len < 1.0) break;
            double head = fmin(len * 0.65, m->width * 6.0), ux = dx / len, uy = dy / len;
            int lx = ex + (int)lround(head * (-ux + 0.48 * uy));
            int ly = ey + (int)lround(head * (-uy - 0.48 * ux));
            int rx = ex + (int)lround(head * (-ux - 0.48 * uy));
            int ry = ey + (int)lround(head * (-uy + 0.48 * ux));
            ok = Polyline(dc, (POINT[]){{x, y}, {ex, ey}, {lx, ly}}, 3) &&
                 Polyline(dc, (POINT[]){{ex, ey}, {rx, ry}}, 2);
            break;
        }
        case PEN:
            if (!m->count) break;
            ok = MoveToEx(dc, m->points[0].x, m->points[0].y, NULL);
            for (size_t i = 1; ok && i < m->count; ++i)
                ok = LineTo(dc, m->points[i].x, m->points[i].y);
            // LineTo excludes its endpoint; keep a click and the final endpoint visible.
            if (ok) {
                EditPoint p = m->points[m->count - 1];
                HBRUSH dot = CreateSolidBrush(m->color);
                int half = m->width / 2;
                ok = dot && FillRect(dc, &(RECT){p.x-half, p.y-half, p.x-half+m->width,
                    p.y-half+m->width}, dot);
                if (dot) DeleteObject(dot);
            }
            break;
        case TEXT:
            break;
        }
    }
    if (brush) SelectObject(dc, brush);
    if (old) SelectObject(dc, old);
    if (pen) DeleteObject(pen);
    return ok;
}

BOOL paint_marks(HDC dc, const History *h, const Mark *draft, EditPoint origin) {
    if (!h->used && !draft) return TRUE;
    int saved = SaveDC(dc);
    if (!saved) return FALSE;
    BOOL ok = OffsetViewportOrgEx(dc, origin.x, origin.y, NULL);
    for (size_t i = 0; ok && i < h->used; ++i) ok = draw_mark(dc, &h->marks[i]);
    if (ok && draft) ok = draw_mark(dc, draft);
    return RestoreDC(dc, saved) && ok;
}

int canvas_render(Canvas *c, const Image *base, const History *h, const Mark *draft, EditPoint origin) {
    size_t bytes = image_bytes(base->width, base->height);
    if (!GdiFlush()) return 0;
    memcpy(c->image.bgra, base->bgra, bytes);
    if (!paint_marks(c->dc, h, draft, origin) || !GdiFlush()) return 0;
    // Frozen monitor pixels are opaque. Holes start as transparent black;
    // full-coverage non-black inks turn only touched hole pixels opaque.
    if (h->used || draft) {
        uint32_t *pixels = (uint32_t *)c->image.bgra;
        const unsigned char *bgra = base->bgra;
        for (size_t i = 0; i < bytes / 4; ++i)
            pixels[i] = (pixels[i] & 0xffffffu) |
                ((bgra[4*i + 3] || (pixels[i] & 0xffffffu)) ? 0xff000000u : 0);
    }
    return 1;
}

static const WCHAR *const labels[] = {
    L"形状", L"箭头", L"自由画笔", L"文字", L"颜色", L"撤销", L"重做", L"保存 PNG（Ctrl+S）", L"复制（Enter）"
};
static const WCHAR *const shape_labels[] = {L"矩形", L"圆", L"椭圆", L"三角形", L"菱形"};
static const WCHAR *const color_labels[] = {L"红色", L"蓝色", L"绿色", L"黄色", L"深色", L"白色"};

static const WCHAR *action_label(int action) {
    unsigned id = (unsigned)action;
    if (id < BAR_BUTTONS) return labels[id];
    if (id >= ACT_RECTANGLE && id < ACT_RECTANGLE + SHAPES) return shape_labels[id - ACT_RECTANGLE];
    if (id >= ACT_RED && id < ACT_RED + COLORS) return color_labels[id - ACT_RED];
    return NULL;
}

#include "toolbar_glyph.h"

static void draw_icon(const DRAWITEMSTRUCT *item, const Panel *p) {
    HDC dc = item->hDC;
    RECT r = item->rcItem;
    int action = (int)(item->CtlID - 1u);
    if (!action_label(action)) return;
    int saved = SaveDC(dc);
    if (!saved) return;
    BOOL selected = (action == ACT_SHAPES && p->tool <= DIAMOND) ||
                    (action == ACT_ARROW && p->tool == ARROW) || (action == ACT_PEN && p->tool == PEN) ||
                    (action == ACT_TEXT && p->tool == TEXT) || action == ACT_RECTANGLE + (int)p->shape ||
                    action == ACT_RED + p->color;
    SelectObject(dc, GetStockObject(DC_PEN));
    SelectObject(dc, GetStockObject(DC_BRUSH));
    POINT cursor;
    BOOL hot = item->hwndItem && GetCursorPos(&cursor) && WindowFromPoint(cursor) == item->hwndItem;
    BOOL pressed = (item->itemState & ODS_SELECTED) != 0;
    BOOL disabled = (item->itemState & ODS_DISABLED) != 0;
    BOOL active = !disabled && (selected || pressed);
    const UiPalette *c = &ui_colors;
    SetDCBrushColor(dc, c->surface);
    FillRect(dc, &r, GetStockObject(DC_BRUSH));

    int dpi = (int)GetDpiForWindow(p->window);
    if (!dpi) dpi = 96;
    int w = r.right - r.left, h = r.bottom - r.top, extent = w < h ? w : h;
    int inset = MulDiv(2, dpi, 96);
    if (inset > extent / 16) inset = extent / 16;
    RECT state = r;
    InflateRect(&state, -inset, -inset);
    int radius = MulDiv(12, dpi, 96);
    if (radius > extent / 3) radius = extent / 3;
    BOOL hover = hot && !disabled && !active;
    COLORREF face = active ? (pressed ? c->pressed : c->active) :
                    hover && !c->high_contrast ? c->hover : c->surface;
    if (face != c->surface) {
        SetDCPenColor(dc, face);
        SetDCBrushColor(dc, face);
        RoundRect(dc, state.left, state.top, state.right, state.bottom, radius, radius);
    }
    COLORREF ink = disabled ? c->muted : active ? c->active_ink : c->ink;
    aa_glyph(dc, r, dpi, action, p, ink, face);
    if (hover && c->high_contrast) {
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        SetDCPenColor(dc, c->ink);
        RoundRect(dc, state.left, state.top, state.right, state.bottom, radius, radius);
    }
    if (item->itemState & ODS_FOCUS) DrawFocusRect(dc, &state);
    RestoreDC(dc, saved);
}

static LRESULT CALLBACK panel_button_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp,
                                          UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(w, panel_button_proc, id);
    if (msg == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track = {.cbSize = sizeof track, .dwFlags = TME_LEAVE, .hwndTrack = w};
        TrackMouseEvent(&track);
        InvalidateRect(w, NULL, FALSE);
    }
    if (msg == WM_MOUSELEAVE) InvalidateRect(w, NULL, FALSE);
    if ((msg == WM_CHAR || msg == WM_SYSCHAR) && wp == 27) return 0;
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
        (wp == VK_ESCAPE || wp == VK_RETURN || (wp == 'S' && GetKeyState(VK_CONTROL) < 0))) {
        HWND overlay = GetAncestor(w, GA_ROOTOWNER);
        if (overlay && overlay != w) {
            SendMessageW(overlay, msg, wp, lp);
            return 0;
        }
    }
    return DefSubclassProc(w, msg, wp, lp);
}

static LRESULT CALLBACK panel_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        return TRUE;
    }
    Panel *p = (Panel *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps = {0};
        HDC dc = BeginPaint(w, &ps);
        if (dc) {
            RECT r;
            GetClientRect(w, &r);
            SelectObject(dc, GetStockObject(DC_BRUSH));
            SelectObject(dc, GetStockObject(DC_PEN));
            SetDCBrushColor(dc, ui_colors.surface);
            SetDCPenColor(dc, ui_colors.edge);
            int dpi = (int)GetDpiForWindow(w);
            int radius = MulDiv(20, dpi, 96);
            if (r.right < radius || r.bottom < radius) Rectangle(dc, 0, 0, r.right, r.bottom);
            else RoundRect(dc, 0, 0, r.right, r.bottom, radius, radius);
            // Separate colors, history, and export without individual button borders.
            if (p && p->buttons[ACT_COPY] && GetDlgCtrlID(p->buttons[0]) == ACT_SHAPES + 1) {
                SetDCPenColor(dc, ui_colors.high_contrast ? ui_colors.edge : ui_colors.hover);
                for (int i = ACT_UNDO; i <= ACT_SAVE; i += ACT_SAVE - ACT_UNDO) {
                    RECT left, right;
                    if (!GetWindowRect(p->buttons[i-1], &left) || !GetWindowRect(p->buttons[i], &right) ||
                        left.top != right.top || right.left <= left.right + 2) continue;
                    POINT at = {(left.right + right.left) / 2, right.top};
                    if (!ScreenToClient(w, &at)) continue;
                    int height = right.bottom - right.top;
                    int inset = (height - MulDiv(20, dpi, 96)) / 2;
                    if (inset < 1) inset = height / 4;
                    MoveToEx(dc, at.x, at.y + inset, NULL);
                    LineTo(dc, at.x, at.y + height - inset);
                }
            }
            EndPaint(w, &ps);
        }
        return 0;
    }
    if (msg == WM_DRAWITEM && p && lp) {
        draw_icon((const DRAWITEMSTRUCT *)lp, p);
        return TRUE;
    }
    if (msg == WM_COMMAND && p && lp && HIWORD(wp) == BN_CLICKED) {
        int action = LOWORD(wp) - 1;
        if (action_label(action)) p->click(action);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

BOOL register_panel(HINSTANCE instance) {
    refresh_ui_palette();
    WNDCLASSW wc = {.lpfnWndProc = panel_proc, .hInstance = instance, .lpszClassName = PANEL_CLASS,
        .hCursor = LoadCursorW(NULL, IDC_ARROW)};
    return RegisterClassW(&wc) != 0;
}

static void panel_corner(HWND w, Dock d) {
    Rect b = d.bounds;
    int radius = d.cell >= 24 ? MulDiv(20, (int)GetDpiForWindow(w), 96) : 0;
    HRGN corner = radius ? CreateRoundRectRgn(0, 0, b.right-b.left+1, b.bottom-b.top+1, radius, radius) : NULL;
    if (!SetWindowRgn(w, corner, FALSE) && corner) DeleteObject(corner);
}

static BOOL place_panel_button(HWND w, Dock d, int index) {
    int pad = d.cell >= 24 ? (d.cell >= 40 ? 4 : 2) : 0;
    return SetWindowPos(w, NULL, index % d.cols * d.cell + pad, index / d.cols * d.cell + pad,
        d.cell - 2 * pad, d.cell - 2 * pad, SWP_NOACTIVATE | SWP_NOZORDER);
}

BOOL move_panel(Panel *p, Dock from, Dock to) {
    if (!p->window || to.cell <= 0 || to.cols <= 0) return FALSE;
    Rect b = to.bounds;
    UINT dpi = GetDpiForWindow(p->window);
    if (!SetWindowPos(p->window, NULL, b.left, b.top, b.right-b.left, b.bottom-b.top,
                      SWP_NOACTIVATE | SWP_NOZORDER)) return FALSE;
    BOOL same_grid = from.cell == to.cell && from.cols == to.cols;
    if (same_grid && dpi == GetDpiForWindow(p->window)) return TRUE;
    panel_corner(p->window, to);
    if (!same_grid)
        for (int i = 0; i < BAR_BUTTONS && p->buttons[i]; ++i)
            if (!place_panel_button(p->buttons[i], to, i)) return FALSE;
    return RedrawWindow(p->window, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

BOOL open_panel(Panel *p, HINSTANCE instance, HWND owner, Dock d, int first, int count, void (*click)(int)) {
    if (d.cell <= 0 || d.cols <= 0 || count <= 0 || count > BAR_BUTTONS) return FALSE;
    p->click = click;
    Rect b = d.bounds;
    p->window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        PANEL_CLASS, L"截图标注", WS_POPUP, b.left, b.top, b.right-b.left, b.bottom-b.top,
        owner, NULL, instance, p);
    if (!p->window) return FALSE;
    panel_corner(p->window, d);
    HWND tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, NULL,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, p->window, NULL, instance, NULL);
    for (int i = 0; i < count; ++i) {
        int action = first + i;
        const WCHAR *label = action_label(action);
        if (!label) {
            close_panel(p);
            return FALSE;
        }
        HWND button = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, p->window, (HMENU)(intptr_t)(action + 1), instance, NULL);
        if (!button || !place_panel_button(button, d, i) || !SetWindowSubclass(button, panel_button_proc, 1, 0)) {
            close_panel(p);
            return FALSE;
        }
        p->buttons[i] = button;
        if (tip) {
            TOOLINFOW info = {.cbSize = sizeof info, .uFlags = TTF_IDISHWND | TTF_SUBCLASS,
                .hwnd = p->window, .uId = (UINT_PTR)button, .lpszText = (WCHAR *)label};
            SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&info);
        }
    }
    if (!SetWindowPos(p->window, HWND_TOPMOST, 0, 0, 0, 0,
                      SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
        close_panel(p);
        return FALSE;
    }
    return TRUE;
}

void close_panel(Panel *p) {
    if (p->window) DestroyWindow(p->window);
    *p = (Panel){0};
}

void update_panel(Panel *p, Tool tool, Tool shape, int color, BOOL undo, BOOL redo) {
    if (!p->window) return;
    p->tool = tool;
    p->shape = shape;
    p->color = color;
    for (int i = 0; i < BAR_BUTTONS && p->buttons[i]; ++i) {
        int id = GetDlgCtrlID(p->buttons[i]) - 1;
        if (id == ACT_UNDO) EnableWindow(p->buttons[i], undo);
        if (id == ACT_REDO) EnableWindow(p->buttons[i], redo);
        InvalidateRect(p->buttons[i], NULL, FALSE);
    }
}

HICON shiguang_icon(int size) {
    if (size <= 0 || size > 256) return NULL;
    BITMAPINFO info = {.bmiHeader = {.biSize = sizeof(BITMAPINFOHEADER), .biWidth = size,
        .biHeight = -size, .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB}};
    void *pixels = NULL;
    HBITMAP color = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    size_t mask_stride = (size_t)((size + 15) / 16 * 2);
    unsigned char *mask_bits = calloc(mask_stride, (size_t)size);
    HBITMAP mask = mask_bits ? CreateBitmap(size, size, 1, 1, mask_bits) : NULL;
    free(mask_bits);
    HICON icon = NULL;
    if (color && mask && pixels) {
        COLORREF surface = ui_colors.accent;
        COLORREF stroke = ui_colors.high_contrast ? ui_colors.active_ink : RGB(255, 255, 247);
        #define ICON_BGRA(c) (0xff000000u | ((uint32_t)GetRValue(c) << 16) | ((uint32_t)GetGValue(c) << 8) | GetBValue(c))
        uint32_t *px = pixels;
        for (int j = 0; j < size; ++j) for (int i = 0; i < size; ++i) {
            int x = i*32/size, y = j*32/size;
            int xx = x < 16 ? x : 31-x, yy = y < 16 ? y : 31-y;
            int dx = xx < 8 ? 8-xx : 0, dy = yy < 8 ? 8-yy : 0;
            BOOL outline = (xx >= 6 && xx <= 8 && yy >= 6 && yy <= 13) ||
                           (yy >= 6 && yy <= 8 && xx >= 6 && xx <= 13);
            BOOL dot = (x-16)*(x-16) + (y-16)*(y-16) <= 11;
            px[j*size+i] = dx*dx+dy*dy > 46 ? 0u : outline || dot ? ICON_BGRA(stroke) : ICON_BGRA(surface);
        }
        #undef ICON_BGRA
        ICONINFO iconinfo = {.fIcon = TRUE, .hbmMask = mask, .hbmColor = color};
        icon = CreateIconIndirect(&iconinfo);
    }
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    return icon;
}

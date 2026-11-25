#ifndef SHIGUANG_UI_DRAW_WIN_H
#define SHIGUANG_UI_DRAW_WIN_H

#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include "ui_palette.h"

typedef struct {
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ old;
    uint32_t *pixels;
    int width, height;
} UiSsaaSurface;

static inline BOOL ui_ssaa_begin(UiSsaaSurface *s, HDC ref, int w, int h, COLORREF bg) {
    *s = (UiSsaaSurface){0};
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return FALSE;
    int sw = w * 4, sh = h * 4;
    BITMAPINFO info = {.bmiHeader = {
        .biSize = sizeof(BITMAPINFOHEADER), .biWidth = sw, .biHeight = -sh,
        .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB}};
    s->dc = CreateCompatibleDC(ref);
    if (!s->dc) return FALSE;
    s->bitmap = CreateDIBSection(s->dc, &info, DIB_RGB_COLORS, (void **)&s->pixels, NULL, 0);
    if (!s->bitmap || !s->pixels) {
        if (s->bitmap) DeleteObject(s->bitmap);
        DeleteDC(s->dc);
        *s = (UiSsaaSurface){0};
        return FALSE;
    }
    s->old = SelectObject(s->dc, s->bitmap);
    if (!s->old) {
        DeleteObject(s->bitmap);
        DeleteDC(s->dc);
        *s = (UiSsaaSurface){0};
        return FALSE;
    }
    s->width = w;
    s->height = h;
    uint32_t fill = 0xff000000u | ((uint32_t)GetRValue(bg) << 16) |
                    ((uint32_t)GetGValue(bg) << 8) | (uint32_t)GetBValue(bg);
    size_t count = (size_t)sw * (size_t)sh;
    for (size_t i = 0; i < count; ++i) s->pixels[i] = fill;
    return TRUE;
}

static inline BOOL ui_ssaa_present(UiSsaaSurface *s, HDC dst, int x, int y) {
    BOOL ok = FALSE;
    if (s->dc && s->pixels && GdiFlush()) {
        int w = s->width, h = s->height, sw = w * 4;
        uint32_t *lo = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof *lo);
        if (lo) {
            for (int row = 0; row < h; ++row) {
                for (int col = 0; col < w; ++col) {
                    unsigned b = 0, g = 0, r = 0;
                    const uint32_t *p = s->pixels + (size_t)(row * 4) * (size_t)sw + (size_t)(col * 4);
                    for (int dy = 0; dy < 4; ++dy) {
                        for (int dx = 0; dx < 4; ++dx) {
                            uint32_t px = p[dx];
                            b += px & 0xffu;
                            g += (px >> 8) & 0xffu;
                            r += (px >> 16) & 0xffu;
                        }
                        p += sw;
                    }
                    lo[(size_t)row * (size_t)w + (size_t)col] =
                        0xff000000u | (((r + 8u) >> 4) << 16) | (((g + 8u) >> 4) << 8) | ((b + 8u) >> 4);
                }
            }
            BITMAPINFO info = {.bmiHeader = {
                .biSize = sizeof(BITMAPINFOHEADER), .biWidth = w, .biHeight = -h,
                .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB}};
            ok = SetDIBitsToDevice(dst, x, y, (DWORD)w, (DWORD)h, 0, 0, 0, (UINT)h,
                                   lo, &info, DIB_RGB_COLORS) > 0;
            free(lo);
        }
    }
    if (s->old) SelectObject(s->dc, s->old);
    if (s->bitmap) DeleteObject(s->bitmap);
    if (s->dc) DeleteDC(s->dc);
    *s = (UiSsaaSurface){0};
    return ok;
}

static inline void ui_ssaa_abort(UiSsaaSurface *s) {
    if (s->old) SelectObject(s->dc, s->old);
    if (s->bitmap) DeleteObject(s->bitmap);
    if (s->dc) DeleteDC(s->dc);
    *s = (UiSsaaSurface){0};
}

static inline void ui_aa_round_rect(HDC dc, RECT r, int radius_px, COLORREF bg,
                                    COLORREF fill, COLORREF border, BOOL has_border) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    UiSsaaSurface s;
    if (!ui_ssaa_begin(&s, dc, w, h, bg)) {
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, fill);
        SetDCPenColor(dc, has_border ? border : fill);
        int d = radius_px * 2;
        RoundRect(dc, r.left, r.top, r.right, r.bottom, d, d);
        return;
    }
    int sw = w * 4, sh = h * 4;
    int diam = radius_px * 8;
    if (diam > sw) diam = sw;
    if (diam > sh) diam = sh;
    LOGBRUSH lb = {BS_SOLID, has_border ? border : fill, 0};
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_JOIN_ROUND, has_border ? 4 : 1, &lb, 0, NULL);
    HGDIOBJ old_pen = pen ? SelectObject(s.dc, pen) : NULL;
    HGDIOBJ old_brush = SelectObject(s.dc, GetStockObject(DC_BRUSH));
    SetDCBrushColor(s.dc, fill);
    int inset = has_border ? 2 : 0;
    RoundRect(s.dc, inset, inset, sw - inset, sh - inset, diam, diam);
    if (old_brush) SelectObject(s.dc, old_brush);
    if (old_pen) SelectObject(s.dc, old_pen);
    if (pen) DeleteObject(pen);
    if (!ui_ssaa_present(&s, dc, r.left, r.top)) {
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, fill);
        SetDCPenColor(dc, has_border ? border : fill);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, radius_px * 2, radius_px * 2);
    }
}

static inline void ui_aa_circle(HDC dc, RECT r, COLORREF bg, COLORREF fill,
                                COLORREF border, int border_px) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    UiSsaaSurface s;
    if (!ui_ssaa_begin(&s, dc, w, h, bg)) {
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, fill);
        SetDCPenColor(dc, border_px > 0 ? border : fill);
        Ellipse(dc, r.left, r.top, r.right, r.bottom);
        return;
    }
    int sw = w * 4, sh = h * 4;
    int pw = border_px > 0 ? border_px * 4 : 1;
    LOGBRUSH lb = {BS_SOLID, border_px > 0 ? border : fill, 0};
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID, pw, &lb, 0, NULL);
    HGDIOBJ old_pen = pen ? SelectObject(s.dc, pen) : NULL;
    HGDIOBJ old_brush = SelectObject(s.dc, GetStockObject(DC_BRUSH));
    SetDCBrushColor(s.dc, fill);
    int inset = border_px > 0 ? pw / 2 + 1 : 1;
    Ellipse(s.dc, inset, inset, sw - inset, sh - inset);
    if (old_brush) SelectObject(s.dc, old_brush);
    if (old_pen) SelectObject(s.dc, old_pen);
    if (pen) DeleteObject(pen);
    ui_ssaa_present(&s, dc, r.left, r.top);
}

static inline void ui_aa_switch(HDC dc, RECT r, COLORREF bg, BOOL on, BOOL disabled,
                                const UiPalette *c, int dpi) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    int track_h = MulDiv(22, dpi, 96);
    int track_w = MulDiv(40, dpi, 96);
    if (track_w > w) track_w = w;
    if (track_h > h) track_h = h;
    int ox = w - track_w;
    int oy = (h - track_h) / 2;
    BOOL hc = c->high_contrast;
    BOOL enabled_on = on && !disabled;
    COLORREF track_fill = enabled_on ? (hc ? c->active : c->accent)
        : (hc ? c->surface : (disabled ? c->card_edge : c->edge));
    COLORREF track_border = hc ? (disabled ? c->muted : (enabled_on ? c->active : c->ink))
        : (enabled_on ? c->accent : c->edge);
    COLORREF thumb_fill = hc ? (enabled_on ? c->active_ink : (disabled ? c->muted : c->ink))
        : (disabled ? c->hover : RGB(255, 255, 255));

    UiSsaaSurface s;
    if (!ui_ssaa_begin(&s, dc, w, h, bg)) {
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, track_fill);
        SetDCPenColor(dc, track_border);
        RoundRect(dc, r.left + ox, r.top + oy, r.left + ox + track_w, r.top + oy + track_h, track_h, track_h);
        return;
    }
    int tx = ox * 4, ty = oy * 4, tw = track_w * 4, th = track_h * 4;
    LOGBRUSH tb = {BS_SOLID, track_border, 0};
    HPEN tpen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID, hc ? 6 : 4, &tb, 0, NULL);
    HGDIOBJ old_pen = tpen ? SelectObject(s.dc, tpen) : NULL;
    HGDIOBJ old_brush = SelectObject(s.dc, GetStockObject(DC_BRUSH));
    SetDCBrushColor(s.dc, track_fill);
    RoundRect(s.dc, tx + 2, ty + 2, tx + tw - 2, ty + th - 2, th - 4, th - 4);
    if (old_pen) SelectObject(s.dc, old_pen);
    if (tpen) DeleteObject(tpen);

    int pad = MulDiv(3, dpi, 96) * 4;
    int dot = th - 2 * pad;
    if (dot < 8) dot = 8;
    int dx = on ? (tx + tw - pad - dot) : (tx + pad);
    int dy = ty + (th - dot) / 2;
    SelectObject(s.dc, GetStockObject(DC_PEN));
    SetDCBrushColor(s.dc, thumb_fill);
    SetDCPenColor(s.dc, thumb_fill);
    Ellipse(s.dc, dx, dy, dx + dot, dy + dot);
    if (old_brush) SelectObject(s.dc, old_brush);
    ui_ssaa_present(&s, dc, r.left, r.top);
}

static inline void ui_aa_caption_glyph(HDC dc, RECT r, BOOL close_btn, COLORREF bg, COLORREF ink, int dpi) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    UiSsaaSurface s;
    if (!ui_ssaa_begin(&s, dc, w, h, bg)) {
        SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, ink);
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, a = MulDiv(5, dpi, 96);
        if (!close_btn) {
            MoveToEx(dc, cx - a, cy, NULL);
            LineTo(dc, cx + a + 1, cy);
        } else {
            MoveToEx(dc, cx - a, cy - a, NULL);
            LineTo(dc, cx + a + 1, cy + a + 1);
            MoveToEx(dc, cx + a, cy - a, NULL);
            LineTo(dc, cx - a - 1, cy + a + 1);
        }
        return;
    }
    int cx = w * 2, cy = h * 2;
    int a = MulDiv(5, dpi, 96) * 4;
    int pw = MulDiv(6, dpi, 96);
    if (pw < 5) pw = 5;
    LOGBRUSH lb = {BS_SOLID, ink, 0};
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, pw, &lb, 0, NULL);
    HGDIOBJ old_pen = pen ? SelectObject(s.dc, pen) : NULL;
    if (!close_btn) {
        MoveToEx(s.dc, cx - a, cy, NULL);
        LineTo(s.dc, cx + a, cy);
    } else {
        MoveToEx(s.dc, cx - a, cy - a, NULL);
        LineTo(s.dc, cx + a, cy + a);
        MoveToEx(s.dc, cx + a, cy - a, NULL);
        LineTo(s.dc, cx - a, cy + a);
    }
    if (old_pen) SelectObject(s.dc, old_pen);
    if (pen) DeleteObject(pen);
    ui_ssaa_present(&s, dc, r.left, r.top);
}

static inline void ui_aa_nav_icon(HDC dc, int x, int y, int side, BOOL updates_icon,
                                  COLORREF bg, COLORREF ink, int dpi) {
    if (side <= 0) return;
    UiSsaaSurface s;
    if (!ui_ssaa_begin(&s, dc, side, side, bg)) return;
    int sw = side * 4;
    int pw = MulDiv(6, dpi, 96);
    if (pw < 5) pw = 5;
    LOGBRUSH lb = {BS_SOLID, ink, 0};
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, pw, &lb, 0, NULL);
    HGDIOBJ old_pen = pen ? SelectObject(s.dc, pen) : NULL;
    HGDIOBJ old_brush = SelectObject(s.dc, GetStockObject(DC_BRUSH));
    if (!updates_icon) {
        // Two clean slider lines with hollow circles ("常规").
        int y1 = sw * 5 / 16, y2 = sw * 11 / 16;
        int l = sw * 2 / 16, r = sw * 14 / 16, rad = sw * 2 / 16;
        MoveToEx(s.dc, l, y1, NULL); LineTo(s.dc, r, y1);
        MoveToEx(s.dc, l, y2, NULL); LineTo(s.dc, r, y2);
        SetDCBrushColor(s.dc, bg);
        int c1 = sw * 6 / 16, c2 = sw * 10 / 16;
        Ellipse(s.dc, c1 - rad, y1 - rad, c1 + rad, y1 + rad);
        Ellipse(s.dc, c2 - rad, y2 - rad, c2 + rad, y2 + rad);
    } else {
        // Crisp circular refresh arrow ("软件更新").
        int pad = sw * 3 / 16;
        SelectObject(s.dc, GetStockObject(NULL_BRUSH));
        SetArcDirection(s.dc, AD_COUNTERCLOCKWISE);
        Arc(s.dc, pad, pad, sw - pad, sw - pad, sw * 13 / 16, sw * 5 / 16, sw * 14 / 16, sw * 9 / 16);
        int ax = sw * 13 / 16, ay = sw * 5 / 16, ah = sw * 3 / 16;
        MoveToEx(s.dc, ax, ay - ah, NULL);
        LineTo(s.dc, ax, ay);
        LineTo(s.dc, ax - ah, ay);
    }
    if (old_brush) SelectObject(s.dc, old_brush);
    if (old_pen) SelectObject(s.dc, old_pen);
    if (pen) DeleteObject(pen);
    ui_ssaa_present(&s, dc, x, y);
}

#endif

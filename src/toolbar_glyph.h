#ifndef SHIGUANG_TOOLBAR_GLYPH_H
#define SHIGUANG_TOOLBAR_GLYPH_H
#include "editor_win.h"

// All glyphs share a 24-unit grid and a round, 1.5-unit stroke.
static BOOL draw_glyph(HDC dc, int action, const Panel *p, COLORREF ink, float scale, int x, int y) {
    int saved = SaveDC(dc);
    if (!saved) return FALSE;
    // Use a doubled logical grid so a 3-unit GDI pen gives a 1.5-unit stroke.
    XFORM transform = {scale / 2, 0, 0, scale / 2, (float)x, (float)y};
    LOGBRUSH color = {BS_SOLID, ink, 0};
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 3, &color, 0, NULL);
    BOOL ok = FALSE;
    if (!pen || !SetGraphicsMode(dc, GM_ADVANCED) || !SetWorldTransform(dc, &transform) ||
        !SelectObject(dc, pen) || !SelectObject(dc, GetStockObject(NULL_BRUSH))) goto done;

    if (action == ACT_SHAPES || (action >= ACT_RECTANGLE && action < ACT_RECTANGLE + SHAPES)) {
        BOOL dropdown = action == ACT_SHAPES;
        Tool shape = dropdown ? p->shape : (Tool)(action - ACT_RECTANGLE);
        int l = dropdown ? 4 : 7, r = dropdown ? 30 : 41;
        int t = dropdown ? 12 : 8, b = 48 - t, cx = (r + l) / 2;
        switch (shape) {
        case CIRCLE: ok = Ellipse(dc, l, 24-(r-l)/2, r, 24+(r-l)/2); break;
        case ELLIPSE: ok = Ellipse(dc, l, t+3, r, b-3); break;
        case TRIANGLE: ok = Polygon(dc, (POINT[]){{cx,t},{r,b},{l,b}}, 3); break;
        case DIAMOND: ok = Polygon(dc, (POINT[]){{cx,t},{r,24},{cx,b},{l,24}}, 4); break;
        default: ok = Rectangle(dc, l, t+2, r, b-2); break;
        }
        if (dropdown) ok = ok && Polyline(dc, (POINT[]){{36,21},{41,26},{46,21}}, 3);
    } else switch (action) {
    case ACT_ARROW:
        ok = Polyline(dc, (POINT[]){{8,40},{40,8},{21,8}}, 3);
        ok = ok && Polyline(dc, (POINT[]){{40,8},{40,27}}, 2);
        break;
    case ACT_PEN:
        ok = Polygon(dc, (POINT[]){{7,41},{11,28},{33,6},{42,15},{20,37}}, 5);
        ok = ok && Polyline(dc, (POINT[]){{27,12},{36,21}}, 2);
        break;
    case ACT_TEXT:
        ok = Polyline(dc, (POINT[]){{8,13},{8,8},{40,8},{40,13}}, 4);
        ok = ok && Polyline(dc, (POINT[]){{24,8},{24,40}}, 2);
        ok = ok && Polyline(dc, (POINT[]){{15,40},{33,40}}, 2);
        break;
    case ACT_UNDO:
    case ACT_REDO: {
        int dir = action == ACT_UNDO ? 1 : -1;
        ok = PolyBezier(dc, (POINT[]){{24-17*dir,21},{24,21},{24+17*dir,17},{24+17*dir,38}}, 4);
        ok = ok && Polyline(dc, (POINT[]){{24-7*dir,11},{24-17*dir,21},{24-7*dir,31}}, 3);
        break;
    }
    case ACT_SAVE:
        ok = Polygon(dc, (POINT[]){{7,6},{33,6},{41,14},{41,42},{7,42}}, 5);
        ok = ok && Polyline(dc, (POINT[]){{15,6},{15,19},{32,19},{32,6}}, 4);
        ok = ok && Polyline(dc, (POINT[]){{15,42},{15,26},{33,26},{33,42}}, 4);
        break;
    case ACT_COPY:
        ok = Polyline(dc, (POINT[]){{29,6},{7,6},{7,34}}, 3);
        ok = ok && RoundRect(dc, 16, 15, 41, 42, 4, 4);
        break;
    default: {
        int index = action == ACT_COLOR ? p->color : action - ACT_RED;
        if ((unsigned)index < COLORS) {
            ok = SelectObject(dc, GetStockObject(DC_BRUSH)) &&
                SetDCBrushColor(dc, palette[index]) != CLR_INVALID && Ellipse(dc, 10, 10, 38, 38);
        }
        break;
    }
    }
done:
    ok = RestoreDC(dc, saved) && ok;
    if (pen) DeleteObject(pen);
    return ok;
}

static void aa_glyph(HDC dc, RECT r, int action, const Panel *p, COLORREF ink, COLORREF face) {
    int w = r.right - r.left, h = r.bottom - r.top;
    int side = (w < h ? w : h) * 2 / 3;
    if (side <= 0) return;
    int x = r.left + (w - side) / 2, y = r.top + (h - side) / 2;
    int sample = side * 4;
    HDC tile = CreateCompatibleDC(dc);
    HBITMAP bitmap = tile ? CreateCompatibleBitmap(dc, sample, sample) : NULL;
    HGDIOBJ old = bitmap ? SelectObject(tile, bitmap) : NULL;
    BOOL drawn = FALSE;
    if (old) {
        SetDCBrushColor(tile, face);
        if (FillRect(tile, &(RECT){0,0,sample,sample}, GetStockObject(DC_BRUSH)) &&
            draw_glyph(tile, action, p, ink, (float)sample / 24, 0, 0) &&
            SetStretchBltMode(dc, HALFTONE) && SetBrushOrgEx(dc, 0, 0, NULL))
            drawn = StretchBlt(dc, x, y, side, side, tile, 0, 0, sample, sample, SRCCOPY);
        SelectObject(tile, old);
    }
    if (bitmap) DeleteObject(bitmap);
    if (tile) DeleteDC(tile);
    if (!drawn) draw_glyph(dc, action, p, ink, (float)side / 24, x, y);
}
#endif

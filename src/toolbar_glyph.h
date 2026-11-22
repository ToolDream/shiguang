#ifndef SHIGUANG_TOOLBAR_GLYPH_H
#define SHIGUANG_TOOLBAR_GLYPH_H
#include "editor_win.h"

typedef struct { short x, y; BYTE op; } GlyphPoint;
#include "toolbar_glyph_data.h"

typedef struct { const GlyphPoint *points; int count; } Glyph;
#define GLYPH(name) {glyph_##name, ARRAYSIZE(glyph_##name)}
static const Glyph shape_glyphs[SHAPES] = {
    [RECTANGLE] = GLYPH(square), [CIRCLE] = GLYPH(circle), [ELLIPSE] = GLYPH(circle),
    [TRIANGLE] = GLYPH(triangle), [DIAMOND] = GLYPH(diamond),
};
static const Glyph action_glyphs[BAR_BUTTONS] = {
    [ACT_ARROW] = GLYPH(arrow_up_right), [ACT_PEN] = GLYPH(pencil), [ACT_TEXT] = GLYPH(type),
    [ACT_UNDO] = GLYPH(undo_2), [ACT_REDO] = GLYPH(redo_2), [ACT_SAVE] = GLYPH(save),
    [ACT_COPY] = GLYPH(copy),
};
static const Glyph chevron_glyph = GLYPH(chevron_down);
#undef GLYPH

static BOOL draw_glyph(HDC dc, Glyph glyph, BOOL squash, BOOL fill, COLORREF swatch, COLORREF ink,
                       float scale, int x, int y) {
    int saved = SaveDC(dc);
    if (!saved) return FALSE;
    XFORM transform = {scale / GLYPH_GRID, 0, 0, scale / GLYPH_GRID, (float)x, (float)y};
    LOGBRUSH color = {BS_SOLID, ink, 0};
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                            2 * GLYPH_GRID, &color, 0, NULL);
    BOOL ok = FALSE;
    if (!pen || !SetGraphicsMode(dc, GM_ADVANCED) || !SetWorldTransform(dc, &transform) ||
        !SelectObject(dc, pen)) goto done;
    if (fill && (!SelectObject(dc, GetStockObject(DC_BRUSH)) ||
                 SetDCBrushColor(dc, swatch) == CLR_INVALID)) goto done;

    POINT points[GLYPH_MAX_POINTS];
    BYTE ops[GLYPH_MAX_POINTS];
    const int center = 12 * GLYPH_GRID;
    for (int i = 0; i < glyph.count; ++i) {
        int dx = glyph.points[i].x - center, dy = glyph.points[i].y - center;
        // Derive the ellipse and filled swatch from Lucide's circle without scaling the pen.
        points[i] = (POINT){center + (fill ? dx * 7 / 10 : dx),
                            center + (fill ? dy * 7 / 10 : squash ? dy * 13 / 20 : dy)};
        ops[i] = glyph.points[i].op;
    }
    if (BeginPath(dc)) {
        ok = PolyDraw(dc, points, ops, glyph.count) && EndPath(dc) &&
            (fill ? StrokeAndFillPath(dc) : StrokePath(dc));
        if (!ok) AbortPath(dc);
    }
done:
    ok = RestoreDC(dc, saved) && ok;
    if (pen) DeleteObject(pen);
    return ok;
}

static void aa_tile(HDC dc, int x, int y, int side, Glyph glyph, BOOL squash, BOOL fill,
                    COLORREF swatch, COLORREF ink, COLORREF face) {
    int sample = side * 4;
    HDC tile = CreateCompatibleDC(dc);
    HBITMAP bitmap = tile ? CreateCompatibleBitmap(dc, sample, sample) : NULL;
    HGDIOBJ old = bitmap ? SelectObject(tile, bitmap) : NULL;
    BOOL drawn = FALSE;
    if (old) {
        SetDCBrushColor(tile, face);
        if (FillRect(tile, &(RECT){0,0,sample,sample}, GetStockObject(DC_BRUSH)) &&
            draw_glyph(tile, glyph, squash, fill, swatch, ink, (float)sample / 24, 0, 0) &&
            SetStretchBltMode(dc, HALFTONE) && SetBrushOrgEx(dc, 0, 0, NULL))
            drawn = StretchBlt(dc, x, y, side, side, tile, 0, 0, sample, sample, SRCCOPY);
        SelectObject(tile, old);
    }
    if (bitmap) DeleteObject(bitmap);
    if (tile) DeleteDC(tile);
    if (!drawn) draw_glyph(dc, glyph, squash, fill, swatch, ink, (float)side / 24, x, y);
}

static void aa_glyph(HDC dc, RECT r, int action, const Panel *p, COLORREF ink, COLORREF face) {
    int w = r.right - r.left, h = r.bottom - r.top;
    int extent = w < h ? w : h;
    int side = extent * 2 / 3;
    if (side <= 0) return;

    BOOL menu = action == ACT_SHAPES || action == ACT_COLOR;
    int shape = action == ACT_SHAPES ? (int)p->shape : action - ACT_RECTANGLE;
    int index = action == ACT_COLOR ? p->color : action - ACT_RED;
    BOOL fill = (unsigned)index < COLORS;
    Glyph glyph = fill ? shape_glyphs[CIRCLE] : (unsigned)shape < SHAPES ? shape_glyphs[shape] :
                  (unsigned)action < BAR_BUTTONS ? action_glyphs[action] : (Glyph){0};
    if (!glyph.count) return;

    int badge = menu ? extent / 4 : 0;
    int x = r.left + (w - side - badge) / 2;
    int y = r.top + (h - side) / 2;
    aa_tile(dc, x, y, side, glyph, !fill && shape == ELLIPSE, fill, fill ? palette[index] : 0, ink, face);
    if (badge)
        aa_tile(dc, r.right - (w - side - badge) / 2 - badge, r.bottom - extent / 12 - badge,
                badge, chevron_glyph, FALSE, FALSE, 0, ink, face);
}
#endif

#ifndef SHIGUANG_TOOLBAR_GLYPH_H
#define SHIGUANG_TOOLBAR_GLYPH_H
#include "editor_win.h"
#include "ui_draw_win.h"

typedef struct { short x, y; BYTE op; } GlyphPoint;
#include "toolbar_glyph_data.h"

typedef struct {
    const GlyphPoint *points;
    int count;
    const GlyphPoint *fill;
    int fill_count;
} Glyph;

#define GLYPH_S(name) {glyph_##name, ARRAYSIZE(glyph_##name), NULL, 0}
#define GLYPH_SF(name) {glyph_##name, ARRAYSIZE(glyph_##name), glyph_##name##_fill, ARRAYSIZE(glyph_##name##_fill)}
#define GLYPH_F(name) {NULL, 0, glyph_##name##_fill, ARRAYSIZE(glyph_##name##_fill)}

static const Glyph shape_glyphs[SHAPES] = {
    [RECTANGLE] = GLYPH_S(rect),
    [CIRCLE] = GLYPH_S(circle),
    [ELLIPSE] = GLYPH_S(circle),
    [TRIANGLE] = GLYPH_S(triangle),
    [DIAMOND] = GLYPH_S(diamond),
};
static const Glyph action_glyphs[BAR_BUTTONS] = {
    [ACT_ARROW] = GLYPH_SF(arrow),
    [ACT_PEN] = GLYPH_S(pen),
    [ACT_TEXT] = GLYPH_S(text),
    [ACT_UNDO] = GLYPH_S(undo),
    [ACT_REDO] = GLYPH_S(redo),
    [ACT_SAVE] = GLYPH_S(save),
    [ACT_COPY] = GLYPH_S(check),
};
static const Glyph caret_glyph = GLYPH_F(caret);

#undef GLYPH_S
#undef GLYPH_SF
#undef GLYPH_F

static BOOL draw_poly_path(HDC dc, const GlyphPoint *src, int count, BOOL squash, int scale_pct, BOOL fill_only) {
    POINT points[GLYPH_MAX_POINTS];
    BYTE ops[GLYPH_MAX_POINTS];
    const int center = 12 * GLYPH_GRID;
    for (int i = 0; i < count; ++i) {
        int dx = src[i].x - center, dy = src[i].y - center;
        points[i] = (POINT){
            center + dx * scale_pct / 100,
            center + (squash ? dy * 68 / 100 : dy * scale_pct / 100)
        };
        ops[i] = src[i].op;
    }
    if (!BeginPath(dc)) return FALSE;
    BOOL ok = PolyDraw(dc, points, ops, count) && EndPath(dc) &&
        (fill_only ? FillPath(dc) : StrokePath(dc));
    if (!ok) AbortPath(dc);
    return ok;
}

static BOOL draw_glyph(HDC dc, Glyph glyph, BOOL squash, BOOL swatch_mode,
                       COLORREF swatch, COLORREF ring_color, COLORREF ink,
                       int weight, float scale, int x, int y) {
    int saved = SaveDC(dc);
    if (!saved) return FALSE;
    XFORM transform = {scale / GLYPH_GRID, 0, 0, scale / GLYPH_GRID, (float)x, (float)y};
    BOOL ok = SetGraphicsMode(dc, GM_ADVANCED) && SetWorldTransform(dc, &transform);
    HPEN pen = NULL;
    if (!ok) goto done;

    if (swatch_mode) {
        // Pure saturated color dot + subtle outer halo ring (no heavy dark border).
        LOGBRUSH ring_lb = {BS_SOLID, ring_color, 0};
        pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                           11 * GLYPH_GRID / 10, &ring_lb, 0, NULL);
        ok = pen && SelectObject(dc, pen) &&
             draw_poly_path(dc, glyph.points, glyph.count, FALSE, 132, FALSE);
        if (ok) {
            ok = SelectObject(dc, GetStockObject(DC_BRUSH)) &&
                 SetDCBrushColor(dc, swatch) != CLR_INVALID &&
                 draw_poly_path(dc, glyph.points, glyph.count, FALSE, 98, TRUE);
        }
        goto done;
    }

    if (glyph.count > 0) {
        LOGBRUSH color = {BS_SOLID, ink, 0};
        pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                           weight, &color, 0, NULL);
        ok = pen && SelectObject(dc, pen) &&
             draw_poly_path(dc, glyph.points, glyph.count, squash, 100, FALSE);
    }
    if (ok && glyph.fill_count > 0) {
        ok = SelectObject(dc, GetStockObject(DC_BRUSH)) &&
             SetDCBrushColor(dc, ink) != CLR_INVALID &&
             draw_poly_path(dc, glyph.fill, glyph.fill_count, squash, 100, TRUE);
    }
done:
    ok = RestoreDC(dc, saved) && ok;
    if (pen) DeleteObject(pen);
    return ok;
}

static void aa_tile(HDC dc, Rect tile_rect, Glyph glyph, BOOL squash, BOOL swatch_mode,
                    COLORREF swatch, COLORREF ring_color, COLORREF ink, COLORREF face, int weight) {
    int x = tile_rect.left, y = tile_rect.top, side = tile_rect.right - x;
    if (side <= 0) return;
    UiSsaaSurface s;
    BOOL drawn = FALSE;
    if (ui_ssaa_begin(&s, dc, side, side, face)) {
        if (draw_glyph(s.dc, glyph, squash, swatch_mode, swatch, ring_color, ink,
                       weight, (float)(side * 4) / 24.0f, 0, 0))
            drawn = ui_ssaa_present(&s, dc, x, y);
        else
            ui_ssaa_abort(&s);
    }
    if (!drawn)
        draw_glyph(dc, glyph, squash, swatch_mode, swatch, ring_color, ink,
                   weight, (float)side / 24.0f, x, y);
}

static void aa_glyph(HDC dc, RECT r, int dpi, int action, const Panel *p, COLORREF ink, COLORREF face) {
    GlyphLayout layout = editor_glyph_layout((Rect){r.left, r.top, r.right, r.bottom}, dpi,
                                             action == ACT_SHAPES || action == ACT_COLOR);
    if (rect_empty(layout.icon)) return;
    int shape = action == ACT_SHAPES ? (int)p->shape : action - ACT_RECTANGLE;
    int index = action == ACT_COLOR ? p->color : action - ACT_RED;
    BOOL swatch_mode = (unsigned)index < COLORS;
    Glyph glyph = swatch_mode ? shape_glyphs[CIRCLE] :
                  (unsigned)shape < SHAPES ? shape_glyphs[shape] :
                  (unsigned)action < BAR_BUTTONS ? action_glyphs[action] : (Glyph){0};
    if (!glyph.count && !glyph.fill_count) return;

    const UiPalette *c = &ui_colors;
    COLORREF ring_color = c->high_contrast ? ink
        : (action == ACT_RED + p->color ? c->accent : c->edge);
    int stroke_weight = action == ACT_COPY ? (24 * GLYPH_GRID / 10) : (19 * GLYPH_GRID / 10);
    aa_tile(dc, layout.icon, glyph, !swatch_mode && shape == ELLIPSE, swatch_mode,
            swatch_mode ? palette[index] : 0, ring_color, ink, face, stroke_weight);
    if (!rect_empty(layout.badge)) {
        COLORREF badge_ink = c->high_contrast ? ink
            : (action == ACT_SHAPES && p->tool <= DIAMOND ? c->active_ink : c->muted);
        aa_tile(dc, layout.badge, caret_glyph, FALSE, FALSE, 0, 0, badge_ink, face, 0);
    }
}
#endif

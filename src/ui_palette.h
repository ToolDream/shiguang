#ifndef SHIGUANG_UI_PALETTE_H
#define SHIGUANG_UI_PALETTE_H

#include <stdbool.h>
#include <stdint.h>

// Use COLORREF byte order without depending on Win32 for palette/contrast tests.
#define UI_RGB(r, g, b) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16))

typedef struct {
    uint32_t surface, ink, muted, edge, hover, active, active_ink, pressed, accent, focus;
    uint32_t hint, hint_ink, warning;
    bool high_contrast;
} UiPalette;

typedef struct { uint32_t fill, ink; } UiFace;

static inline UiPalette ui_normal_palette(bool dark) {
    const uint32_t sage = UI_RGB(112, 132, 104);
    if (dark) return (UiPalette){
        .surface = UI_RGB(37, 40, 36), .ink = UI_RGB(239, 239, 231), .muted = UI_RGB(170, 175, 164),
        .edge = UI_RGB(85, 91, 80), .hover = UI_RGB(52, 56, 50), .active = UI_RGB(64, 78, 56),
        .active_ink = UI_RGB(223, 234, 215), .pressed = UI_RGB(76, 94, 68), .accent = sage, .focus = UI_RGB(163, 186, 150),
        .hint = UI_RGB(43, 49, 42), .hint_ink = UI_RGB(244, 244, 234), .warning = UI_RGB(239, 188, 113)
    };
    return (UiPalette){
        .surface = UI_RGB(253, 251, 245), .ink = UI_RGB(48, 54, 47), .muted = UI_RGB(95, 104, 91),
        .edge = UI_RGB(198, 203, 191), .hover = UI_RGB(239, 239, 231), .active = UI_RGB(229, 236, 220),
        .active_ink = UI_RGB(47, 69, 41), .pressed = UI_RGB(206, 222, 194), .accent = sage, .focus = UI_RGB(83, 107, 76),
        .hint = UI_RGB(48, 55, 46), .hint_ink = UI_RGB(253, 251, 245), .warning = UI_RGB(134, 82, 25)
    };
}

static inline UiFace ui_primary_face(const UiPalette *p, bool pressed) {
    if (p->high_contrast && !pressed) return (UiFace){p->surface, p->ink};
    return (UiFace){pressed ? p->pressed : p->active, p->active_ink};
}

static inline bool ui_palette_equal(const UiPalette *a, const UiPalette *b) {
    return a->surface == b->surface && a->ink == b->ink && a->muted == b->muted &&
        a->edge == b->edge && a->hover == b->hover && a->active == b->active &&
        a->active_ink == b->active_ink && a->pressed == b->pressed && a->accent == b->accent &&
        a->focus == b->focus && a->hint == b->hint && a->hint_ink == b->hint_ink &&
        a->warning == b->warning && a->high_contrast == b->high_contrast;
}

#undef UI_RGB
#endif

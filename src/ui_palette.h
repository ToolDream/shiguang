#ifndef SHIGUANG_UI_PALETTE_H
#define SHIGUANG_UI_PALETTE_H

#include <stdbool.h>
#include <stdint.h>

// Use COLORREF byte order without depending on Win32 for palette/contrast tests.
#define UI_RGB(r, g, b) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16))

typedef struct {
    uint32_t surface, sidebar, card, card_edge, banner, banner_edge;
    uint32_t ink, muted, edge, hover, active, active_ink, pressed;
    uint32_t accent, accent_pressed, accent_ink, focus;
    uint32_t hint, hint_ink, warning, success;
    bool high_contrast;
} UiPalette;

typedef struct {
    uint32_t surface, sidebar, card, card_edge, divider, keycap, banner, banner_edge;
    uint32_t ink, muted, edge, hover, active, active_ink, pressed;
    uint32_t accent, accent_pressed, accent_ink, focus;
    uint32_t warning, success;
    bool high_contrast;
} SettingsPalette;

typedef struct { uint32_t fill, ink; } UiFace;

static inline UiPalette ui_normal_palette(bool dark) {
    const uint32_t brand = UI_RGB(37, 110, 190);
    const uint32_t brand_pressed = UI_RGB(27, 89, 158);
    if (dark) return (UiPalette){
        .surface = UI_RGB(28, 33, 43),
        .sidebar = UI_RGB(21, 25, 33),
        .card = UI_RGB(36, 43, 56),
        .card_edge = UI_RGB(51, 61, 79),
        .banner = UI_RGB(26, 44, 68),
        .banner_edge = UI_RGB(45, 74, 112),
        .ink = UI_RGB(241, 245, 249),
        .muted = UI_RGB(148, 163, 184),
        .edge = UI_RGB(62, 74, 96),
        .hover = UI_RGB(42, 51, 66),
        .active = UI_RGB(30, 58, 95),
        .active_ink = UI_RGB(157, 205, 255),
        .pressed = UI_RGB(38, 72, 116),
        .accent = brand,
        .accent_pressed = brand_pressed,
        .accent_ink = UI_RGB(255, 255, 255),
        .focus = UI_RGB(96, 165, 250),
        .hint = UI_RGB(19, 24, 33),
        .hint_ink = UI_RGB(248, 250, 252),
        .warning = UI_RGB(251, 191, 36),
        .success = UI_RGB(52, 211, 153),
    };
    return (UiPalette){
        .surface = UI_RGB(255, 255, 255),
        .sidebar = UI_RGB(244, 246, 250),
        .card = UI_RGB(248, 250, 252),
        .card_edge = UI_RGB(229, 234, 241),
        .banner = UI_RGB(239, 246, 255),
        .banner_edge = UI_RGB(191, 219, 254),
        .ink = UI_RGB(15, 23, 42),
        .muted = UI_RGB(100, 116, 139),
        .edge = UI_RGB(203, 213, 225),
        .hover = UI_RGB(241, 245, 249),
        .active = UI_RGB(230, 240, 252),
        .active_ink = UI_RGB(29, 93, 163),
        .pressed = UI_RGB(210, 229, 250),
        .accent = brand,
        .accent_pressed = brand_pressed,
        .accent_ink = UI_RGB(255, 255, 255),
        .focus = UI_RGB(37, 115, 194),
        .hint = UI_RGB(24, 32, 44),
        .hint_ink = UI_RGB(248, 250, 252),
        .warning = UI_RGB(180, 83, 9),
        .success = UI_RGB(22, 163, 74),
    };
}

static inline SettingsPalette ui_settings_palette(bool dark) {
    const uint32_t brand = UI_RGB(38, 118, 184);
    const uint32_t brand_pressed = UI_RGB(29, 96, 154);
    if (dark) return (SettingsPalette){
        .surface = UI_RGB(28, 33, 43),
        .sidebar = UI_RGB(22, 27, 36),
        .card = UI_RGB(35, 42, 54),
        .card_edge = UI_RGB(52, 64, 82),
        .divider = UI_RGB(45, 55, 71),
        .keycap = UI_RGB(43, 52, 67),
        .banner = UI_RGB(26, 46, 68),
        .banner_edge = UI_RGB(46, 79, 114),
        .ink = UI_RGB(236, 242, 248),
        .muted = UI_RGB(148, 166, 184),
        .edge = UI_RGB(68, 82, 104),
        .hover = UI_RGB(42, 52, 67),
        .active = UI_RGB(31, 58, 88),
        .active_ink = UI_RGB(165, 210, 255),
        .pressed = UI_RGB(40, 74, 112),
        .accent = brand,
        .accent_pressed = brand_pressed,
        .accent_ink = UI_RGB(255, 255, 255),
        .focus = UI_RGB(96, 165, 250),
        .warning = UI_RGB(251, 191, 36),
        .success = UI_RGB(68, 184, 154),
    };
    return (SettingsPalette){
        .surface = UI_RGB(255, 255, 255),
        .sidebar = UI_RGB(246, 248, 252),
        .card = UI_RGB(255, 255, 255),
        .card_edge = UI_RGB(223, 231, 240),
        .divider = UI_RGB(232, 237, 243),
        .keycap = UI_RGB(245, 248, 252),
        .banner = UI_RGB(240, 247, 253),
        .banner_edge = UI_RGB(213, 230, 245),
        .ink = UI_RGB(32, 48, 69),
        .muted = UI_RGB(82, 100, 122),
        .edge = UI_RGB(204, 217, 231),
        .hover = UI_RGB(238, 244, 250),
        .active = UI_RGB(230, 240, 251),
        .active_ink = UI_RGB(28, 88, 143),
        .pressed = UI_RGB(214, 231, 248),
        .accent = brand,
        .accent_pressed = brand_pressed,
        .accent_ink = UI_RGB(255, 255, 255),
        .focus = brand,
        .warning = UI_RGB(180, 83, 9),
        .success = UI_RGB(43, 131, 103),
    };
}

static inline UiFace ui_primary_face(const UiPalette *p, bool pressed) {
    if (p->high_contrast && !pressed) return (UiFace){p->surface, p->ink};
    return (UiFace){pressed ? p->pressed : p->active, p->active_ink};
}

static inline UiFace ui_accent_face(const UiPalette *p, bool pressed) {
    if (p->high_contrast && !pressed) return (UiFace){p->surface, p->ink};
    if (p->high_contrast) return (UiFace){p->active, p->active_ink};
    return (UiFace){pressed ? p->accent_pressed : p->accent, p->accent_ink};
}

static inline UiFace ui_settings_primary_face(const SettingsPalette *p, bool pressed) {
    if (p->high_contrast && !pressed) return (UiFace){p->surface, p->ink};
    return (UiFace){pressed ? p->pressed : p->active, p->active_ink};
}

static inline UiFace ui_settings_accent_face(const SettingsPalette *p, bool pressed) {
    if (p->high_contrast && !pressed) return (UiFace){p->surface, p->ink};
    if (p->high_contrast) return (UiFace){p->active, p->active_ink};
    return (UiFace){pressed ? p->accent_pressed : p->accent, p->accent_ink};
}

static inline bool ui_palette_equal(const UiPalette *a, const UiPalette *b) {
    return a->surface == b->surface && a->sidebar == b->sidebar &&
        a->card == b->card && a->card_edge == b->card_edge &&
        a->banner == b->banner && a->banner_edge == b->banner_edge &&
        a->ink == b->ink && a->muted == b->muted &&
        a->edge == b->edge && a->hover == b->hover && a->active == b->active &&
        a->active_ink == b->active_ink && a->pressed == b->pressed &&
        a->accent == b->accent && a->accent_pressed == b->accent_pressed &&
        a->accent_ink == b->accent_ink && a->focus == b->focus &&
        a->hint == b->hint && a->hint_ink == b->hint_ink &&
        a->warning == b->warning && a->success == b->success &&
        a->high_contrast == b->high_contrast;
}

static inline bool ui_settings_palette_equal(const SettingsPalette *a, const SettingsPalette *b) {
    return a->surface == b->surface && a->sidebar == b->sidebar &&
        a->card == b->card && a->card_edge == b->card_edge &&
        a->divider == b->divider && a->keycap == b->keycap &&
        a->banner == b->banner && a->banner_edge == b->banner_edge &&
        a->ink == b->ink && a->muted == b->muted &&
        a->edge == b->edge && a->hover == b->hover && a->active == b->active &&
        a->active_ink == b->active_ink && a->pressed == b->pressed &&
        a->accent == b->accent && a->accent_pressed == b->accent_pressed &&
        a->accent_ink == b->accent_ink && a->focus == b->focus &&
        a->warning == b->warning && a->success == b->success &&
        a->high_contrast == b->high_contrast;
}

#undef UI_RGB
#endif

#include "../src/ui_palette.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static double linear(unsigned byte) {
    double s = byte / 255.0;
    return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}

static double luminance(uint32_t color) {
    return 0.2126 * linear(color & 255u) + 0.7152 * linear((color >> 8) & 255u) +
        0.0722 * linear((color >> 16) & 255u);
}

static double contrast(UiFace face) {
    double a = luminance(face.fill), b = luminance(face.ink);
    return (fmax(a, b) + 0.05) / (fmin(a, b) + 0.05);
}

static void test_toolbar_palette_scope(void) {
    // Protect screenshot toolbar/overlay shared palette from settings-only restyling.
    UiPalette light = ui_normal_palette(false);
    UiPalette dark = ui_normal_palette(true);
    assert(light.surface == (255u | (255u << 8) | (255u << 16)));
    assert(light.card_edge == (229u | (234u << 8) | (241u << 16)));
    assert(light.edge == (203u | (213u << 8) | (225u << 16)));
    assert(light.ink == (15u | (23u << 8) | (42u << 16)));
    assert(light.accent == (37u | (110u << 8) | (190u << 16)));
    assert(dark.surface == (28u | (33u << 8) | (43u << 16)));
    assert(dark.sidebar == (21u | (25u << 8) | (33u << 16)));
    assert(dark.card == (36u | (43u << 8) | (56u << 16)));
    assert(dark.card_edge == (51u | (61u << 8) | (79u << 16)));
    assert(dark.edge == (62u | (74u << 8) | (96u << 16)));
    assert(dark.ink == (241u | (245u << 8) | (249u << 16)));
    assert(dark.accent == light.accent);
}

static void test_primary_face(bool dark) {
    UiPalette palette = ui_normal_palette(dark);
    UiFace normal = ui_primary_face(&palette, false);
    UiFace pressed = ui_primary_face(&palette, true);
    UiFace accent = ui_accent_face(&palette, false);
    UiFace accent_pressed = ui_accent_face(&palette, true);
    printf("%s toolbar primary %.2f:1 / %.2f:1, accent %.2f:1 / %.2f:1\n",
           dark ? "dark" : "light", contrast(normal), contrast(pressed),
           contrast(accent), contrast(accent_pressed));
    fflush(stdout);
    assert(contrast(normal) >= 4.5 && contrast(pressed) >= 4.5);
    assert(contrast(accent) >= 4.5 && contrast(accent_pressed) >= 4.5);
    assert(normal.fill != pressed.fill && accent.fill != accent_pressed.fill);
}

static void test_settings_palette(bool dark) {
    SettingsPalette palette = ui_settings_palette(dark);
    UiFace normal = ui_settings_primary_face(&palette, false);
    UiFace pressed = ui_settings_primary_face(&palette, true);
    UiFace accent = ui_settings_accent_face(&palette, false);
    UiFace accent_pressed = ui_settings_accent_face(&palette, true);
    UiFace body = {palette.surface, palette.ink};
    UiFace card = {palette.card, palette.ink};
    UiFace banner = {palette.banner, palette.ink};
    UiFace keycap = {palette.keycap, palette.ink};
    printf("%s settings primary %.2f:1 / %.2f:1, accent %.2f:1 / %.2f:1, ink %.2f:1\n",
           dark ? "dark" : "light", contrast(normal), contrast(pressed),
           contrast(accent), contrast(accent_pressed), contrast(body));
    fflush(stdout);
    assert(contrast(normal) >= 4.5 && contrast(pressed) >= 4.5);
    assert(contrast(accent) >= 4.5 && contrast(accent_pressed) >= 4.5);
    assert(contrast(body) >= 4.5 && contrast(card) >= 4.5);
    assert(contrast(banner) >= 4.5 && contrast(keycap) >= 4.5);
    assert(normal.fill != pressed.fill && accent.fill != accent_pressed.fill);
}

static void test_invalidation(void) {
    UiPalette palette = ui_normal_palette(false), next = palette;
    assert(ui_palette_equal(&palette, &next));
#define CHANGE(field) do { next = palette; next.field ^= 1; assert(!ui_palette_equal(&palette, &next)); } while (0)
    CHANGE(surface); CHANGE(sidebar); CHANGE(card); CHANGE(card_edge);
    CHANGE(banner); CHANGE(banner_edge);
    CHANGE(ink); CHANGE(muted); CHANGE(edge); CHANGE(hover);
    CHANGE(active); CHANGE(active_ink); CHANGE(pressed);
    CHANGE(accent); CHANGE(accent_pressed); CHANGE(accent_ink); CHANGE(focus);
    CHANGE(hint); CHANGE(hint_ink); CHANGE(warning); CHANGE(success); CHANGE(high_contrast);
#undef CHANGE

    SettingsPalette sp = ui_settings_palette(false), snext = sp;
    assert(ui_settings_palette_equal(&sp, &snext));
#define SCHANGE(field) do { snext = sp; snext.field ^= 1; assert(!ui_settings_palette_equal(&sp, &snext)); } while (0)
    SCHANGE(surface); SCHANGE(sidebar); SCHANGE(card); SCHANGE(card_edge);
    SCHANGE(divider); SCHANGE(keycap); SCHANGE(banner); SCHANGE(banner_edge);
    SCHANGE(ink); SCHANGE(muted); SCHANGE(edge); SCHANGE(hover);
    SCHANGE(active); SCHANGE(active_ink); SCHANGE(pressed);
    SCHANGE(accent); SCHANGE(accent_pressed); SCHANGE(accent_ink); SCHANGE(focus);
    SCHANGE(warning); SCHANGE(success); SCHANGE(high_contrast);
#undef SCHANGE
}

int main(void) {
    test_toolbar_palette_scope();
    test_primary_face(false);
    test_primary_face(true);
    test_settings_palette(false);
    test_settings_palette(true);
    test_invalidation();
    puts("6 UI palette tests passed");
}

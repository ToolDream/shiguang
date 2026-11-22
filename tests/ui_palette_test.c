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

static void test_primary_face(bool dark) {
    UiPalette palette = ui_normal_palette(dark);
    UiFace normal = ui_primary_face(&palette, false);
    UiFace pressed = ui_primary_face(&palette, true);
    printf("%s primary normal %.2f:1, pressed %.2f:1\n",
           dark ? "dark" : "light", contrast(normal), contrast(pressed));
    fflush(stdout);
    assert(contrast(normal) >= 4.5 && contrast(pressed) >= 4.5);
    assert(normal.fill != pressed.fill);
}

static void test_invalidation(void) {
    UiPalette palette = ui_normal_palette(false), next = palette;
    assert(ui_palette_equal(&palette, &next));
#define CHANGE(field) do { next = palette; next.field ^= 1; assert(!ui_palette_equal(&palette, &next)); } while (0)
    CHANGE(surface); CHANGE(ink); CHANGE(muted); CHANGE(edge); CHANGE(hover);
    CHANGE(active); CHANGE(active_ink); CHANGE(pressed); CHANGE(accent); CHANGE(focus);
    CHANGE(hint); CHANGE(hint_ink); CHANGE(warning); CHANGE(high_contrast);
#undef CHANGE
}

int main(void) {
    test_primary_face(false);
    test_primary_face(true);
    test_invalidation();
    puts("3 UI palette tests passed");
}

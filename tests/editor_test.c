#include "../src/editor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void history_test(void) {
    History h = {0};
    assert(!history_undo(&h) && !history_redo(&h));
    Mark first = {.tool = RECTANGLE, .x = 2, .y = 3};
    Mark stroke = {.tool = PEN, .points = malloc(sizeof(EditPoint)), .count = 1};
    assert(stroke.points);
    stroke.points[0] = (EditPoint){9, 12};
    assert(history_add(&h, &first) && history_add(&h, &stroke));
    assert(!stroke.points && h.used == 2 && h.marks[1].points[0].y == 12);
    assert(history_undo(&h) && h.used == 1 && h.marks[0].x == 2);
    assert(history_redo(&h) && h.used == 2 && !history_redo(&h));
    assert(history_undo(&h));
    Mark replaced = {.tool = DIAMOND, .x = 17};
    assert(history_add(&h, &replaced) && !history_redo(&h));
    assert(h.marks[1].tool == DIAMOND && h.marks[1].x == 17);
    history_clear(&h);
    assert(!history_undo(&h) && !history_redo(&h));
}

static void layout_test(void) {
    Rect screens[] = {{-800, -200, 0, 600}, {0, 0, 1024, 768}};
    Rect area = {-520, 80, -40, 300};
    Dock d = editor_dock(area, screens[0], 96, 8, 9);
    assert(d.cell == 48 && d.cols == 9 && d.bounds.left == -472 &&
           d.bounds.right == -40 && d.bounds.top == 308);
    Dock list = editor_popup(d, screens[0], 8, 5);
    assert(list.bounds.left == -472 && list.bounds.top == 364 && list.cols == 5);
    d = editor_dock((Rect){-520, 420, -40, 600}, screens[0], 96, 8, 9);
    assert(d.bounds.top == 364 && d.bounds.bottom == 412);
    d = editor_dock((Rect){5, 0, 1024, 768}, screens[1], 96, 8, 9);
    assert(d.bounds.top == 712 && d.bounds.bottom == 760);
    list = editor_popup(d, screens[1], 8, 6);
    assert(list.bounds.bottom == 704 && list.bounds.top == 656);
    d = editor_dock(area, screens[0], 144, 12, 9);
    assert(d.cell == 72 && d.cols == 9 && d.bounds.top == 312);
    d = editor_dock((Rect){80, 50, 410, 90}, (Rect){0, 0, 410, 260}, 96, 8, 9);
    assert(d.cell == 44 && d.cols == 9 && d.bounds.left == 14);
    d = editor_dock((Rect){80, 10, 410, 40}, (Rect){0, 0, 600, 96}, 96, 8, 9);
    assert(d.cell == 44 && d.cols == 9 && d.bounds.top == 48);
    d = editor_dock((Rect){-90, -30, -5, 8}, (Rect){-100, -40, 0, 152}, 96, 8, 9);
    assert(d.cols == 2 && d.cell == 38 && d.bounds.top == -40);
    assert(d.bounds.right <= 0 && d.bounds.bottom <= 152);
    assert((9 + d.cols - 1) / d.cols * d.cell == d.bounds.bottom - d.bounds.top);
    assert(editor_screen((Rect){-700, -110, 200, -40}, screens, 2) == 0);
    assert(editor_screen((Rect){-700, -110, 200, 620}, screens, 2) == 1);

    // At this anchor, 48 DIP has no vertical room for a popup even though two rows fit on screen.
    const struct { int width, right, cell, popup_x; } popup_cases[] = {
        {432, 432, 44, 36},  // Both new popups would overlap the bar.
        {704, 432, 44, 36},  // The shape popup fits at the right, but the color popup does not.
        {728, 432, 48, 440}, // Both popups can open to the right.
        {728, 728, 48, 0},   // Both popups can open to the left (color starts at x=0).
    };
    for (size_t i = 0; i < sizeof popup_cases / sizeof *popup_cases; ++i) {
        Rect screen = {0, 0, popup_cases[i].width, 112};
        d = editor_dock((Rect){0, 0, popup_cases[i].right, 4}, screen, 96, 8, BAR_BUTTONS);
        Dock shapes = editor_popup(d, screen, 8, SHAPES);
        Dock colors = editor_popup(d, screen, 8, COLORS);
        assert(rect_empty(intersect_rect(d.bounds, shapes.bounds)) && "shape popup covers the toolbar");
        assert(rect_empty(intersect_rect(d.bounds, colors.bounds)) && "color popup covers the toolbar");
        assert(d.cell == popup_cases[i].cell && colors.bounds.left == popup_cases[i].popup_x);
    }
    // Only the compact shape popup has room to the right; color already overlapped before.
    Rect short_screen = {0, 0, 660, 104};
    d = editor_dock((Rect){0, 0, 432, 4}, short_screen, 96, 8, BAR_BUTTONS);
    list = editor_popup(d, short_screen, 8, SHAPES);
    assert(rect_empty(intersect_rect(d.bounds, list.bounds)) && "compact shape popup can still open");
    assert(d.cell == 44 && list.bounds.left == 440);

    // Glyphs scale by DPI, not by the padded button, and only shrink to fit tight cells.
    const struct { int w, h, dpi, icon, badge; } sizes[] = {
        {36, 36, 96, 20, 10}, {40, 40, 96, 20, 10}, {64, 64, 144, 30, 15},
        {88, 88, 192, 40, 20}, {22, 24, 96, 13, 6}, {2, 2, 96, 2, 0}
    };
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        int w = sizes[i].w, h = sizes[i].h, dpi = sizes[i].dpi;
        Rect button = {0, 0, w, h};
        GlyphLayout plain = editor_glyph_layout(button, dpi, 0);
        GlyphLayout menu = editor_glyph_layout(button, dpi, 1);
        assert(plain.icon.right - plain.icon.left == sizes[i].icon && rect_empty(plain.badge));
        assert(menu.icon.right - menu.icon.left == sizes[i].icon);
        assert(menu.icon.top == plain.icon.top && menu.icon.bottom == plain.icon.bottom);
        assert(menu.icon.top >= 0 && menu.icon.bottom <= h);
        assert(plain.icon.left == (w - sizes[i].icon) / 2 && menu.icon.left >= 0 && menu.icon.right <= w);
        if (sizes[i].badge) {
            assert(menu.badge.right - menu.badge.left == sizes[i].badge);
            assert(menu.icon.right <= menu.badge.left && menu.badge.right <= w);
            assert(menu.badge.top == (h - sizes[i].badge) / 2);
        } else assert(rect_empty(menu.badge));
    }
    GlyphLayout offset = editor_glyph_layout((Rect){10, 20, 50, 60}, 96, 1);
    assert(offset.icon.left == 14 && offset.icon.top == 30);
    assert(offset.badge.left == 36 && offset.badge.top == 35);
}

static void selection_edges_test(void) {
    Rect area = drag_rect(16, 0, -1, -12), edges[4];
    selection_edges(area, (Rect){0, -12, 16, 0}, edges);
    // The seam is not an edge. Only the true, one-pixel perimeter is painted.
    assert(edges[0].left == 0 && edges[0].right == 16 && edges[0].top == -12 && edges[0].bottom == -11);
    assert(edges[1].top == -1 && edges[1].bottom == 0);
    assert(rect_empty(edges[2]));
    assert(edges[3].left == 15 && edges[3].right == 16 && edges[3].bottom == -1);
    selection_edges(area, (Rect){-2, -12, 0, 0}, edges);
    assert(edges[2].left == -1 && edges[2].right == 0 && edges[2].top == -11);
    selection_edges(area, (Rect){7, -9, 8, -3}, edges);
    for (int i = 0; i < 4; ++i) assert(rect_empty(edges[i]));
    selection_edges(drag_rect(2, 2, 1, 1), (Rect){0, 0, 3, 3}, edges);
    assert(!rect_empty(edges[0]) && edges[0].left == 1 && edges[0].bottom == 2);
    selection_edges(drag_rect(1, 2, 1, 1), (Rect){0, 0, 3, 3}, edges);
    for (int i = 0; i < 4; ++i) assert(rect_empty(edges[i]));
}

static void selection_adjust_test(void) {
    Rect desktop = {-150, -90, 250, 180}, start = {-12, -9, 44, 35};
    Rect r = selection_adjust(start, desktop, SEL_MOVE, 300, -100);
    assert(r.left == 194 && r.top == -90 && r.right == 250 && r.bottom == -46);
    r = selection_adjust(start, desktop, SEL_W | SEL_N, 80, 60);
    assert(r.left == 44 && r.top == 35 && r.right == 68 && r.bottom == 51);
    r = selection_adjust(start, desktop, SEL_E, -56, 50);
    assert(r.left == -12 && r.right == -11 && r.top == -9 && r.bottom == 35);
    r = selection_adjust(start, desktop, SEL_S, 0, INT64_MAX);
    assert(r.bottom == desktop.bottom && r.top == start.top);
}

static void selection_hit_test(void) {
    Rect area = {-20, 10, 180, 130}, desktop = {-80, -20, 245, 200};
    EditPoint points[SELECTION_GRIPS];
    selection_grips(area, desktop, points);
    const int directions[] = {SEL_N|SEL_W, SEL_N, SEL_N|SEL_E, SEL_E,
                              SEL_S|SEL_E, SEL_S, SEL_S|SEL_W, SEL_W};
    for (int i = 0; i < SELECTION_GRIPS; ++i)
        assert(selection_hit(area, desktop, points[i], 0) == directions[i]);
    assert(selection_hit(area, desktop, (EditPoint){37, 129}, 0) == SEL_S);
    assert(selection_hit(area, desktop, (EditPoint){60, 50}, 0) == 0);
    assert(selection_hit(area, desktop, (EditPoint){60, 50}, 1) == SEL_MOVE);
    assert(selection_hit(area, desktop, (EditPoint){-20, 10}, 1) == SEL_MOVE);
    area = (Rect){desktop.left, desktop.top, desktop.left + 2, desktop.top + 2};
    selection_grips(area, desktop, points);
    for (int i = 0; i < SELECTION_GRIPS; ++i) {
        assert(points[i].x >= desktop.left && points[i].y >= desktop.top);
        assert(selection_hit(area, desktop, points[i], 0) == directions[i]);
    }
}

static void narrow_selection_hit_test(void) {
    const Rect desk = {0, 0, 200, 200};
    const struct { Rect area; EditPoint inside, edge; int direction; } cases[] = {
        {{20, 20, 25, 120}, {22, 52}, {19, 52}, SEL_W},
        {{20, 20, 120, 25}, {52, 22}, {52, 19}, SEL_N},
        {{0, 20, 5, 120}, {2, 69}, {5, 53}, SEL_E},
        {{20, 195, 120, 200}, {69, 197}, {53, 194}, SEL_N},
        {{0, 0, 1, 1}, {0, 0}, {1, 1}, SEL_N | SEL_W},
        {{198, 198, 200, 200}, {199, 199}, {198, 197}, SEL_S | SEL_E},
    };
    const int directions[] = {SEL_N|SEL_W, SEL_N, SEL_N|SEL_E, SEL_E,
                              SEL_S|SEL_E, SEL_S, SEL_S|SEL_W, SEL_W};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        Rect area = cases[i].area;
        assert(selection_hit(area, desk, cases[i].inside, 0) == 0 &&
               "a narrow crop must leave interior pixels for ordinary marking");
        assert(selection_hit(area, desk, cases[i].inside, 1) == SEL_MOVE);
        assert(selection_hit(area, desk, cases[i].edge, 0) == cases[i].direction);
        EditPoint points[SELECTION_GRIPS];
        selection_grips(area, desk, points);
        for (int j = 0; j < SELECTION_GRIPS; ++j) {
            int accessible = 0;
            for (int y = points[j].y - GRIP_RADIUS; y <= points[j].y + GRIP_RADIUS; ++y)
                for (int x = points[j].x - GRIP_RADIUS; x <= points[j].x + GRIP_RADIUS; ++x)
                    if (x >= desk.left && x < desk.right && y >= desk.top && y < desk.bottom &&
                        selection_hit(area, desk, (EditPoint){x, y}, 0) == directions[j]) accessible = 1;
            assert(accessible && "each grip retains a hit on visible desktop pixels");
        }
    }
}

static void selection_contrast_test(void) {
    for (int side = 2; side <= 4; ++side) {
        assert(selection_dash_white(side, 0, 0) && !selection_dash_white(side, 1, 0));
        assert(!selection_dash_white(side, 0, 1));
    }
    assert(selection_dash_white(50, 0, 0) && selection_dash_white(50, 3, 0));
    assert(!selection_dash_white(50, 4, 0) && !selection_dash_white(50, 0, 1));
}

static void hotkey_test(void) {
    assert(decode_hotkey(0) == 0x0653 && decode_hotkey(1) == 0x0353 && decode_hotkey(2) == 0x0177);
    assert(decode_hotkey(0x10941) == 0x0941);
    assert(decode_hotkey(0x10041) == 0x0653 && decode_hotkey(0xffffffff) == 0x0653);
    assert(!valid_hotkey(0x001b) && !valid_hotkey(0x0210) && !valid_hotkey(0x027b));
    assert(valid_hotkey(0x0253) && valid_hotkey(0x0377) && valid_hotkey(0x0c39));
}

int main(void) {
    history_test();
    layout_test();
    hotkey_test();
    selection_edges_test();
    selection_adjust_test();
    selection_hit_test();
    selection_contrast_test();
    narrow_selection_hit_test();
    puts("8 editor logic tests passed");
}

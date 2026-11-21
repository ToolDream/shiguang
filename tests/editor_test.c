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
    Dock d = editor_dock(area, screens[0], 40, 8, 9);
    assert(d.bounds.left == -400 && d.bounds.right == -40 && d.bounds.top == 308);
    Dock list = editor_popup(d, screens[0], 8, 5);
    assert(list.bounds.left == -400 && list.bounds.top == 356 && list.cols == 5);
    d = editor_dock((Rect){-520, 420, -40, 600}, screens[0], 40, 8, 9);
    assert(d.bounds.top == 372 && d.bounds.bottom == 412);
    d = editor_dock((Rect){5, 0, 1024, 768}, screens[1], 40, 8, 9);
    assert(d.bounds.top == 720 && d.bounds.bottom == 760);
    list = editor_popup(d, screens[1], 8, 6);
    assert(list.bounds.bottom == 712 && list.bounds.top == 672);
    d = editor_dock((Rect){-90, -30, -5, 8}, (Rect){-100, -40, 0, 152}, 40, 8, 9);
    assert(d.cols == 2 && d.cell == 38 && d.bounds.top == -40);
    assert(d.bounds.right <= 0 && d.bounds.bottom <= 152);
    assert((9 + d.cols - 1) / d.cols * d.cell == d.bounds.bottom - d.bounds.top);
    assert(editor_screen((Rect){-700, -110, 200, -40}, screens, 2) == 0);
    assert(editor_screen((Rect){-700, -110, 200, 620}, screens, 2) == 1);
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
    puts("3 editor logic tests passed");
}

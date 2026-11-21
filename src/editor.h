#ifndef SHIGUANG_EDITOR_H
#define SHIGUANG_EDITOR_H
#include "crop.h"
#include <wchar.h>

typedef enum { RECTANGLE, CIRCLE, ELLIPSE, TRIANGLE, DIAMOND, ARROW, PEN, TEXT } Tool;
typedef struct { int x, y; } EditPoint;
typedef struct {
    Tool tool;
    uint32_t color;
    int width, x, y, end_x, end_y;
    EditPoint *points;
    size_t count, capacity;
    wchar_t *text;
} Mark;
typedef struct { Mark *marks; size_t used, total, capacity; } History;

int mark_point(Mark *mark, int x, int y);
void mark_clear(Mark *mark);
int history_add(History *history, Mark *mark); // Takes ownership on success.
int history_undo(History *history);
int history_redo(History *history);
void history_clear(History *history);

typedef struct { Rect bounds; int cols, cell; } Dock;
size_t editor_screen(Rect area, const Rect *screens, size_t count);
Dock editor_dock(Rect area, Rect screen, int cell, int gap, int buttons);
Dock editor_popup(Dock bar, Rect screen, int gap, int buttons);

// The low byte is the Win32 virtual key; the next nibble is MOD_*.
#define HOTKEY_DEFAULT 0x0653u
#define HOTKEY_TAG 0x10000u
int valid_hotkey(uint32_t chord);
uint32_t decode_hotkey(uint32_t saved);
#endif

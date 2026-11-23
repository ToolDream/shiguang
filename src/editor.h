#ifndef SHIGUANG_EDITOR_H
#define SHIGUANG_EDITOR_H
#include "crop.h"
#include <wchar.h>

typedef enum { RECTANGLE, CIRCLE, ELLIPSE, TRIANGLE, DIAMOND, ARROW, PEN, TEXT } Tool;
enum { BAR_BUTTONS = 9, SHAPES = 5, COLORS = 6 };
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
Dock editor_dock(Rect area, Rect screen, int dpi, int gap, int buttons);
Dock editor_popup(Dock bar, Rect screen, int gap, int buttons);
typedef struct { Rect icon, badge; } GlyphLayout;
GlyphLayout editor_glyph_layout(Rect button, int dpi, int menu);
// Top, bottom, left, right stripes of the real selection, clipped to one surface.
void selection_edges(Rect area, Rect clip, Rect edges[4]);
enum { SEL_N = 1, SEL_E = 2, SEL_S = 4, SEL_W = 8, SEL_MOVE = 16 };
enum { SELECTION_GRIPS = 8, GRIP_RADIUS = 4 };
void selection_grips(Rect area, Rect desktop, EditPoint points[SELECTION_GRIPS]);
int selection_hit(Rect area, Rect desktop, EditPoint at, int alt);
Rect selection_adjust(Rect start, Rect desktop, int mode, int64_t dx, int64_t dy);
int selection_dash_white(int64_t length, int along, int across);

// The low byte is the Win32 virtual key; the next nibble is MOD_*.
#define HOTKEY_DEFAULT 0x0653u
#define HOTKEY_TAG 0x10000u
int valid_hotkey(uint32_t chord);
uint32_t decode_hotkey(uint32_t saved);
#endif

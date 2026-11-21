#include "editor.h"
#include <stdlib.h>

int mark_point(Mark *mark, int x, int y) {
    if (mark->count && mark->points[mark->count - 1].x == x && mark->points[mark->count - 1].y == y)
        return 1;
    if (mark->count == mark->capacity) {
        if (mark->capacity > SIZE_MAX / sizeof(EditPoint) / 2) return 0;
        size_t next = mark->capacity ? mark->capacity * 2 : 32;
        EditPoint *p = realloc(mark->points, next * sizeof *p);
        if (!p) return 0;
        mark->points = p;
        mark->capacity = next;
    }
    mark->points[mark->count++] = (EditPoint){x, y};
    return 1;
}

void mark_clear(Mark *mark) {
    free(mark->points);
    free(mark->text);
    *mark = (Mark){0};
}

int history_add(History *h, Mark *mark) {
    if (h->used == h->capacity) {
        if (h->capacity > SIZE_MAX / sizeof(Mark) / 2) return 0;
        size_t next = h->capacity ? h->capacity * 2 : 16;
        Mark *p = realloc(h->marks, next * sizeof *p);
        if (!p) return 0;
        h->marks = p;
        h->capacity = next;
    }
    for (size_t i = h->used; i < h->total; ++i) mark_clear(&h->marks[i]);
    h->marks[h->used++] = *mark;
    h->total = h->used;
    *mark = (Mark){0};
    return 1;
}

int history_undo(History *h) {
    if (!h->used) return 0;
    --h->used;
    return 1;
}

int history_redo(History *h) {
    if (h->used == h->total) return 0;
    ++h->used;
    return 1;
}

void history_clear(History *h) {
    for (size_t i = 0; i < h->total; ++i) mark_clear(&h->marks[i]);
    free(h->marks);
    *h = (History){0};
}

static int min(int a, int b) { return a < b ? a : b; }
static int max(int a, int b) { return a > b ? a : b; }
static int clamp(int val, int low, int high) { return max(low, min(val, high)); }

size_t editor_screen(Rect area, const Rect *screens, size_t count) {
    size_t best = count;
    int64_t distance = INT64_MAX;
    for (size_t i = 0; i < count; ++i) {
        Rect part = intersect_rect(area, screens[i]);
        if (rect_empty(part)) continue;
        int64_t gap = (int64_t)area.right - part.right + (int64_t)area.bottom - part.bottom;
        if (gap < distance) {
            distance = gap;
            best = i;
        }
    }
    return best;
}

static Dock dock_grid(Rect screen, int preferred, int buttons) {
    int w = screen.right - screen.left, h = screen.bottom - screen.top;
    for (int cell = min(preferred, min(w, h)); cell > 0; --cell) {
        int cols = min(buttons, w / cell);
        int rows = (buttons + cols - 1) / cols;
        if (rows <= h / cell) return (Dock){{0, 0, cell * cols, cell * rows}, cols, cell};
    }
    return (Dock){0};
}

static Dock place(Dock d, Rect screen, int x, int y) {
    int w = d.bounds.right, h = d.bounds.bottom;
    x = clamp(x, screen.left, screen.right - w);
    y = clamp(y, screen.top, screen.bottom - h);
    d.bounds = (Rect){x, y, x + w, y + h};
    return d;
}

Dock editor_dock(Rect area, Rect screen, int cell, int gap, int buttons) {
    Dock d = dock_grid(screen, cell, buttons);
    int h = d.bounds.bottom;
    int y = area.bottom + gap;
    if (y > screen.bottom - h) y = area.top - gap - h;
    if (y < screen.top) y = area.bottom - gap - h;
    return place(d, screen, area.right - d.bounds.right, y);
}

Dock editor_popup(Dock bar, Rect screen, int gap, int buttons) {
    Dock d = dock_grid(screen, bar.cell, buttons);
    Rect b = bar.bounds;
    int h = d.bounds.bottom, w = d.bounds.right;
    int y = b.bottom + gap;
    if (y <= screen.bottom - h) return place(d, screen, b.left, y);
    y = b.top - gap - h;
    if (y >= screen.top) return place(d, screen, b.left, y);
    if (b.right + gap <= screen.right - w) return place(d, screen, b.right + gap, b.top);
    if (b.left - gap - w >= screen.left) return place(d, screen, b.left - gap - w, b.top);
    // In an exceptionally small viewport, this transient panel may overlap the bar.
    return place(d, screen, b.left, b.top - h);
}

int valid_hotkey(uint32_t chord) {
    if (chord > 0xfff || !(chord & 0xb00)) return 0;  // Ctrl, Alt or Win required.
    unsigned key = chord & 255;
    return (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') ||
        (key >= 0x70 && key <= 0x87 && key != 0x7b) || key == 0x20 ||
        (key >= 0x21 && key <= 0x28) || key == 0x2d || key == 0x2e;
}

uint32_t decode_hotkey(uint32_t saved) {
    static const uint32_t legacy[] = {HOTKEY_DEFAULT, 0x0353, 0x0177};
    if (saved < sizeof legacy / sizeof legacy[0]) return legacy[saved];
    if ((saved & HOTKEY_TAG) && valid_hotkey(saved & ~HOTKEY_TAG)) return saved & ~HOTKEY_TAG;
    return HOTKEY_DEFAULT;
}

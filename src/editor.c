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

static int scale96(int px, int dpi) { return (px * dpi + 48) / 96; }

static Dock dock_bar(Rect area, Rect screen, int cell, int gap, int buttons) {
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

static int popup_overlaps_bar(Dock bar, Rect screen, int gap, int buttons) {
    Dock popup = editor_popup(bar, screen, gap, buttons);
    return !rect_empty(intersect_rect(bar.bounds, popup.bounds));
}

Dock editor_dock(Rect area, Rect screen, int dpi, int gap, int buttons) {
    int cell = scale96(48, dpi);
    Dock compact = dock_bar(area, screen, scale96(44, dpi), gap, buttons);
    if (screen.right - screen.left < buttons * cell || screen.bottom - screen.top < 2 * cell + gap)
        return compact;

    Dock full = dock_bar(area, screen, cell, gap, buttons);
    // An anchor can leave neither vertical nor side room even when the screen fits two rows.
    const int counts[] = {SHAPES, COLORS};
    for (size_t i = 0; i < sizeof counts / sizeof *counts; ++i)
        if (popup_overlaps_bar(full, screen, gap, counts[i]) &&
            !popup_overlaps_bar(compact, screen, gap, counts[i])) return compact;
    return full;
}

GlyphLayout editor_glyph_layout(Rect button, int dpi, int menu) {
    int w = button.right - button.left, h = button.bottom - button.top;
    int extent = min(w, h);
    if (extent <= 0) return (GlyphLayout){0};
    int room = extent - 2 * (extent / 16);
    int badge = min(scale96(6, dpi), room / 4);
    int max_side = room - 2 * badge;
    if (max_side <= 0) {
        badge = 0;
        max_side = room;
    }
    // Keep main icon centered and equal-sized across plain and menu buttons.
    int side = min(scale96(20, dpi), max_side);
    int x = button.left + (w - side) / 2;
    int y = button.top + (h - side) / 2;
    GlyphLayout layout = {.icon = {x, y, x + side, y + side}};
    if (menu && badge) {
        int gap = max(0, (button.right - (x + side) - badge) / 4);
        int bx = min(button.right - badge, x + side + gap);
        int by = min(button.bottom - badge, y + side - badge / 3);
        if (bx < x + side) bx = x + side;
        layout.badge = (Rect){bx, by, bx + badge, by + badge};
    }
    return layout;
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

void selection_edges(Rect area, Rect clip, Rect edges[4]) {
    if (rect_empty(area)) {
        for (int i = 0; i < 4; ++i) edges[i] = (Rect){0};
        return;
    }
    // The inset 1px outline survives at monitor edges; never double-paint corners.
    int top_end = area.top + 1;
    int bottom_start = max(top_end, area.bottom - 1);
    int left_end = area.left + 1;
    int right_start = max(left_end, area.right - 1);
    edges[0] = intersect_rect(clip, (Rect){area.left, area.top, area.right, top_end});
    edges[1] = intersect_rect(clip, (Rect){area.left, bottom_start, area.right, area.bottom});
    edges[2] = intersect_rect(clip, (Rect){area.left, top_end, left_end, bottom_start});
    edges[3] = intersect_rect(clip, (Rect){right_start, top_end, area.right, bottom_start});
}

static const int grip_modes[SELECTION_GRIPS] = {
    SEL_N | SEL_W, SEL_N, SEL_N | SEL_E, SEL_E, SEL_S | SEL_E, SEL_S, SEL_S | SEL_W, SEL_W
};

static void grip_axis(int lo, int hi, int desk_lo, int desk_hi, int out[3]) {
    int64_t mid = lo + ((int64_t)hi - 1 - lo) / 2;
    int64_t start = lo, end = hi - 1;
    const int gap = 2 * GRIP_RADIUS + 2;
    // Make the three hit targets distinct on short edges, including at the
    // desktop boundary. Do not change the ROI to make room for decorations.
    if (((int64_t)desk_hi - desk_lo) >= 2 * gap + 1 && (mid - start < gap || end - mid < gap)) {
        mid = clamp((int)mid, desk_lo + gap, desk_hi - 1 - gap);
        start = mid - gap;
        end = mid + gap;
    }
    out[0] = (int)start;
    out[1] = (int)mid;
    out[2] = (int)end;
}

void selection_grips(Rect area, Rect desk, EditPoint points[SELECTION_GRIPS]) {
    int x[3], y[3];
    grip_axis(area.left, area.right, desk.left, desk.right, x);
    grip_axis(area.top, area.bottom, desk.top, desk.bottom, y);
    for (int i = 0; i < SELECTION_GRIPS; ++i) {
        int mode = grip_modes[i];
        points[i] = (EditPoint){x[mode & SEL_W ? 0 : mode & SEL_E ? 2 : 1],
                                y[mode & SEL_N ? 0 : mode & SEL_S ? 2 : 1]};
    }
}

int selection_hit(Rect area, Rect desk, EditPoint at, int alt) {
    if (rect_empty(area)) return 0;
    int in_x = at.x >= area.left && at.x < area.right;
    int in_y = at.y >= area.top && at.y < area.bottom;
    const int slop = 2;
    int64_t width = (int64_t)area.right - area.left, height = (int64_t)area.bottom - area.top;
    int narrow_x = width <= 2 * (slop + 1), narrow_y = height <= 2 * (slop + 1);
    if (in_x && in_y) {
        if (alt) return SEL_MOVE;
        // When the edge bands consume an axis, reserve its center pixels
        // even if a grip is pushed inward at the desktop boundary.
        if ((narrow_x || narrow_y) &&
            (!narrow_x || at.x == area.left + width / 2) &&
            (!narrow_y || at.y == area.top + height / 2)) return 0;
    }
    EditPoint grips[SELECTION_GRIPS];
    selection_grips(area, desk, grips);
    for (int i = 0; i < SELECTION_GRIPS; ++i)
        if (llabs((int64_t)at.x - grips[i].x) <= GRIP_RADIUS &&
            llabs((int64_t)at.y - grips[i].y) <= GRIP_RADIUS) return grip_modes[i];
    int64_t left = llabs((int64_t)at.x - area.left);
    int64_t right = llabs((int64_t)at.x - area.right + 1);
    int64_t top = llabs((int64_t)at.y - area.top);
    int64_t bottom = llabs((int64_t)at.y - area.bottom + 1);
    int x = narrow_x && in_x ? 0 : left <= right && left <= slop ? SEL_W : right <= slop ? SEL_E : 0;
    int y = narrow_y && in_y ? 0 : top <= bottom && top <= slop ? SEL_N : bottom <= slop ? SEL_S : 0;
    if (x && y) return x | y;
    return in_y && x ? x : in_x && y ? y : 0;
}

static int bounded_offset(int start, int64_t delta, int lo, int hi) {
    if (delta < (int64_t)lo - start) return lo;
    if (delta > (int64_t)hi - start) return hi;
    return (int)(start + delta);
}

static void adjust_axis(int *lo, int *hi, int low, int high, int lower, int64_t delta) {
    int fixed = lower ? *hi : *lo;
    int end = bounded_offset(lower ? *lo : *hi, delta, low, high);
    if (end == fixed) end += lower ? -1 : 1;
    *lo = min(fixed, end);
    *hi = max(fixed, end);
}

Rect selection_adjust(Rect start, Rect desk, int mode, int64_t dx, int64_t dy) {
    if (mode == SEL_MOVE) {
        int64_t w = (int64_t)start.right - start.left, h = (int64_t)start.bottom - start.top;
        int x = bounded_offset(start.left, dx, desk.left, (int)(desk.right - w));
        int y = bounded_offset(start.top, dy, desk.top, (int)(desk.bottom - h));
        return (Rect){x, y, (int)(x + w), (int)(y + h)};
    }
    if (mode & (SEL_W | SEL_E))
        adjust_axis(&start.left, &start.right, desk.left, desk.right, mode & SEL_W, dx);
    if (mode & (SEL_N | SEL_S))
        adjust_axis(&start.top, &start.bottom, desk.top, desk.bottom, mode & SEL_N, dy);
    return start;
}

int selection_dash_white(int64_t length, int along, int across) {
    return ((along / (length < 8 ? 1 : 4) + across) & 1) == 0;
}

#ifndef SHIGUANG_EDITOR_WIN_H
#define SHIGUANG_EDITOR_WIN_H
#include <windows.h>
#include "editor.h"

#include "ui_palette.h"

extern UiPalette ui_colors;
UiPalette ui_palette(BOOL dark, BOOL high_contrast);
BOOL refresh_ui_palette(void);

typedef struct {
    HDC dc;
    HBITMAP dib;
    HGDIOBJ previous;
    Image image;
} Canvas;
int canvas_open(Canvas *c, const Image *base);
void canvas_close(Canvas *c);
int canvas_render(Canvas *c, const Image *base, const History *h, const Mark *draft);
HFONT mark_font(int height);

enum Action {
    ACT_SHAPES, ACT_ARROW, ACT_PEN, ACT_TEXT, ACT_COLOR, ACT_UNDO, ACT_REDO, ACT_SAVE, ACT_COPY,
    ACT_RECTANGLE = 20, ACT_RED = 30
};
enum { BAR_BUTTONS = 9, SHAPES = 5, COLORS = 6 };
extern const COLORREF palette[COLORS];

typedef struct {
    HWND window;
    HWND buttons[BAR_BUTTONS];
    Tool tool, shape;
    int color;
    void (*click)(int);
} Panel;
BOOL register_panel(HINSTANCE instance);
BOOL open_panel(Panel *p, HINSTANCE instance, HWND owner, Dock d, int first, int count, void (*click)(int));
void close_panel(Panel *p);
void update_panel(Panel *p, Tool tool, Tool shape, int color, BOOL undo, BOOL redo);
HICON shiguang_icon(int size);
#endif

#ifndef SHIGUANG_CROP_H
#define SHIGUANG_CROP_H
#include <stddef.h>
#include <stdint.h>

// Half-open desktop coordinates; pixels are tightly packed, top-down BGRA.
typedef struct { int left, top, right, bottom; } Rect;
typedef struct { Rect bounds; const unsigned char *bgra; } Frame;
typedef struct { int width, height; unsigned char *bgra; } Image;
typedef enum { CROP_EMPTY, CROP_OK, CROP_ERROR } CropResult;

Rect drag_rect(int x1, int y1, int x2, int y2);
Rect intersect_rect(Rect a, Rect b);
int rect_empty(Rect r);
size_t image_bytes(int64_t width, int64_t height);
CropResult crop_frozen(Rect area, const Frame *frames, size_t count, Image *out);
#endif

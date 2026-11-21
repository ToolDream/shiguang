#include "crop.h"
#include <stdlib.h>
#include <string.h>

static int smaller(int a, int b) { return a < b ? a : b; }
static int larger(int a, int b) { return a > b ? a : b; }

Rect drag_rect(int x1, int y1, int x2, int y2) {
    return (Rect){smaller(x1, x2), smaller(y1, y2), larger(x1, x2), larger(y1, y2)};
}

Rect intersect_rect(Rect a, Rect b) {
    return (Rect){larger(a.left, b.left), larger(a.top, b.top),
                  smaller(a.right, b.right), smaller(a.bottom, b.bottom)};
}

int rect_empty(Rect r) { return r.right <= r.left || r.bottom <= r.top; }

size_t image_bytes(int64_t width, int64_t height) {
    // Bound transient allocations on unusually large or very sparse desktops.
    const int64_t pixel_limit = 512LL * 1024 * 1024 / 4;
    if (width <= 0 || height <= 0 || width > pixel_limit / height) return 0;
    return (size_t)(width * height * 4);
}

CropResult crop_frozen(Rect area, const Frame *frames, size_t count, Image *out) {
    if (rect_empty(area)) return CROP_EMPTY;
    int64_t width = (int64_t)area.right - area.left;
    int64_t height = (int64_t)area.bottom - area.top;
    size_t bytes = image_bytes(width, height);
    if (!bytes) return CROP_ERROR;

    unsigned char *pixels = NULL;
    for (size_t i = 0; i < count; ++i) {
        Rect b = frames[i].bounds, overlap = intersect_rect(area, b);
        if (rect_empty(overlap)) continue;
        size_t src_stride = (size_t)((int64_t)b.right - b.left) * 4;
        size_t dst_stride = (size_t)width * 4;
        size_t span = (size_t)(overlap.right - overlap.left) * 4;
        if (!pixels && !(pixels = calloc(1, bytes))) return CROP_ERROR;
        for (int y = overlap.top; y < overlap.bottom; ++y) {
            const unsigned char *src = frames[i].bgra + (size_t)((int64_t)y - b.top) * src_stride
                + (size_t)((int64_t)overlap.left - b.left) * 4;
            unsigned char *dst = pixels + (size_t)(y - area.top) * dst_stride
                + (size_t)(overlap.left - area.left) * 4;
            memcpy(dst, src, span);
            for (size_t x = 3; x < span; x += 4) dst[x] = 255;
        }
    }
    if (!pixels) return CROP_EMPTY;
    *out = (Image){(int)width, (int)height, pixels};
    return CROP_OK;
}

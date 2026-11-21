#include "../src/crop.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void reversed_physical_drag(void) {
    Rect r = drag_rect(40000, 280, -40000, -320);
    assert(r.left == -40000 && r.top == -320 && r.right == 40000 && r.bottom == 280);
}

static void joins_frozen_frames_without_filling_gaps(void) {
    // Two monitors at different heights, with deliberately non-opaque GDI input.
    const unsigned char left[] = {
        1, 2, 3, 0,   4, 5, 6, 7,
        8, 9, 10, 0,  11, 12, 13, 0
    };
    const unsigned char right[] = {
        21, 22, 23, 0,  24, 25, 26, 0,
        31, 32, 33, 0,  34, 35, 36, 0
    };
    Frame frames[] = {{{-2, -1, 0, 1}, left}, {{0, 0, 2, 2}, right}};
    static const unsigned char expected[] = {
        4, 5, 6, 255,    0, 0, 0, 0,       0, 0, 0, 0,
        11, 12, 13, 255, 21, 22, 23, 255,  24, 25, 26, 255,
        0, 0, 0, 0,      31, 32, 33, 255,  34, 35, 36, 255
    };
    Image image = {0};
    assert(crop_frozen(drag_rect(2, 2, -1, -1), frames, 2, &image) == CROP_OK);
    assert(image.width == 3 && image.height == 3);
    assert(memcmp(image.bgra, expected, sizeof expected) == 0);
    assert(left[7] == 7);
    free(image.bgra);
}

static void rejects_empty_and_unreasonable_crops(void) {
    const unsigned char pixel[] = {1, 2, 3, 4};
    Frame frame = {{0, 0, 1, 1}, pixel};
    Image image = {0};
    assert(crop_frozen(drag_rect(1, 0, 1, 1), &frame, 1, &image) == CROP_EMPTY);
    assert(crop_frozen(drag_rect(-3, -3, 0, 0), &frame, 1, &image) == CROP_EMPTY);
    assert(crop_frozen(drag_rect(-1000000, -1000000, 1000000, 1000000), &frame, 1, &image) == CROP_ERROR);
    assert(image.bgra == NULL);
}

int main(void) {
    reversed_physical_drag();
    joins_frozen_frames_without_filling_gaps();
    rejects_empty_and_unreasonable_crops();
    puts("3 crop tests passed");
}

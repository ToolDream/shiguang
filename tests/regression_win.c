#define COBJMACROS
#define UNICODE
#include "../src/desktop_win.h"
#include "../src/crop.h"
#include "../src/editor_win.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static BOOL fail_blit, fail_dib, fail_preview;
static int copies, blits;
static HWND observe_preview;
static BOOL force_present_fallback;
static int present_blits, present_dibs, window_shades;
static COLORREF presented_hint, presented_old_hint, presented_inside, presented_outside;
static POINT hint_sample, old_hint_sample, inside_sample, outside_sample;
static RECT hint_label;
static BOOL presented_text;
static HRESULT save_result;
static BOOL copy_succeeds = TRUE, persist_succeeds = TRUE;
static DWORD persisted;
static const Image *expected_copy;
static HWND close_during_blit, invalidate_in_picker, invalidate_on_flush;
static const WCHAR *picked_path;
static unsigned stage_flushes, moves;

static BOOL test_blit(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD op) {
    ++blits;
    if (observe_preview && WindowFromDC(dst) == observe_preview) {
        ++present_blits;
        assert(sx == x && sy == y);
        presented_hint = GetPixel(src, hint_sample.x, hint_sample.y);
        presented_inside = GetPixel(src, inside_sample.x, inside_sample.y);
        presented_outside = GetPixel(src, outside_sample.x, outside_sample.y);
        presented_text = FALSE;
        for (LONG row = hint_label.top; row < hint_label.bottom && !presented_text; ++row)
            for (LONG col = hint_label.left; col < hint_label.right && !presented_text; ++col)
                presented_text = GetPixel(src, col, row) == ui_colors.hint_ink;
        if (force_present_fallback) {
            presented_old_hint = GetPixel(src, old_hint_sample.x, old_hint_sample.y);
            return FALSE;
        }
    }
    if (close_during_blit) {
        HWND w = close_during_blit;
        close_during_blit = NULL;
        SendMessageW(w, WM_CLOSE, 0, 0);
        assert(IsWindow(w));  // Reentrant cancellation cannot free a live paint target.
    }
    return !fail_blit && !(fail_preview && WindowFromDC(dst)) &&
        BitBlt(dst, x, y, w, h, src, sx, sy, op);
}

static int test_dib(HDC dst, int x, int y, int w, int h, int sx, int sy, int sw, int sh,
                    const void *bits, const BITMAPINFO *info, UINT usage, DWORD op) {
    if (observe_preview && WindowFromDC(dst) == observe_preview) {
        ++present_dibs;
        assert(sw == w && sh == h && sx == x && sy == y);
        const uint32_t *pixels = bits;
        uint32_t pixel = pixels[hint_sample.y * info->bmiHeader.biWidth + hint_sample.x];
        assert(RGB((pixel >> 16) & 255, (pixel >> 8) & 255, pixel & 255) == ui_colors.hint);
    }
    return fail_dib || (fail_preview && WindowFromDC(dst)) ? GDI_ERROR :
        StretchDIBits(dst, x, y, w, h, sx, sy, sw, sh, bits, info, usage, op);
}

static BOOL test_alpha_blend(HDC dst, int x, int y, int w, int h, HDC src,
                             int sx, int sy, int sw, int sh, BLENDFUNCTION blend) {
    if (observe_preview && WindowFromDC(dst) == observe_preview) ++window_shades;
    return AlphaBlend(dst, x, y, w, h, src, sx, sy, sw, sh, blend);
}

static BOOL create_at_publish;
static const char marker[] = "existing file";

static void create_target(const WCHAR *path) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    DWORD count;
    assert(WriteFile(file, marker, sizeof marker, &count, NULL) && count == sizeof marker);
    assert(CloseHandle(file));
}

static BOOL test_move(const WCHAR *from, const WCHAR *to, DWORD flags) {
    ++moves;
    if (create_at_publish) create_target(to);
    return MoveFileExW(from, to, flags);
}

static BOOL test_flush_file(HANDLE file) {
    if (!FlushFileBuffers(file)) return FALSE;
    ++stage_flushes;
    if (invalidate_on_flush) {
        HWND w = invalidate_on_flush;
        invalidate_on_flush = NULL;
        SendMessageW(w, WM_DPICHANGED, 0, 0);
        assert(IsWindow(w));  // The owner must not be released while saving.
    }
    return TRUE;
}

// Include the production units to exercise their private paths without shipping test hooks.
#define FlushFileBuffers test_flush_file
#define MoveFileExW test_move
#include "../src/export_win.c"
#undef MoveFileExW
#undef FlushFileBuffers
static BOOL count_copy(HWND w, const Image *image) {
    (void)w;
    ++copies;
    if (expected_copy) {
        assert(image->width == expected_copy->width && image->height == expected_copy->height);
        assert(!memcmp(image->bgra, expected_copy->bgra, image_bytes(image->width, image->height)));
    }
    return copy_succeeds;  // Never touch the user's clipboard.
}

static SaveAllowed editor_save_guard;
static unsigned guard_calls;
static BOOL last_guard_allowed;

static BOOL record_save_guard(void) {
    ++guard_calls;
    last_guard_allowed = editor_save_guard();
    return last_guard_allowed;
}

static HRESULT test_save_dialog(HWND w, const Image *image, SaveAllowed allowed) {
    if (!picked_path) return save_result;
    if (invalidate_in_picker) {
        SendMessageW(invalidate_in_picker, WM_DISPLAYCHANGE, 0, 0);
        invalidate_in_picker = NULL;
        assert(IsWindow(w));
    }
    if (!allowed) return finish_png_save(save_result, picked_path, image, NULL);
    editor_save_guard = allowed;
    guard_calls = 0;
    last_guard_allowed = TRUE;
    HRESULT result = finish_png_save(save_result, picked_path, image, record_save_guard);
    editor_save_guard = NULL;
    return result;
}

static LSTATUS test_persist(HKEY root, LPCWSTR path, LPCWSTR name, DWORD type,
                            const void *value, DWORD size) {
    assert(root == HKEY_CURRENT_USER && !wcscmp(path, L"Software\\Shiguang"));
    assert(!wcscmp(name, L"Hotkey") && type == REG_DWORD && size == sizeof persisted);
    if (!persist_succeeds) return ERROR_ACCESS_DENIED;
    memcpy(&persisted, value, size);
    return ERROR_SUCCESS;  // Never alter the user's configured shortcut.
}

static BOOL fail_adjust_canvas;
static HDC failed_canvas_dc;
static int test_canvas_open(Canvas *target, const Image *image) {
    if (!fail_adjust_canvas) return canvas_open(target, image);
    // Fail after acquiring a real resource to verify partial cleanup.
    failed_canvas_dc = target->dc = CreateCompatibleDC(NULL);
    assert(failed_canvas_dc);
    return 0;
}

static BOOL adjust_alt;
static SHORT WINAPI test_key_state(int vk) {
    return adjust_alt && vk == VK_MENU ? (SHORT)0x8000 : GetKeyState(vk);
}

static int glyph_pen_calls, glyph_blits, glyph_paths;
static BOOL fail_glyph_pen, fail_glyph_path;
static int WINAPI test_glyph_dib(HDC dst, int x, int y, DWORD w, DWORD h, int sx, int sy,
                                 UINT start, UINT lines, const void *bits, const BITMAPINFO *info, UINT usage) {
    ++glyph_blits;
    return SetDIBitsToDevice(dst, x, y, w, h, sx, sy, start, lines, bits, info, usage);
}

#define SetDIBitsToDevice test_glyph_dib
#define GetKeyState test_key_state
#define canvas_open test_canvas_open
#define AlphaBlend test_alpha_blend
#define BitBlt test_blit
#define StretchDIBits test_dib
#define publish_image count_copy
#define save_png_dialog test_save_dialog
#define RegSetKeyValueW test_persist
#include "../src/desktop_win.c"
#undef RegSetKeyValueW
#undef save_png_dialog
#undef publish_image
#undef StretchDIBits
#undef BitBlt
#undef AlphaBlend
#undef canvas_open
#undef GetKeyState

static HPEN WINAPI test_glyph_pen(DWORD style, DWORD width, const LOGBRUSH *brush,
                                 DWORD entries, const DWORD *dashes) {
    if (++glyph_pen_calls == 1 && fail_glyph_pen) return NULL;
    return ExtCreatePen(style, width, brush, entries, dashes);
}
static BOOL WINAPI test_glyph_path(HDC dc, const POINT *points, const BYTE *ops, int count) {
    if (++glyph_paths == 1 && fail_glyph_path) return FALSE;
    return PolyDraw(dc, points, ops, count);
}
#define PolyDraw test_glyph_path
#define ExtCreatePen test_glyph_pen
#include "../src/toolbar_glyph.h"
#undef ExtCreatePen
#undef PolyDraw
#undef SetDIBitsToDevice

static void save_test(void) {
    WCHAR folder[MAX_PATH], path[MAX_PATH];
    assert(GetTempPathW(ARRAYSIZE(folder), folder));
    assert(GetTempFileNameW(folder, L"sgt", 0, path));
    assert(DeleteFileW(path));

    unsigned char pixel[] = {0, 0, 255, 255};
    Image image = {1, 1, pixel};
    IStream *png = NULL;
    assert(encode_png(&image, &png) == S_OK);
    assert(write_png(path, png, NULL) == S_OK);

    assert(DeleteFileW(path));
    for (int late = 0; late < 2; ++late) {
        create_at_publish = late;
        if (!late) create_target(path);
        HRESULT conflict = write_png(path, png, NULL);
        char stored[sizeof marker + 1] = {0};
        DWORD count;
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        assert(file != INVALID_HANDLE_VALUE);
        assert(ReadFile(file, stored, sizeof stored, &count, NULL));
        assert(CloseHandle(file));
        assert(DeleteFileW(path));
        assert(FAILED(conflict) && count == sizeof marker && memcmp(stored, marker, sizeof marker) == 0);
    }
    IStream_Release(png);
}

static void hotkey_test(void) {
    MSG msg;
    while (PeekMessageW(&msg, owner, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {}
    phase = OUTPUT;
    assert(PostMessageW(owner, WM_HOTKEY, HOTKEY_A, 1));
    assert(PostMessageW(owner, WM_HOTKEY, HOTKEY_A, 2));
    assert(PostMessageW(owner, WM_APP + 99, 0, 0));
    idle();
    assert(phase == IDLE);
    // Enqueued without a sleep; wall-clock equality is irrelevant to this boundary.
    assert(PostMessageW(owner, WM_HOTKEY, HOTKEY_A, 3));
    assert(PeekMessageW(&msg, owner, WM_HOTKEY, WM_HOTKEY, PM_REMOVE));
    assert(msg.message == WM_HOTKEY && msg.lParam == 3);
    active_hotkey = HOTKEY_A;
    fail_blit = TRUE;  // Observe the attempt without activating visible overlays.
    DispatchMessageW(&msg);
    assert(blits > 0 && phase == IDLE);
    active_hotkey = 0;
    assert(!PeekMessageW(&msg, owner, WM_HOTKEY, WM_HOTKEY, PM_REMOVE));
    assert(PeekMessageW(&msg, owner, WM_APP + 99, WM_APP + 99, PM_REMOVE));

    phase = OUTPUT;
    for (int i = 0; i < 64; ++i) assert(PostMessageW(owner, WM_HOTKEY, HOTKEY_A, 0));
    idle();
    assert(phase == IDLE && !PeekMessageW(&msg, owner, WM_HOTKEY, WM_HOTKEY, PM_REMOVE));

    phase = OUTPUT;
    for (int i = 0; i < 70; ++i) assert(PostMessageW(owner, WM_HOTKEY, HOTKEY_A, 0));
    idle();
    assert(phase == DRAINING);
    assert(PeekMessageW(&msg, owner, WM_HOTKEY, WM_HOTKEY, PM_REMOVE));
    active_hotkey = HOTKEY_A;
    blits = 0;
    DispatchMessageW(&msg);
    assert(!blits);
    sg_after_dispatch();
    assert(phase == IDLE && !PeekMessageW(&msg, owner, WM_HOTKEY, WM_HOTKEY, PM_REMOVE));
    active_hotkey = 0;
}

static HWND small_overlay(void) {
    phase = PREPARING;
    layout_invalid = preview_failed = FALSE;
    selection = (Rect){0};
    assert(freeze_desktop());
    Rect b = frames[0].bounds;
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, CAPTURE_CLASS, L"", WS_POPUP,
                             b.left, b.top, 32, 32, NULL, NULL, instance, NULL);
    assert(w);
    surfaces[0].window = w;
    ShowWindow(w, SW_SHOWNOACTIVATE);
    assert(UpdateWindow(w) && !preview_failed && !layout_invalid);
    phase = SELECTING;
    dragging = TRUE;
    selection = (Rect){b.left + 1, b.top + 1, b.left + 8, b.top + 8};
    return w;
}

static void assert_shaded(uint32_t color, uint32_t original) {
    for (int shift = 0; shift < 24; shift += 8) {
        int expected = (int)((original >> shift) & 255u) * 158 / 255;
        int actual = (int)((color >> shift) & 255u);
        assert(abs(expected - actual) <= 1);
    }
}

// Exercise the real preview painter with deterministic frozen colors and
// monitor origins, without reading live desktop pixels.
static void selection_preview_test(void) {
    assert(open_shade());
    enum { W = 13, H = 14 };
    for (size_t i = 0; i < 2; ++i) {
        HDC source = CreateCompatibleDC(NULL), target = CreateCompatibleDC(NULL);
        BITMAPINFO info = bitmap_info(W, H);
        uint32_t *original, *pixels;
        HBITMAP src = CreateDIBSection(source, &info, DIB_RGB_COLORS, (void **)&original, NULL, 0);
        HBITMAP dst = CreateDIBSection(target, &info, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
        assert(src && dst && source && target);
        HGDIOBJ old_src = SelectObject(source, src), old_dst = SelectObject(target, dst);
        assert(old_src && old_dst);
        uint32_t frozen = i ? 0xfff4f4f4u : 0xff171717u;
        for (int j = 0; j < W * H; ++j) original[j] = frozen;
        Rect b = {i ? 0 : -W, -H, i ? W : 0, 0};
        frames[i] = (Frame){b, (unsigned char *)original};
        surfaces[i].dc = source;
        const RECT full = {0, 0, W, H};

        assert(!dragging && !base.bgra && draw_preview(target, &full, i) && GdiFlush());
        assert_shaded(pixels[0], frozen);
        // Reverse drag spans the seam and touches both monitor edges.
        selection = drag_rect(W, -2, -W, -H);
        dragging = TRUE;
        assert(draw_preview(target, &full, i) && GdiFlush());
        assert((pixels[0] & 0xffffffu) == (i ? 0u : 0xffffffu));
        assert((pixels[6] & 0xffffffu) == (i ? 0xffffffu : 0u));
        assert_shaded(pixels[W * (H - 1)], frozen);
        // A second row/column under the outline is unchanged, not another dash.
        assert((pixels[W + 6] & 0xffffffu) == (frozen & 0xffffffu));
        assert((pixels[5 * W + (i ? W - 2 : 1)] & 0xffffffu) == (frozen & 0xffffffu));
        assert((pixels[5 * W + (i ? 0 : W - 1)] & 0xffffffu) == (frozen & 0xffffffu));
        assert((pixels[5 * W + (i ? W - 1 : 0)] & 0xffffffu) == (i ? 0xffffffu : 0u));

        // Repaint only a dirty subrectangle as the active ROI moves. A pixel
        // leaving the selection is shaded once; a pixel entering it is restored.
        const RECT dirty = {1, 1, W - 1, H};
        selection = (Rect){b.left + 2, b.top + 3, b.left + 9, b.top + 9};
        assert(draw_preview(target, &dirty, i) && GdiFlush());
        assert_shaded(pixels[2 * W + 5], frozen);
        assert((pixels[5 * W + 5] & 0xffffffu) == (frozen & 0xffffffu));
        base = (Image){W, H, (unsigned char *)original};
        dragging = FALSE;
        adjust_mode = SEL_MOVE;
        adjust_area = (Rect){b.left + 2, b.top + 7, b.left + 9, b.bottom};
        assert(draw_preview(target, &dirty, i) && GdiFlush());
        assert_shaded(pixels[2 * W + 5], frozen);
        assert_shaded(pixels[5 * W + 5], frozen);
        assert((pixels[(H - 2) * W + 5] & 0xffffffu) == (frozen & 0xffffffu));
        adjust_mode = 0;
        base = (Image){0};
        dragging = TRUE;

        for (int side = 2; side <= 4; ++side) {
            selection = (Rect){b.left + 2, b.top + 2, b.left + 2 + side, b.top + 2 + side};
            assert(draw_preview(target, &full, i) && GdiFlush());
            assert((pixels[2 * W + 2] & 0xffffffu) == 0xffffffu);
            assert(!(pixels[2 * W + 3] & 0xffffffu));
        }
        for (int horizontal = 0; horizontal <= 1; ++horizontal) {
            selection = (Rect){b.left + 2, b.top + 2, b.left + 3 + horizontal, b.top + 4 - horizontal};
            assert(draw_preview(target, &full, i) && GdiFlush());
            assert((pixels[2 * W + 2] & 0xffffffu) == 0xffffffu);
            assert(!(pixels[2 * W + 2 + (horizontal ? 1 : W)] & 0xffffffu));
        }
        selection = (Rect){b.left + 2, b.top + 2, b.left + 3, b.top + 3};
        assert(draw_preview(target, &full, i) && GdiFlush());
        assert((pixels[2 * W + 2] & 0xffffffu) == (i ? 0u : 0xffffffu));
        for (int j = 0; j < W * H; ++j) assert(original[j] == frozen);
        dragging = FALSE;
        selection = (Rect){0};
        frames[i] = (Frame){0};
        surfaces[i].dc = NULL;
        SelectObject(target, old_dst);
        SelectObject(source, old_src);
        DeleteObject(dst);
        DeleteObject(src);
        DeleteDC(target);
        DeleteDC(source);
    }
    canvas_close(&shade);
}

static void sample_hint(Rect screen) {
    RECT box = {escape_hint.left - screen.left, escape_hint.top - screen.top,
                escape_hint.right - screen.left, escape_hint.bottom - screen.top};
    assert(box.right > box.left && box.bottom > box.top);
    hint_sample = (POINT){box.left + 6, (box.top + box.bottom) / 2};
    // Ignore rounded edges and the contrasting border in high-contrast mode.
    hint_label = box;
    InflateRect(&hint_label, -12, -4);
}

static void preview_frame_test(void) {
    // Exercise real WM_PAINT with a known frozen monitor, not a live capture.
    enum { W = 640, H = 400 };
    const uint32_t frozen = 0xffd0e0f0u;
    assert(open_shade() && open_preview(W, H));
    HDC source = CreateCompatibleDC(NULL);
    BITMAPINFO info = bitmap_info(W, H);
    void *bits;
    HBITMAP dib = CreateDIBSection(source, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    assert(source && dib);
    HGDIOBJ previous = SelectObject(source, dib);
    assert(previous);
    uint32_t *pixels = bits;
    for (int j = 0; j < W * H; ++j) pixels[j] = frozen;
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, CAPTURE_CLASS, L"", WS_POPUP,
                             0, 0, W, H, NULL, NULL, instance, NULL);
    assert(w);
    POINT origin = {0};
    assert(ClientToScreen(w, &origin));
    Rect screen = {origin.x, origin.y, origin.x + W, origin.y + H};
    layout.count = 1;
    desktop_bounds = screen;
    wide_desktop = layout_invalid = preview_failed = FALSE;
    frames[0] = (Frame){screen, bits};
    surfaces[0] = (Surface){.dc = source, .bitmap = dib, .previous = previous, .window = w};
    phase = SELECTING;
    dragging = TRUE;
    anchor = (POINT){screen.left + 350, screen.top + 160};
    ShowWindow(w, SW_SHOWNOACTIVATE);
    assert(update_selection(w, MAKELPARAM(530, 260)));
    assert(!rect_empty(escape_hint));

    observe_preview = w;
    sample_hint(screen);
    inside_sample = (POINT){450, 200};
    outside_sample = (POINT){250, 200};
    assert(UpdateWindow(w) && !preview_failed);
    assert(present_blits == 1 && !present_dibs && !window_shades);
    assert(presented_hint == ui_colors.hint && presented_text);
    const COLORREF original = RGB(0xd0, 0xe0, 0xf0);
    assert(presented_inside == original);
    assert_shaded(presented_outside, original);

    // Reverse the drag across the anchor. Both old/new selection and hint must
    // be invalidated, even though the old hint no longer belongs to the ROI.
    old_hint_sample = hint_sample;
    assert(update_selection(w, MAKELPARAM(60, 260)));
    sample_hint(screen);
    inside_sample = (POINT){250, 200};
    outside_sample = (POINT){450, 200};
    HRGN dirty = CreateRectRgn(0, 0, 0, 0);
    assert(dirty && GetUpdateRgn(w, dirty, FALSE) != ERROR);
    assert(PtInRegion(dirty, old_hint_sample.x, old_hint_sample.y) && PtInRegion(dirty, hint_sample.x, hint_sample.y));
    assert(PtInRegion(dirty, 450, 200) && PtInRegion(dirty, 250, 200));
    DeleteObject(dirty);

    // The fallback must present that same composed frame without an unshaded
    // window paint in between.
    force_present_fallback = TRUE;
    assert(UpdateWindow(w) && !preview_failed);
    assert(present_blits == 2 && present_dibs == 1 && !window_shades);
    assert(presented_hint == ui_colors.hint && presented_text && presented_inside == original);
    assert_shaded(presented_old_hint, original);
    assert_shaded(presented_outside, original);
    force_present_fallback = FALSE;
    observe_preview = NULL;

    Image cut = {0};
    assert(crop_frozen(selection, frames, layout.count, &cut) == CROP_OK);
    assert(cut.width == 290 && cut.height == 100);
    for (size_t j = 0; j < image_bytes(cut.width, cut.height) / 4; ++j)
        assert(((uint32_t *)cut.bgra)[j] == frozen);
    free(cut.bgra);
    cancel_capture();
    assert(phase == IDLE && !IsWindow(w) && layout.count == 0 && rect_empty(escape_hint) && !preview.dc);
}

static void preview_test(void) {
    recent = (Image){1, 1, malloc(4)};
    assert(recent.bgra);
    unsigned char *previous = recent.bgra;

    // A failed first paint must not arm selection.
    fail_preview = TRUE;
    start_capture();
    assert(phase == IDLE && layout.count == 0 && recent.bgra == previous && !copies && !shade.dc && !preview.dc);
    fail_preview = FALSE;

    HWND overlay = small_overlay();
    RECT dirty = {0, 0, 10, 10};
    fail_blit = fail_dib = TRUE;
    assert(InvalidateRect(overlay, &dirty, FALSE));
    UpdateWindow(overlay);
    assert(preview_failed && IsWindow(overlay));
    finish_capture();  // Simulate release before the posted wake is delivered.
    assert(phase == IDLE && !IsWindow(overlay) && recent.bgra == previous && !copies);
    fail_blit = fail_dib = FALSE;

    overlay = small_overlay();
    close_during_blit = overlay;
    assert(InvalidateRect(overlay, &dirty, FALSE));
    UpdateWindow(overlay);
    assert(!close_during_blit && layout_invalid && !preview_failed && IsWindow(overlay));
    sg_after_dispatch();
    assert(phase == IDLE && !IsWindow(overlay) && recent.bgra == previous && !copies);
}

static void replace_test(void) {
    const uint32_t mod = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    const uint32_t first = (mod << 8) | VK_F21, second = (mod << 8) | VK_F22;
    const int blocker = 21;
    assert(sg_set_hotkey(first) == 0 && sg_hotkey() == first && decode_hotkey(persisted) == first);
    int old_id = active_hotkey;
    assert(sg_set_hotkey(VK_F6) == 3 && sg_hotkey() == first && active_hotkey == old_id);
    assert(RegisterHotKey(owner, blocker, mod | MOD_NOREPEAT, VK_F22));
    assert(sg_set_hotkey(second) == 1 && sg_hotkey() == first && active_hotkey == old_id);
    assert(decode_hotkey(persisted) == first && UnregisterHotKey(owner, blocker));
    persist_succeeds = FALSE;
    assert(sg_set_hotkey(second) == 2 && sg_hotkey() == first && active_hotkey == old_id);
    assert(decode_hotkey(persisted) == first);
    persist_succeeds = TRUE;
    assert(sg_set_hotkey(second) == 0 && sg_hotkey() == second && active_hotkey != old_id);
    assert(decode_hotkey(persisted) == second);
    // The previous combination is genuinely available again.
    assert(RegisterHotKey(owner, blocker, mod | MOD_NOREPEAT, VK_F21));
    assert(UnregisterHotKey(owner, blocker));
}

static void verify_export_pixels(const Image *image) {
    IStream *png = NULL;
    IWICImagingFactory *factory = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    size_t bytes = image_bytes(image->width, image->height);
    BYTE *pixels = malloc(bytes);
    assert(pixels && encode_png(image, &png) == S_OK);
    assert(SUCCEEDED(IStream_Seek(png, (LARGE_INTEGER){.QuadPart = 0}, STREAM_SEEK_SET, NULL)));
    assert(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void **)&factory) == S_OK);
    assert(IWICImagingFactory_CreateDecoderFromStream(factory, png, NULL,
        WICDecodeMetadataCacheOnLoad, &decoder) == S_OK);
    assert(IWICBitmapDecoder_GetFrame(decoder, 0, &frame) == S_OK);
    UINT w, h;
    assert(IWICBitmapFrameDecode_GetSize(frame, &w, &h) == S_OK &&
           w == (UINT)image->width && h == (UINT)image->height);
    assert(IWICImagingFactory_CreateFormatConverter(factory, &converter) == S_OK);
    assert(IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom) == S_OK);
    assert(IWICFormatConverter_CopyPixels(converter, NULL, w * 4, (UINT)bytes, pixels) == S_OK);
    assert(!memcmp(pixels, image->bgra, bytes));
    IWICFormatConverter_Release(converter);
    IWICBitmapFrameDecode_Release(frame);
    IWICBitmapDecoder_Release(decoder);
    IWICImagingFactory_Release(factory);
    IStream_Release(png);
    free(pixels);

    HGLOBAL dib = clipboard_dib(image);
    assert(dib);
    const BITMAPV5HEADER *header = GlobalLock(dib);
    assert(header && header->bV5Width == (LONG)w && header->bV5Height == (LONG)h &&
        header->bV5AlphaMask == 0xff000000u);
    const BYTE *dib_pixels = (const BYTE *)(header + 1);
    for (UINT y = 0; y < h; ++y)
        assert(!memcmp(dib_pixels + (h - 1 - y) * w * 4, image->bgra + y * w * 4, w * 4));
    GlobalUnlock(dib);
    GlobalFree(dib);
}

static void theme_test(void) {
    const UiPalette light = ui_palette(FALSE, FALSE), dark = ui_palette(TRUE, FALSE);
    const UiPalette hc_light = ui_palette(FALSE, TRUE), hc_dark = ui_palette(TRUE, TRUE);
    assert(light.surface == RGB(255, 255, 255) && dark.surface == RGB(28, 33, 43));
    assert(light.sidebar != light.surface && dark.sidebar != dark.surface);
    assert(light.surface != dark.surface && light.ink != dark.ink);
    assert(light.accent == dark.accent && light.active != dark.active);
    assert(hc_light.surface == GetSysColor(COLOR_WINDOW) && hc_dark.surface == hc_light.surface);
    assert(hc_dark.ink == GetSysColor(COLOR_WINDOWTEXT) && hc_dark.active == GetSysColor(COLOR_HIGHLIGHT));
    assert(hc_dark.active_ink == GetSysColor(COLOR_HIGHLIGHTTEXT) && hc_dark.accent == hc_light.accent);
    UiFace hc_plain = ui_primary_face(&hc_dark, false), hc_pressed = ui_primary_face(&hc_dark, true);
    assert(hc_plain.fill == GetSysColor(COLOR_WINDOW) && hc_plain.ink == GetSysColor(COLOR_WINDOWTEXT));
    assert(hc_pressed.fill == GetSysColor(COLOR_HIGHLIGHT) && hc_pressed.ink == GetSysColor(COLOR_HIGHLIGHTTEXT));
    UiFace acc = ui_accent_face(&light, false), acc_p = ui_accent_face(&light, true);
    assert(acc.fill == light.accent && acc.ink == RGB(255, 255, 255) && acc_p.fill == light.accent_pressed);
    assert(palette[0] == RGB(233, 74, 68) && palette[1] == RGB(28, 113, 208));
}

static void render_test(void) {
    unsigned char raw[32 * 24 * 4] = {0};
    raw[3] = 255; // An opaque black source pixel must stay opaque.
    Image frozen = {32, 24, raw};
    Canvas target = {0};
    History marks = {0};
    Mark point = {.tool = PEN, .color = RGB(233, 74, 68), .width = 3};
    assert(canvas_open(&target, &frozen));
    assert(mark_point(&point, 8, 5) && history_add(&marks, &point));
    unsigned char *ink = target.image.bgra + (5 * 32 + 8) * 4;
    assert(canvas_render(&target, &frozen, &marks, NULL, (EditPoint){0}));
    assert(!memcmp(ink, (unsigned char[]){68, 74, 233, 255}, 4));
    assert(target.image.bgra[3] == 255 && target.image.bgra[7] == 0);
    assert(history_undo(&marks) && canvas_render(&target, &frozen, &marks, NULL, (EditPoint){0}));
    assert(!memcmp(ink, (unsigned char[]){0, 0, 0, 0}, 4));
    assert(history_redo(&marks) && canvas_render(&target, &frozen, &marks, NULL, (EditPoint){0}) && ink[3] == 255);
    Mark text = {.tool = TEXT, .color = RGB(28, 113, 208), .width = 12, .x = 3, .y = 8, .text = L"拾光"};
    assert(canvas_render(&target, &frozen, &marks, &text, (EditPoint){0}));
    size_t text_pixels = 0;
    for (size_t i = 0; i < sizeof raw; i += 4)
        if (!memcmp(target.image.bgra + i, (unsigned char[]){208, 113, 28, 255}, 4)) ++text_pixels;
    assert(text_pixels > 0);
    verify_export_pixels(&target.image);
    history_clear(&marks);
    canvas_close(&target);
}

static void release_into_editor(void) {
    start_capture();
    assert(phase == SELECTING && layout.count != 0);
    Rect b = frames[0].bounds;
    selection = (Rect){b.left + 2, b.top + 2, b.left + 72, b.top + 56};
    dragging = TRUE;
    invalidate_selection(selection);
    finish_capture();
    assert(phase == EDITING && !dragging && !GetCapture() && toolbar.window);
    assert(base.width == 70 && base.height == 54 && canvas.image.bgra);
}

static void assert_antialiased_arrow(const uint32_t *pixels, int side, COLORREF face, COLORREF ink) {
    assert(GdiFlush());
    int coverage = 0, partial = 0;
    for (int y = side / 6; y < side - side / 6; ++y)
        for (int x = side / 6; x < side - side / 6; ++x) {
            uint32_t px = pixels[y * side + x];
            COLORREF color = RGB(px >> 16, px >> 8, px);
            if (color != face) {
                ++coverage;
                if (color != ink) ++partial;
            }
        }
    assert(coverage > 30 && partial > 16);
}

static void toolbar_glyph_test(void) {
    const UiPalette saved = ui_colors;
    ui_colors = ui_palette(FALSE, FALSE);
    HRSRC notice = FindResourceW(instance, MAKEINTRESOURCEW(102), RT_RCDATA);
    assert(notice && SizeofResource(instance, notice) > 1000);
    // Actual button widths after padding at 100%, 150%, and 200%.
    const int button_sizes[] = {40, 64, 88};
    for (int size = 0; size < (int)ARRAYSIZE(button_sizes); ++size) {
        int side = button_sizes[size], dpi = 96 + size * 48;
        HDC dc = CreateCompatibleDC(NULL);
        uint32_t *pixels;
        BITMAPINFO info = bitmap_info(side, side);
        HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
        assert(dc && dib);
        HGDIOBJ previous = SelectObject(dc, dib);
        DRAWITEMSTRUCT item = {.CtlType = ODT_BUTTON, .CtlID = ACT_ARROW + 1, .hDC = dc,
                               .rcItem = {0, 0, side, side}};
        // WM_DRAWITEM uses this HWND's real DPI; only the direct path can select a DPI explicitly.
        if (size == 0) {
            assert(SendMessageW(toolbar.window, WM_DRAWITEM, ACT_ARROW + 1, (LPARAM)&item));
            assert_antialiased_arrow(pixels, side, ui_colors.surface, ui_colors.ink);
        }
        assert(FillRect(dc, &item.rcItem, GetStockObject(WHITE_BRUSH)));
        glyph_pen_calls = glyph_blits = glyph_paths = 0;
        aa_glyph(dc, item.rcItem, dpi, ACT_ARROW, &toolbar, RGB(0, 0, 0), RGB(255, 255, 255));
        assert(glyph_pen_calls == 1 && glyph_blits == 1);
        assert_antialiased_arrow(pixels, side, RGB(255, 255, 255), RGB(0, 0, 0));

        Rect button = {0, 0, side, side};
        GlyphLayout plain = editor_glyph_layout(button, dpi, 0);
        GlyphLayout with_badge = editor_glyph_layout(button, dpi, 1);
        Panel glyph_panel = toolbar;
        for (int shape = 0; shape < SHAPES; ++shape) {
            int main[88] = {0}, popup[88] = {0}, badge_pixels = 0;
            glyph_panel.shape = (Tool)shape;
            for (int menu = 0; menu <= 1; ++menu) {
                Rect icon = menu ? with_badge.icon : plain.icon;
                assert(FillRect(dc, &item.rcItem, GetStockObject(WHITE_BRUSH)));
                glyph_pen_calls = glyph_blits = glyph_paths = 0;
                aa_glyph(dc, item.rcItem, dpi, menu ? ACT_SHAPES : ACT_RECTANGLE + shape,
                         &glyph_panel, RGB(0, 0, 0), RGB(255, 255, 255));
                assert(glyph_pen_calls == 1 && glyph_blits == 1 + menu &&
                       "compare supersampled shape tiles, not the direct-draw fallback");
                assert(GdiFlush());
                for (int y = 0; y < side; ++y)
                    for (int x = 0; x < side; ++x)
                        if ((pixels[y * side + x] & 0xffffffu) != 0xffffffu) {
                            if (x >= icon.left && x < icon.right && y >= icon.top && y < icon.bottom)
                                ++(menu ? main : popup)[x - icon.left];
                            else if (menu && x >= with_badge.badge.left && x < with_badge.badge.right &&
                                     y >= with_badge.badge.top && y < with_badge.badge.bottom)
                                ++badge_pixels;
                            else assert(!"glyph paints outside its tile");
                        }
            }
            assert(!memcmp(main, popup, sizeof main) && "shape menu and popup must draw the same glyph size");
            assert(badge_pixels > 0 && "menu caret must not overlap the shape");
        }

        glyph_panel.color = 1;
        assert(FillRect(dc, &item.rcItem, GetStockObject(WHITE_BRUSH)));
        aa_glyph(dc, item.rcItem, dpi, ACT_COLOR, &glyph_panel, RGB(0, 0, 0), RGB(255, 255, 255));
        assert(GdiFlush());
        int color_x = (with_badge.icon.left + with_badge.icon.right) / 2;
        assert(GetPixel(dc, color_x, side / 2) == palette[glyph_panel.color]);
        int first_y = side, last_y = -1;
        BOOL halo = FALSE;
        for (int y = 0; y < side; ++y) {
            COLORREF pixel = GetPixel(dc, color_x, y);
            if (pixel != RGB(255, 255, 255)) {
                if (y < first_y) first_y = y;
                if (y > last_y) last_y = y;
                if (pixel != palette[glyph_panel.color] && GetBValue(pixel) > 160 && GetRValue(pixel) > 150)
                    halo = TRUE;
            }
        }
        int diameter = first_y <= last_y ? last_y - first_y + 1 : 0;
        int expected = MulDiv(17, dpi, 96);
        assert(diameter >= expected - 2 && diameter <= expected + 3);
        assert(halo && "swatch must retain a subtle outer halo ring");

        // Neither a failed allocation nor a failed primitive may blit the tile.
        DWORD objects = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int n = 0; n < 8; ++n) {
            assert(FillRect(dc, &item.rcItem, GetStockObject(WHITE_BRUSH)));
            fail_glyph_path = n % 2;
            fail_glyph_pen = !fail_glyph_path;
            glyph_pen_calls = glyph_blits = glyph_paths = 0;
            aa_glyph(dc, item.rcItem, dpi, ACT_ARROW, &toolbar, RGB(0, 0, 0), RGB(255, 255, 255));
            assert(glyph_pen_calls == 2 && glyph_blits == 0);
            assert(GdiFlush());
            int direct = 0;
            for (int y = side / 2 - 1; y <= side / 2 + 1; ++y)
                for (int x = side / 2 - 1; x <= side / 2 + 1; ++x)
                    if (!(pixels[y * side + x] & 0xffffffu)) ++direct;
            assert(direct > 0);
        }
        assert(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == objects);
        fail_glyph_pen = fail_glyph_path = FALSE;
        SelectObject(dc, previous);
        DeleteObject(dib);
        DeleteDC(dc);
    }
    ui_colors = saved;
}

static void send_edit_mouse(HWND w, UINT msg, POINT at) {
    assert(!wide_desktop && ScreenToClient(w, &at));
    SendMessageW(w, msg, msg == WM_LBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(at.x, at.y));
}

static void adjustment_test(void) {
    release_into_editor();
    Rect original = selection;
    HWND w = surfaces[0].window, bar = toolbar.window;
    unsigned char *previous_recent = recent.bgra;
    int previous_copies = copies;
    POINT edge = {original.right - 1, original.bottom - 1};
    POINT smaller = {edge.x - 40, edge.y - 12};
    RECT first_bar;
    assert(GetWindowRect(bar, &first_bar));
    Mark point = {.tool = PEN, .color = palette[0], .width = 3};
    assert(mark_point(&point, 60, 20) && history_add(&history, &point));
    Mark redo = {.tool = RECTANGLE, .color = palette[1], .width = 3, .end_x = 10, .end_y = 10};
    assert(history_add(&history, &redo) && history_undo(&history));
    edit_changed();
    assert(flush_overlays());
    tool = TEXT;
    start_mark(w, (POINT){original.left + 12, original.top + 16});
    HWND field = text_input;
    assert(field && SetWindowTextW(field, L"保留输入"));

    // A failed allocation leaves the original crop, edit, and redo states intact.
    unsigned char *original_pixels = base.bgra;
    HDC original_canvas = canvas.dc;
    if (wide_desktop) assert(start_adjustment(w, SEL_E | SEL_S, edge));
    else send_edit_mouse(w, WM_LBUTTONDOWN, edge);
    assert(adjust_mode == (SEL_E | SEL_S) && !drawing);
    if (wide_desktop) update_adjustment(smaller);
    else send_edit_mouse(w, WM_MOUSEMOVE, smaller);
    assert(bar_dock.bounds.top != first_bar.top && toolbar.window == bar);
    fail_adjust_canvas = TRUE;
    if (wide_desktop) finish_adjustment(w, TRUE);
    else send_edit_mouse(w, WM_LBUTTONUP, smaller);
    fail_adjust_canvas = FALSE;
    assert(failed_canvas_dc && GetObjectType(failed_canvas_dc) == 0);
    assert(same_rect(selection, original) && base.bgra == original_pixels && canvas.dc == original_canvas);
    assert(bar_dock.bounds.top == first_bar.top && recent.bgra == previous_recent && copies == previous_copies);
    assert(text_input == field && !adjust_mode && !drawing && history.used == 1 && history.total == 2);

    assert(start_adjustment(w, SEL_E | SEL_S, edge));
    update_adjustment((POINT){edge.x - 40, edge.y - 12});
    finish_adjustment(w, TRUE);
    assert(base.width == 30 && history.used == 1 && history.total == 2);
    assert(history.marks[0].points[0].x == 60 && text_input == field);
    WCHAR text[16];
    assert(GetWindowTextW(field, text, ARRAYSIZE(text)) == 4 && !wcscmp(text, L"保留输入"));

    POINT inside = {selection.left + 16, selection.top + 21};
    adjust_alt = TRUE;
    ime_composing = TRUE;
    if (wide_desktop) assert(!start_adjustment(w, SEL_MOVE, inside));
    else send_edit_mouse(field, WM_LBUTTONDOWN, inside);
    assert(!adjust_mode && text_input == field);
    ime_composing = FALSE;
    if (wide_desktop) assert(start_adjustment(w, SEL_MOVE, inside));
    else send_edit_mouse(field, WM_LBUTTONDOWN, inside);
    adjust_alt = FALSE; // Drag mode is latched even if Alt is released early.
    assert(adjust_mode == SEL_MOVE && !drawing && text_input == field);
    POINT moved = {inside.x + 50, inside.y};
    if (wide_desktop) {
        update_adjustment(moved);
        finish_adjustment(w, TRUE);
    } else send_edit_mouse(w, WM_LBUTTONUP, moved);
    assert(base.width == 30 && selection.left == original.left + 50);
    unsigned char *ink = canvas.image.bgra + (20 * base.width + 10) * 4;
    assert(!memcmp(ink, (unsigned char[]){68, 74, 233, 255}, 4));
    assert(history_redo(&history) && history_undo(&history) && history.total == 2);

    assert(start_adjustment(w, SEL_MOVE, (POINT){selection.left + 10, selection.top + 20}));
    Rect before_cancel = selection;
    update_adjustment((POINT){original.left + 20, original.top + 20});
    SendMessageW(w, WM_CANCELMODE, 0, 0);
    assert(!adjust_mode && same_rect(selection, before_cancel) && text_input == field);

    struct { HWND target; UINT msg; WPARAM key; } escapes[] = {
        {owner, WM_HOTKEY, HOTKEY_CANCEL}, {w, WM_KEYDOWN, VK_ESCAPE},
        {field, WM_KEYDOWN, VK_ESCAPE}, {field, WM_SYSKEYDOWN, VK_ESCAPE},
        {toolbar.buttons[ACT_PEN], WM_KEYDOWN, VK_ESCAPE}
    };
    // Exercise WM_HOTKEY dispatch without acquiring the machine's Escape key.
    BOOL originally_registered = escape_registered;
    escape_registered = TRUE;
    for (size_t i = 0; i < ARRAYSIZE(escapes); ++i) {
        assert(start_adjustment(w, SEL_MOVE, (POINT){selection.left + 10, selection.top + 20}));
        update_adjustment((POINT){original.left + 20, original.top + 20});
        assert(!same_rect(adjust_area, before_cancel));
        SendMessageW(escapes[i].target, escapes[i].msg, escapes[i].key, 0);
        assert(phase == EDITING && !adjust_mode && !GetCapture());
        assert(same_rect(selection, before_cancel) && text_input == field);
        assert(history.used == 1 && history.total == 2);
        if (escapes[i].msg != WM_HOTKEY) {
            SendMessageW(escapes[i].target, escapes[i].msg, VK_ESCAPE, 1L << 30);
            SendMessageW(escapes[i].target, escapes[i].msg == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR, 27, 0);
            assert(phase == EDITING && text_input == field);
        }
    }
    escape_registered = originally_registered;
    SendMessageW(field, WM_KEYDOWN, VK_ESCAPE, 0);
    assert(phase == IDLE && recent.bgra == previous_recent && copies == previous_copies);
}

static void narrow_edit_test(void) {
    release_into_editor();
    HWND w = surfaces[0].window;
    POINT edge = {selection.right - 1, selection.top + 18};
    assert(start_adjustment(w, SEL_E, edge));
    update_adjustment((POINT){edge.x - 65, edge.y});
    finish_adjustment(w, TRUE);
    assert(base.width == 5 && phase == EDITING);
    if (!wide_desktop) {
        POINT mark = {selection.left + 2, selection.top + 15};
        tool = PEN;
        send_edit_mouse(w, WM_LBUTTONDOWN, mark);
        assert(drawing && !adjust_mode);
        send_edit_mouse(w, WM_LBUTTONUP, mark);
        assert(!drawing && history.used == 1);
    }
    escape_registered = TRUE;
    SendMessageW(owner, WM_HOTKEY, HOTKEY_CANCEL, 0);
    assert(phase == IDLE && !escape_registered && !history.used);
}

static void edit_test(void) {
    theme_test();
    render_test();
    adjustment_test();
    narrow_edit_test();
    recent = (Image){1, 1, malloc(4)};
    assert(recent.bgra);
    unsigned char *old = recent.bgra;
    uintptr_t old_address = (uintptr_t)old;
    release_into_editor();
    assert(recent.bgra == old && !copies);
    toolbar_glyph_test();
    Rect screen = frames[0].bounds;
    HDC probe = CreateCompatibleDC(NULL);
    void *bits;
    BITMAPINFO info = bitmap_info(12, 12);
    HBITMAP dib = CreateDIBSection(probe, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    assert(probe && dib);
    HGDIOBJ prev = SelectObject(probe, dib);
    RECT dirty = {0, 0, 12, 12};
    int x = selection.left - screen.left, y = selection.top - screen.top;
    assert(IntersectClipRect(probe, 0, 0, 12, 12) != ERROR && draw_preview(probe, &dirty, 0));
    assert(GetPixel(probe, x, y) == RGB(255, 255, 255) && GetPixel(probe, x + 6, y) == RGB(0, 0, 0));
    assert(GetPixel(probe, x + 6, y + 1) == GetPixel(surfaces[0].dc, x + 6, y + 1));
    COLORREF outside = GetPixel(probe, x - 1, y + 1);
    COLORREF original = GetPixel(surfaces[0].dc, x - 1, y + 1);
    assert_shaded(outside, original);
    paint_grips(probe, screen);
    assert(GetPixel(probe, x, y) == RGB(255, 255, 255));
    assert(GetPixel(probe, x + 3, y) == ui_colors.accent);
    assert(!memcmp(canvas.image.bgra, base.bgra, image_bytes(base.width, base.height)));
    SelectObject(probe, prev);
    DeleteObject(dib);
    DeleteDC(probe);
    assert(!IsWindowEnabled(toolbar.buttons[ACT_UNDO]) && !IsWindowEnabled(toolbar.buttons[ACT_REDO]));
    draft = (Mark){.tool = RECTANGLE, .color = palette[0], .width = 3,
                   .x = 1, .y = 1, .end_x = 14, .end_y = 14};
    commit_draft();
    assert(IsWindowEnabled(toolbar.buttons[ACT_UNDO]));
    editor_action(ACT_UNDO);
    assert(!IsWindowEnabled(toolbar.buttons[ACT_UNDO]) && IsWindowEnabled(toolbar.buttons[ACT_REDO]));
    editor_action(ACT_REDO);
    assert(IsWindowEnabled(toolbar.buttons[ACT_UNDO]) && !IsWindowEnabled(toolbar.buttons[ACT_REDO]));

    // Force real panel/overlay paints in all modes without changing system preferences.
    assert(flush_overlays());
    size_t bytes = image_bytes(canvas.image.width, canvas.image.height);
    unsigned char *before = malloc(bytes);
    assert(before);
    memcpy(before, canvas.image.bgra, bytes);
    for (unsigned mode = 0; mode < 3; ++mode) {
        ui_colors = ui_palette(mode == 1, mode == 2);
        invalidate_theme_windows();
        assert(flush_overlays());
        assert(RedrawWindow(toolbar.window, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN));
        assert(!memcmp(before, canvas.image.bgra, bytes));
    }
    Image frozen_render = {canvas.image.width, canvas.image.height, before};
    verify_export_pixels(&frozen_render);

    save_result = S_FALSE;
    editor_action(ACT_SAVE);
    assert(phase == EDITING && history.used == 1 && recent.bgra == old);
    save_result = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    editor_action(ACT_SAVE);
    assert(phase == EDITING && history.used == 1 && recent.bgra == old);
    copy_succeeds = FALSE;
    expected_copy = &frozen_render;
    editor_action(ACT_COPY);
    assert(phase == EDITING && history.used == 1 && copies == 1 && recent.bgra == old);
    copy_succeeds = TRUE;
    SendMessageW(toolbar.buttons[ACT_COPY], WM_KEYDOWN, VK_RETURN, 0);
    assert(phase == IDLE && recent.bgra && (uintptr_t)recent.bgra != old_address && copies == 2 && recent.width == 70);
    expected_copy = NULL;
    free(before);

    unsigned char *copied = recent.bgra;
    release_into_editor();
    SendMessageW(toolbar.buttons[ACT_PEN], WM_KEYDOWN, VK_ESCAPE, 0);
    assert(phase == IDLE && recent.bgra == copied && copies == 2 && !history.used);

    release_into_editor();
    save_result = E_FAIL;
    editor_action(ACT_SAVE);
    assert(phase == EDITING && recent.bgra == copied && copies == 2);
    save_result = S_OK;
    editor_action(ACT_SAVE);
    assert(phase == IDLE && recent.bgra && recent.width == 70 && copies == 2);
}

static void assert_no_target(void) {
    assert(GetFileAttributesW(picked_path) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND);
}

static void save_editor_test(void) {
    WCHAR temp[MAX_PATH], folder[MAX_PATH], path[MAX_PATH];
    DWORD len = GetTempPathW(ARRAYSIZE(temp), temp);
    assert(len && len < ARRAYSIZE(temp));
    assert(GetTempFileNameW(temp, L"sgt", 0, folder) && DeleteFileW(folder));
    assert(CreateDirectoryW(folder, NULL));
    assert(swprintf(path, ARRAYSIZE(path), L"%ls\\target.png", folder) > 0);
    picked_path = path;

    recent = (Image){1, 1, malloc(4)};
    assert(recent.bgra);
    memcpy(recent.bgra, "\x11\x22\x33\xff", 4);
    unsigned char *old = recent.bgra;
    for (int late = 0; late < 2; ++late) {
        release_into_editor();
        save_result = S_OK;
        if (late) invalidate_on_flush = surfaces[0].window;
        else invalidate_in_picker = owner;
        editor_action(ACT_SAVE);
        assert(!invalidate_in_picker && !invalidate_on_flush);
        // Late invalidation must reach the post-staging guard, not an I/O error path.
        assert(guard_calls == 1u + (unsigned)late && !last_guard_allowed);
        assert_no_target();
        assert(phase == IDLE && !base.bgra && !canvas.image.bgra && layout.count == 0 && !toolbar.window);
        assert(recent.bgra == old && !memcmp(recent.bgra, "\x11\x22\x33\xff", 4) && !copies);
        assert(moves == 0 && stage_flushes == (unsigned)late);
        // Removing the otherwise empty workspace proves no staged temp survived.
        assert(RemoveDirectoryW(folder) && CreateDirectoryW(folder, NULL));
    }

    // The tray saves a previously completed image, independent of editor layout.
    assert(layout_invalid);
    SendMessageW(owner, COMMAND_MSG, CMD_SAVE, 0);
    assert(phase == IDLE && recent.bgra == old && !copies && moves == 1);
    assert(DeleteFileW(path));

    release_into_editor();
    unsigned before = stage_flushes;
    save_result = S_FALSE; // A success-class HRESULT, but not permission to write.
    editor_action(ACT_SAVE);
    assert_no_target();
    assert(phase == EDITING && recent.bgra == old && stage_flushes == before && moves == 1 && !copies);
    save_result = S_OK;
    create_target(path);
    editor_action(ACT_SAVE);
    assert(phase == EDITING && recent.bgra == old && moves == 2 && !copies);
    HANDLE target = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char bytes[sizeof marker + 1] = {0};
    DWORD count;
    assert(target != INVALID_HANDLE_VALUE && ReadFile(target, bytes, sizeof bytes, &count, NULL));
    assert(CloseHandle(target) && count == sizeof marker && !memcmp(bytes, marker, sizeof marker));
    assert(DeleteFileW(path));
    editor_action(ACT_SAVE);
    assert(phase == IDLE && recent.width == 70 && recent.height == 54 && moves == 3 && !copies);
    assert(DeleteFileW(path) && RemoveDirectoryW(folder));
    picked_path = NULL;
}

static void settings_caption_test(void) {
    // The same icon group must be usable by Explorer and by the window/tray loader.
    assert(FindResourceW(instance, MAKEINTRESOURCEW(IDI_SHIGUANG), RT_GROUP_ICON));
    small_icon = load_app_icon(SM_CXSMICON);
    large_icon = load_app_icon(SM_CXICON);
    assert(small_icon && large_icon);
    const WNDCLASSW cls = {.lpfnWndProc = DefWindowProcW, .hInstance = instance,
                           .lpszClassName = L"Shiguang.Test.Settings"};
    assert(RegisterClassW(&cls));
    MONITORINFO monitor = {.cbSize = sizeof monitor};
    assert(GetMonitorInfoW(MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor));
    RECT work = monitor.rcWork;
    HWND window = CreateWindowExW(0, cls.lpszClassName, APP_TITLE, WS_OVERLAPPEDWINDOW,
        work.left, work.top, 570, 350, NULL, NULL, instance, NULL);
    RECT content, after, frame;
    assert(window && GetClientRect(window, &content) && sg_bind_settings(window));
    assert(!IsWindowVisible(window));
    assert((HICON)SendMessageW(window, WM_GETICON, ICON_SMALL, 0) == small_icon);
    assert((HICON)SendMessageW(window, WM_GETICON, ICON_BIG, 0) == large_icon);
    assert(GetClientRect(window, &after) && EqualRect(&content, &after));
    LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    assert(!(style & (WS_MAXIMIZEBOX | WS_THICKFRAME | WS_DLGFRAME)));
    assert((style & (WS_SYSMENU | WS_MINIMIZEBOX)) == (WS_SYSMENU | WS_MINIMIZEBOX));
    HWND title = CreateWindowExW(0, L"STATIC", L"拾光截图 / 设置", WS_CHILD | WS_VISIBLE,
        0, 0, 400, 48, window, NULL, instance, NULL);
    HWND min = CreateWindowExW(0, L"BUTTON", L"最小化", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        400, 0, 46, 48, window, NULL, instance, NULL);
    HWND close = CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        446, 0, 46, 48, window, NULL, instance, NULL);
    int dpi = (int)GetDpiForWindow(window);
    HWND toggle = CreateWindowExW(0, L"BUTTON", L"登录 Windows 时启动拾光",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        20, MulDiv(60, dpi, 96), MulDiv(320, dpi, 96), MulDiv(32, dpi, 96),
        window, NULL, instance, NULL);
    assert(title && min && close && toggle);
    show_settings();
    assert(settings_title == title && IsWindowVisible(window));
    assert(GetWindowRect(window, &frame));
    assert(abs((frame.left - work.left) - (work.right - frame.right)) <= 1 &&
           abs((frame.top - work.top) - (work.bottom - frame.bottom)) <= 1 &&
           "settings frame must open centered in the monitor work area");
    RECT header, min_rect;
    assert(GetWindowRect(title, &header) && GetWindowRect(min, &min_rect));
    assert(SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(header.left + 20, header.top + 20)) == HTCAPTION);
    assert(SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(min_rect.left + 20, min_rect.top + 20)) == HTCLIENT);
    RECT close_rect;
    assert(GetWindowRect(close, &close_rect));
    assert(min_rect.right - min_rect.left == close_rect.right - close_rect.left);
    assert(SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(close_rect.left + 20, close_rect.top + 20)) == HTCLIENT);

    assert((GetWindowLongPtrW(toggle, GWL_STYLE) & BS_TYPEMASK) == BS_AUTOCHECKBOX);
    assert(SendMessageW(toggle, BM_GETCHECK, 0, 0) == BST_UNCHECKED);
    SetActiveWindow(window);
    SetFocus(toggle);
    SendMessageW(toggle, BM_CLICK, 0, 0);
    assert(SendMessageW(toggle, BM_GETCHECK, 0, 0) == BST_CHECKED);
    HDC probe = CreateCompatibleDC(NULL);
    RECT bounds;
    assert(probe && GetClientRect(toggle, &bounds));
    BITMAPINFO info = bitmap_info(bounds.right, bounds.bottom);
    void *bits;
    HBITMAP dib = CreateDIBSection(probe, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    assert(dib);
    HGDIOBJ previous = SelectObject(probe, dib);
    SendMessageW(toggle, WM_PRINTCLIENT, (WPARAM)probe, PRF_CLIENT);
    COLORREF track = GetPixel(probe, bounds.right - MulDiv(28, dpi, 96), bounds.bottom / 2);
    assert(track == ui_colors.accent);
    SelectObject(probe, previous);
    DeleteObject(dib);
    DeleteDC(probe);
    EnableWindow(toggle, FALSE);
    assert(!IsWindowEnabled(toggle) && SendMessageW(toggle, BM_GETCHECK, 0, 0) == BST_CHECKED);

    MSG message;
    sg_update_set_available(0);
    SendMessageW(owner, COMMAND_MSG, CMD_UPDATE, 0);
    assert(!PeekMessageW(&message, window, SG_UPDATE_MESSAGE, SG_UPDATE_MESSAGE, PM_REMOVE));
    sg_update_set_available(1);
    SendMessageW(owner, COMMAND_MSG, CMD_UPDATE, 0);
    assert(PeekMessageW(&message, window, SG_UPDATE_MESSAGE, SG_UPDATE_MESSAGE, PM_REMOVE));
    assert(message.wParam == 1);
    sg_update_set_available(0);

    // Move the shown window as a user could, then exercise both ways of reopening it.
    assert(SetWindowPos(window, NULL, frame.left - 24, frame.top - 24, 0, 0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
    RECT moved;
    assert(GetWindowRect(window, &moved) && !EqualRect(&frame, &moved));
    sg_settings_caption(0);
    assert(PeekMessageW(&message, window, WM_SYSCOMMAND, WM_SYSCOMMAND, PM_REMOVE));
    DispatchMessageW(&message);
    assert(IsIconic(window));
    show_settings();
    assert(!IsIconic(window) && IsWindowVisible(window));
    assert(GetWindowRect(window, &after) && EqualRect(&moved, &after));

    tray_ready = TRUE;  // Use the real close handler without installing a tray icon.
    sg_settings_caption(1);
    assert(PeekMessageW(&message, window, WM_CLOSE, WM_CLOSE, PM_REMOVE));
    DispatchMessageW(&message);
    assert(IsWindow(window) && !IsWindowVisible(window));
    show_settings();
    assert(IsWindowVisible(window) && GetWindowRect(window, &after) && EqualRect(&moved, &after));
    tray_ready = FALSE;
    assert(DestroyWindow(window));
}

static void startup_test(void) {
    GUID id;
    WCHAR guid[40], path[96];
    assert(CoCreateGuid(&id) == S_OK && StringFromGUID2(&id, guid, ARRAYSIZE(guid)));
    assert(swprintf(path, ARRAYSIZE(path), L"Software\\Shiguang.Test.Startup-%ls", guid) > 0);
    HKEY key, readonly;
    DWORD created;
    assert(RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, REG_OPTION_VOLATILE,
        KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key, &created) == ERROR_SUCCESS);
    assert(created == REG_CREATED_NEW_KEY);
    assert(RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &readonly) == ERROR_SUCCESS);

    const WCHAR exe[] = L"C:\\应用 程序\\拾光.exe", expected[] = L"\"C:\\应用 程序\\拾光.exe\" --startup";
    WCHAR command[260], stored[260];
    assert(startup_command(exe, command) && !wcscmp(expected, command));
    assert(!startup_command(L"C:\\bad\"name.exe", stored));
    assert(startup_state(key, command) == STARTUP_OFF);
    assert(startup_apply(readonly, command, TRUE) == ERROR_ACCESS_DENIED);
    assert(startup_state(key, command) == STARTUP_OFF);
    assert(startup_apply(key, command, TRUE) == ERROR_SUCCESS);
    assert(startup_apply(readonly, command, TRUE) == ERROR_SUCCESS); // no second write
    assert(startup_state(key, command) == STARTUP_ON);
    DWORD bytes = sizeof stored, type;
    assert(RegQueryValueExW(key, STARTUP_NAME, NULL, &type, (BYTE *)stored, &bytes) == ERROR_SUCCESS);
    assert(type == REG_SZ && bytes == sizeof expected && !memcmp(stored, expected, bytes));
    assert(startup_apply(readonly, command, FALSE) == ERROR_ACCESS_DENIED);
    assert(startup_state(key, command) == STARTUP_ON);

    // A different path or a non-string entry is not ours to replace or delete.
    const WCHAR moved[] = L"\"D:\\moved\\拾光.exe\" --startup";
    assert(startup_state(key, moved) == STARTUP_FOREIGN);
    assert(startup_apply(key, moved, TRUE) == ERROR_ALREADY_EXISTS);
    assert(startup_apply(key, moved, FALSE) == ERROR_ALREADY_EXISTS);
    assert(startup_state(key, command) == STARTUP_ON);
    assert(startup_apply(key, command, FALSE) == ERROR_SUCCESS);
    assert(startup_apply(readonly, command, FALSE) == ERROR_SUCCESS);
    DWORD marker = 42;
    assert(RegSetValueExW(key, STARTUP_NAME, 0, REG_DWORD, (BYTE *)&marker, sizeof marker) == ERROR_SUCCESS);
    assert(startup_state(key, command) == STARTUP_FOREIGN);
    assert(startup_apply(key, command, FALSE) == ERROR_ALREADY_EXISTS);
    DWORD read_marker = 0;
    bytes = sizeof read_marker;
    assert(RegQueryValueExW(key, STARTUP_NAME, NULL, &type, (BYTE *)&read_marker, &bytes) == ERROR_SUCCESS);
    assert(type == REG_DWORD && read_marker == marker);
    assert(RegCloseKey(readonly) == ERROR_SUCCESS && RegCloseKey(key) == ERROR_SUCCESS);
    assert(RegDeleteKeyW(HKEY_CURRENT_USER, path) == ERROR_SUCCESS);
}

static void update_busy_test(void) {
    assert(phase == IDLE && sg_update_can_restart());
    for (enum Phase candidate = PREPARING; candidate <= DRAINING; ++candidate) {
        phase = candidate;
        assert(!sg_update_can_restart());
    }
    phase = IDLE;
    menu_open = TRUE;
    assert(!sg_update_can_restart());
    menu_open = FALSE;
    // Nonzero hook alone reserves the recorder, before/after actual key input.
    recording_hook = (HHOOK)(UINT_PTR)1;
    assert(!sg_update_can_restart());
    recording_hook = NULL;
    assert(sg_update_can_restart());
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ||
        AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    // Do not register the app's capture shortcut, show a tray icon, or use its mutex.
    instance = GetModuleHandleW(NULL);
    const WNDCLASSW controller = {.lpfnWndProc = owner_proc, .hInstance = instance,
                                  .lpszClassName = OWNER_CLASS};
    const WNDCLASSW overlay = {.lpfnWndProc = overlay_proc, .hInstance = instance,
                               .lpszClassName = CAPTURE_CLASS};
    assert(SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)));
    INITCOMMONCONTROLSEX controls = {.dwSize = sizeof controls, .dwICC = ICC_WIN95_CLASSES};
    assert(InitCommonControlsEx(&controls) && register_panel(instance));
    assert(RegisterClassW(&controller) && RegisterClassW(&overlay));
    owner = CreateWindowExW(0, OWNER_CLASS, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, instance, NULL);
    assert(owner);
    if (!strcmp(argv[1], "save")) save_test();
    else if (!strcmp(argv[1], "hotkey")) hotkey_test();
    else if (!strcmp(argv[1], "preview")) { preview_test(); selection_preview_test(); preview_frame_test(); }
    else if (!strcmp(argv[1], "replace")) replace_test();
    else if (!strcmp(argv[1], "edit")) edit_test();
    else if (!strcmp(argv[1], "save-edit")) save_editor_test();
    else if (!strcmp(argv[1], "settings-caption")) settings_caption_test();
    else if (!strcmp(argv[1], "startup")) startup_test();
    else if (!strcmp(argv[1], "update-busy")) update_busy_test();
    else assert(!"unknown Windows regression case");
    sg_stop();
    CoUninitialize();
    printf("%s regression passed\n", argv[1]);
}

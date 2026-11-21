#define COBJMACROS
#define UNICODE
#include "../src/desktop_win.h"
#include "../src/crop.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static BOOL fail_blit, fail_dib, fail_blend;
static int copies, blits;
static HRESULT save_result;
static BOOL copy_succeeds = TRUE, persist_succeeds = TRUE;
static DWORD persisted;
static const Image *expected_copy;
static HWND close_during_blend, invalidate_in_picker, invalidate_on_flush;
static const WCHAR *picked_path;
static unsigned stage_flushes, moves;

static BOOL test_blit(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD op) {
    ++blits;
    return !fail_blit && BitBlt(dst, x, y, w, h, src, sx, sy, op);
}

static int test_dib(HDC dst, int x, int y, int w, int h, int sx, int sy, int sw, int sh,
                    const void *bits, const BITMAPINFO *info, UINT usage, DWORD op) {
    return fail_dib ? GDI_ERROR : StretchDIBits(dst, x, y, w, h, sx, sy, sw, sh, bits, info, usage, op);
}

static BOOL test_blend(HDC dst, int x, int y, int w, int h, HDC src,
                       int sx, int sy, int sw, int sh, BLENDFUNCTION blend) {
    if (close_during_blend) {
        HWND w = close_during_blend;
        close_during_blend = NULL;
        SendMessageW(w, WM_CLOSE, 0, 0);
        assert(IsWindow(w));  // Reentrant cancellation cannot free a live paint target.
    }
    return !fail_blend && AlphaBlend(dst, x, y, w, h, src, sx, sy, sw, sh, blend);
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

#define BitBlt test_blit
#define StretchDIBits test_dib
#define AlphaBlend test_blend
#define publish_image count_copy
#define save_png_dialog test_save_dialog
#define RegSetKeyValueW test_persist
#include "../src/desktop_win.c"
#undef RegSetKeyValueW
#undef save_png_dialog
#undef publish_image
#undef AlphaBlend
#undef StretchDIBits
#undef BitBlt

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

static void preview_test(void) {
    recent = (Image){1, 1, malloc(4)};
    assert(recent.bgra);
    unsigned char *previous = recent.bgra;

    // A failed first paint must not arm selection.
    fail_blend = TRUE;
    start_capture();
    assert(phase == IDLE && layout.count == 0 && recent.bgra == previous && !copies);
    fail_blend = FALSE;

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
    close_during_blend = overlay;
    assert(InvalidateRect(overlay, &dirty, FALSE));
    UpdateWindow(overlay);
    assert(!close_during_blend && layout_invalid && !preview_failed && IsWindow(overlay));
    sg_after_dispatch();
    assert(phase == IDLE && !IsWindow(overlay) && recent.bgra == previous && !copies);
}

static void replace_test(void) {
    const uint32_t mod = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    const uint32_t first = (mod << 8) | VK_F21, second = (mod << 8) | VK_F22;
    const int blocker = 21;
    assert(sg_set_hotkey(first) == 0 && sg_hotkey() == first && decode_hotkey(persisted) == first);
    int old_id = active_hotkey;
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
    assert(canvas_render(&target, &frozen, &marks, NULL));
    assert(!memcmp(ink, (unsigned char[]){68, 74, 233, 255}, 4));
    assert(target.image.bgra[3] == 255 && target.image.bgra[7] == 0);
    assert(history_undo(&marks) && canvas_render(&target, &frozen, &marks, NULL));
    assert(!memcmp(ink, (unsigned char[]){0, 0, 0, 0}, 4));
    assert(history_redo(&marks) && canvas_render(&target, &frozen, &marks, NULL) && ink[3] == 255);
    Mark text = {.tool = TEXT, .color = RGB(28, 113, 208), .width = 12, .x = 3, .y = 8, .text = L"拾光"};
    assert(canvas_render(&target, &frozen, &marks, &text));
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

static void edit_test(void) {
    render_test();
    recent = (Image){1, 1, malloc(4)};
    assert(recent.bgra);
    unsigned char *old = recent.bgra;
    uintptr_t old_address = (uintptr_t)old;
    release_into_editor();
    assert(recent.bgra == old && !copies);
    assert(!IsWindowEnabled(toolbar.buttons[ACT_UNDO]) && !IsWindowEnabled(toolbar.buttons[ACT_REDO]));
    draft = (Mark){.tool = RECTANGLE, .color = palette[0], .width = 3,
                   .x = 1, .y = 1, .end_x = 14, .end_y = 14};
    commit_draft();
    assert(IsWindowEnabled(toolbar.buttons[ACT_UNDO]));
    editor_action(ACT_UNDO);
    assert(!IsWindowEnabled(toolbar.buttons[ACT_UNDO]) && IsWindowEnabled(toolbar.buttons[ACT_REDO]));
    editor_action(ACT_REDO);
    assert(IsWindowEnabled(toolbar.buttons[ACT_UNDO]) && !IsWindowEnabled(toolbar.buttons[ACT_REDO]));

    save_result = S_FALSE;
    editor_action(ACT_SAVE);
    assert(phase == EDITING && history.used == 1 && recent.bgra == old);
    save_result = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    editor_action(ACT_SAVE);
    assert(phase == EDITING && history.used == 1 && recent.bgra == old);
    copy_succeeds = FALSE;
    expected_copy = &canvas.image;
    editor_action(ACT_COPY);
    assert(phase == EDITING && history.used == 1 && copies == 1 && recent.bgra == old);
    copy_succeeds = TRUE;
    SendMessageW(toolbar.buttons[ACT_COPY], WM_KEYDOWN, VK_RETURN, 0);
    assert(phase == IDLE && recent.bgra && (uintptr_t)recent.bgra != old_address && copies == 2 && recent.width == 70);
    expected_copy = NULL;

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
    else if (!strcmp(argv[1], "preview")) preview_test();
    else if (!strcmp(argv[1], "replace")) replace_test();
    else if (!strcmp(argv[1], "edit")) edit_test();
    else if (!strcmp(argv[1], "save-edit")) save_editor_test();
    else assert(!"expected save, hotkey, preview, replace, edit or save-edit");
    sg_stop();
    CoUninitialize();
    printf("%s regression passed\n", argv[1]);
}

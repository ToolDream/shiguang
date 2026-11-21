#define COBJMACROS
#include "export_win.h"
#include <shobjidl.h>
#include <wincodec.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static HRESULT encode_png(const Image *image, IStream **encoded) {
    IWICImagingFactory *factory = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IStream *stream = NULL;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    UINT stride = (UINT)image->width * 4;
    HRESULT hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IWICImagingFactory, (void **)&factory);
    if (SUCCEEDED(hr))
        hr = IWICImagingFactory_CreateEncoder(factory, &GUID_ContainerFormatPng, NULL, &encoder);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(encoder, stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(frame, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(frame, (UINT)image->width, (UINT)image->height);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
    if (SUCCEEDED(hr) && !IsEqualGUID(&format, &GUID_WICPixelFormat32bppBGRA)) hr = E_FAIL;
    if (SUCCEEDED(hr))
        hr = IWICBitmapFrameEncode_WritePixels(frame, (UINT)image->height, stride,
                                               stride * (UINT)image->height, image->bgra);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(encoder);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (factory) IWICImagingFactory_Release(factory);
    if (SUCCEEDED(hr)) *encoded = stream;
    else if (stream) IStream_Release(stream);
    return hr;
}

static HRESULT png_bytes(IStream *stream, HGLOBAL *buffer, DWORD *length) {
    STATSTG stat;
    HRESULT hr = IStream_Stat(stream, &stat, STATFLAG_NONAME);
    if (FAILED(hr)) return hr;
    if (stat.cbSize.HighPart || !stat.cbSize.LowPart) return E_FAIL;
    *length = stat.cbSize.LowPart;
    return GetHGlobalFromStream(stream, buffer);
}

static HGLOBAL clipboard_png(const Image *image) {
    IStream *stream = NULL;
    if (FAILED(encode_png(image, &stream))) return NULL;

    HGLOBAL src, copy = NULL;
    DWORD length;
    if (SUCCEEDED(png_bytes(stream, &src, &length))) {
        copy = GlobalAlloc(GMEM_MOVEABLE, length);
        const void *from = GlobalLock(src);
        void *to = copy ? GlobalLock(copy) : NULL;
        if (from && to) memcpy(to, from, length);
        if (from) GlobalUnlock(src);
        if (to) GlobalUnlock(copy);
        if (!(from && to) && copy) {
            GlobalFree(copy);
            copy = NULL;
        }
    }
    IStream_Release(stream);
    return copy;
}

static HGLOBAL clipboard_dib(const Image *image) {
    size_t stride = (size_t)image->width * 4;
    size_t bytes = stride * (size_t)image->height;
    HGLOBAL buffer = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(BITMAPV5HEADER) + bytes);
    if (!buffer) return NULL;
    BITMAPV5HEADER *header = GlobalLock(buffer);
    if (!header) {
        GlobalFree(buffer);
        return NULL;
    }
    *header = (BITMAPV5HEADER){
        .bV5Size = sizeof *header, .bV5Width = image->width, .bV5Height = image->height,
        .bV5Planes = 1, .bV5BitCount = 32, .bV5Compression = BI_BITFIELDS,
        .bV5SizeImage = (DWORD)bytes, .bV5RedMask = 0x00ff0000, .bV5GreenMask = 0x0000ff00,
        .bV5BlueMask = 0x000000ff, .bV5AlphaMask = 0xff000000,
        .bV5CSType = LCS_sRGB, .bV5Intent = LCS_GM_IMAGES
    };
    unsigned char *dst = (unsigned char *)(header + 1);
    for (int y = 0; y < image->height; ++y)
        memcpy(dst + (size_t)(image->height - 1 - y) * stride, image->bgra + (size_t)y * stride, stride);
    GlobalUnlock(buffer);
    return buffer;
}

BOOL publish_image(HWND owner, const Image *image) {
    HGLOBAL dib = clipboard_dib(image);
    if (!dib) return FALSE;
    UINT png_format = RegisterClipboardFormatW(L"PNG");
    HGLOBAL png = png_format ? clipboard_png(image) : NULL;
    BOOL published = FALSE;
    if (OpenClipboard(owner)) {
        if (EmptyClipboard()) {
            // Eager GMEM_MOVEABLE transfers ownership to the OS, with no delayed renderer.
            if (SetClipboardData(CF_DIBV5, dib)) {
                dib = NULL;
                published = TRUE;
            }
            if (png && SetClipboardData(png_format, png)) {
                png = NULL;
                published = TRUE;
            }
        }
        CloseClipboard();
    }
    if (dib) GlobalFree(dib);
    if (png) GlobalFree(png);
    return published;
}

static HRESULT write_png(const wchar_t *path, IStream *stream, SaveAllowed allowed) {
    HGLOBAL buffer;
    DWORD length;
    HRESULT hr = png_bytes(stream, &buffer, &length);
    if (FAILED(hr)) return hr;
    void *bytes = GlobalLock(buffer);
    if (!bytes) return E_FAIL;
    GUID id;
    wchar_t suffix[40];
    hr = CoCreateGuid(&id);
    if (FAILED(hr) || !StringFromGUID2(&id, suffix, ARRAYSIZE(suffix))) {
        GlobalUnlock(buffer);
        return E_FAIL;
    }

    // Stage beside the destination so a failed encode/write never truncates an existing PNG.
    const wchar_t *name = wcsrchr(path, L'\\');
    size_t folder = name ? (size_t)(name + 1 - path) : 0;
    wchar_t *tmp = malloc((folder + ARRAYSIZE(suffix) + 4) * sizeof *tmp);
    if (!tmp) {
        GlobalUnlock(buffer);
        return E_OUTOFMEMORY;
    }
    wmemcpy(tmp, path, folder);
    wcscpy(tmp + folder, suffix);
    wcscat(tmp, L".tmp");
    HANDLE file = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        hr = HRESULT_FROM_WIN32(GetLastError());
    } else {
        hr = S_OK;
        DWORD written;
        if (!WriteFile(file, bytes, length, &written, NULL)) hr = HRESULT_FROM_WIN32(GetLastError());
        else if (written != length) hr = E_FAIL;
        else if (!FlushFileBuffers(file)) hr = HRESULT_FROM_WIN32(GetLastError());
        if (!CloseHandle(file) && hr == S_OK) hr = HRESULT_FROM_WIN32(GetLastError());
        // The session may have been invalidated after the picker or while staging.
        if (hr == S_OK && allowed && !allowed()) hr = S_FALSE;
        // Atomically publish only a new name. Cancellation must not reach the move.
        if (hr == S_OK && !MoveFileExW(tmp, path, MOVEFILE_WRITE_THROUGH))
            hr = HRESULT_FROM_WIN32(GetLastError());
        if (hr != S_OK) DeleteFileW(tmp);
    }
    free(tmp);
    GlobalUnlock(buffer);
    return hr;
}

static HRESULT pick_png_path(HWND owner, wchar_t **path) {
    IFileSaveDialog *dialog = NULL;
    IShellItem *item = NULL;
    const COMDLG_FILTERSPEC types[] = {{L"PNG 图片", L"*.png"}};
    HRESULT hr = CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IFileSaveDialog, (void **)&dialog);
    // On redirected drives the shell's write probe can leave an empty target.
    // Only write_png may create the target, via its no-replace publish.
    if (hr == S_OK) hr = IFileSaveDialog_SetOptions(dialog,
        FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOTESTFILECREATE);
    if (hr == S_OK) hr = IFileSaveDialog_SetTitle(dialog, L"另存 PNG（请选择未使用的文件名）");
    if (hr == S_OK) hr = IFileSaveDialog_SetFileTypes(dialog, ARRAYSIZE(types), types);
    if (hr == S_OK) hr = IFileSaveDialog_SetDefaultExtension(dialog, L"png");
    if (hr == S_OK) hr = IFileSaveDialog_SetFileName(dialog, L"拾光截图.png");
    if (hr == S_OK) hr = IFileSaveDialog_Show(dialog, owner);
    if (hr == S_OK) hr = IFileSaveDialog_GetResult(dialog, &item);
    if (hr == S_OK) hr = IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, path);
    if (item) IShellItem_Release(item);
    if (dialog) IFileSaveDialog_Release(dialog);
    return hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? S_FALSE : hr;
}

static HRESULT finish_png_save(HRESULT picked, const wchar_t *path, const Image *image,
                               SaveAllowed allowed) {
    if (picked != S_OK) return picked;
    if (allowed && !allowed()) return S_FALSE;
    IStream *png = NULL;
    HRESULT hr = encode_png(image, &png);
    if (hr == S_OK) hr = write_png(path, png, allowed);
    if (png) IStream_Release(png);
    return hr;
}

HRESULT save_png_dialog(HWND owner, const Image *image, SaveAllowed allowed) {
    wchar_t *path = NULL;
    HRESULT picked = pick_png_path(owner, &path);
    HRESULT result = finish_png_save(picked, path, image, allowed);
    CoTaskMemFree(path);
    return result;
}

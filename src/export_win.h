#ifndef SHIGUANG_EXPORT_WIN_H
#define SHIGUANG_EXPORT_WIN_H
#include <windows.h>
#include "crop.h"

BOOL publish_image(HWND owner, const Image *image);
typedef BOOL (*SaveAllowed)(void);
// S_FALSE means cancelled or no longer allowed; the target was not published.
HRESULT save_png_dialog(HWND owner, const Image *image, SaveAllowed allowed);
#endif

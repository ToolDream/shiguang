#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <string.h>
#include <wchar.h>

#define URL_CAP 8192
#define PREFIX(s) (wcsncmp(url, L##s, (sizeof(L##s) / sizeof(wchar_t)) - 1) == 0)

static int origin(const wchar_t *url, int initial, int api) {
    if (initial) {
        if (wcscmp(url, L"https://api.github.com/repos/ToolDream/shiguang/releases?per_page=30") == 0 ||
            wcscmp(url, L"https://api.github.com/repos/ToolDream/shiguang/releases/latest") == 0 ||
            wcscmp(url, L"https://api.github.com/repos/ToolDream/shiguang/releases/tags/dev-latest") == 0)
            return 1;
        if (PREFIX("https://github.com/ToolDream/shiguang/releases/download/")) return 2;
    } else if (!api && (PREFIX("https://release-assets.githubusercontent.com/") ||
                        PREFIX("https://objects.githubusercontent.com/"))) {
        return 2;
    }
    return 0;
}

static int safe_url(const wchar_t *url) {
    if (!*url) return 0;
    for (; *url; ++url)
        if (*url <= 32 || *url >= 127 || *url == L'\\' || *url == L'#' || *url == L'@')
            return 0;
    return 1;
}

static int before(ULONGLONG limit) { return GetTickCount64() < limit; }

int sg_update_get(const char *utf8, void *data, int capacity, int milliseconds, int *size) {
    if (capacity <= 0 || milliseconds <= 0 || milliseconds > 30000) return 0;
    wchar_t url[URL_CAP], host[256], path[URL_CAP];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, url, URL_CAP)) return 0;
    int api = origin(url, 1, 0) == 1;
    ULONGLONG limit = GetTickCount64() + (ULONGLONG)milliseconds;
    HINTERNET session = WinHttpOpen(L"Shiguang-Updater/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return 0;
    int ok = 0;
    *size = 0;
    int timeout = milliseconds < 4000 ? milliseconds : 4000;
    if (!WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout)) goto done;
    for (int hop = 0; hop < 3 && before(limit); ++hop) {
        URL_COMPONENTS parsed = {0};
        parsed.dwStructSize = sizeof parsed;
        parsed.lpszHostName = host;
        parsed.dwHostNameLength = sizeof host / sizeof *host;
        parsed.dwUrlPathLength = (DWORD)-1;
        parsed.dwExtraInfoLength = (DWORD)-1;
        if (!safe_url(url) || !origin(url, hop == 0, api) ||
            !WinHttpCrackUrl(url, 0, 0, &parsed) || parsed.nScheme != INTERNET_SCHEME_HTTPS ||
            parsed.nPort != INTERNET_DEFAULT_HTTPS_PORT ||
            (size_t)parsed.dwUrlPathLength + parsed.dwExtraInfoLength >= URL_CAP)
            break;
        memcpy(path, parsed.lpszUrlPath, parsed.dwUrlPathLength * sizeof *path);
        if (parsed.dwExtraInfoLength)
            memcpy(path + parsed.dwUrlPathLength, parsed.lpszExtraInfo,
                   parsed.dwExtraInfoLength * sizeof *path);
        path[parsed.dwUrlPathLength + parsed.dwExtraInfoLength] = 0;
        HINTERNET connection = WinHttpConnect(session, host, parsed.nPort, 0);
        if (!connection) break;
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", path, NULL,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        int redirect = 0;
        if (request) {
            DWORD disable = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES |
                            WINHTTP_DISABLE_AUTHENTICATION;
            DWORD status = 0, bytes = sizeof status;
            if (WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof disable) &&
                before(limit) && WinHttpSendRequest(request,
                    api ? L"Accept: application/vnd.github+json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS,
                    api ? (DWORD)-1 : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                before(limit) && WinHttpReceiveResponse(request, NULL) &&
                WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX)) {
                if (status == 200) {
                    while (*size < capacity && before(limit)) {
                        DWORD got = 0, room = (DWORD)(capacity - *size);
                        if (!WinHttpReadData(request, (char *)data + *size,
                                             room > 65536 ? 65536 : room, &got)) break;
                        if (!got) { ok = before(limit); break; }
                        *size += (int)got;
                    }
                } else if (status == 404 && api &&
                           (wcscmp(url, L"https://api.github.com/repos/ToolDream/shiguang/releases/latest") == 0 ||
                            wcscmp(url, L"https://api.github.com/repos/ToolDream/shiguang/releases/tags/dev-latest") == 0)) {
                    ok = before(limit);
                } else if (!api && (status == 301 || status == 302 || status == 303 ||
                                    status == 307 || status == 308)) {
                    bytes = sizeof url;
                    redirect = WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                        WINHTTP_HEADER_NAME_BY_INDEX, url, &bytes, WINHTTP_NO_HEADER_INDEX);
                }
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connection);
        if (!redirect) break;
    }
done:
    WinHttpCloseHandle(session);
    return ok;
}

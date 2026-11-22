#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#define CAP 8192
#define CHECK_SIZE (64u * 1024u * 1024u)

typedef struct { unsigned count; HANDLE handles[128]; } Pins;

static int full_path(const wchar_t *input, wchar_t path[CAP]) {
    DWORD n = GetFullPathNameW(input, CAP, path, NULL);
    return n >= 3 && n < CAP && path[1] == L':' && path[2] == L'\\' &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'));
}

static int metadata(HANDLE h, BY_HANDLE_FILE_INFORMATION *info, int directory) {
    if (h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h, info)) return 0;
    if ((info->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !!(info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory) {
        SetLastError(ERROR_INVALID_DATA);
        return 0;
    }
    return 1;
}

static int is_dos_path(HANDLE h, const wchar_t *wanted) {
    wchar_t resolved[CAP + 4];
    DWORD n = GetFinalPathNameByHandleW(h, resolved, CAP + 4, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    return n >= 7 && n < CAP + 4 && wcsncmp(resolved, L"\\\\?\\", 4) == 0 &&
           _wcsicmp(resolved + 4, wanted) == 0;
}

void sg_update_unpin(void *p) {
    Pins *pins = p;
    if (!pins) return;
    while (pins->count) CloseHandle(pins->handles[--pins->count]);
    HeapFree(GetProcessHeap(), 0, pins);
}

void *sg_update_pin(const wchar_t *input) {
    wchar_t path[CAP];
    if (!full_path(input, path)) return NULL;
    Pins *pins = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *pins);
    if (!pins) return NULL;
    for (wchar_t *end = path + 3;; ++end) {
        if (*end && *end != L'\\' && end != path + 3) continue;
        if (end == path + 3 && !*end && pins->count) break;
        wchar_t save = *end;
        *end = 0;
        // Attribute-only handles still let NTFS detach a pinned name.
        HANDLE h = CreateFileW(path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
        BY_HANDLE_FILE_INFORMATION info;
        if (!metadata(h, &info, 1) || !is_dos_path(h, path) || pins->count == sizeof pins->handles / sizeof *pins->handles) {
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            sg_update_unpin(pins);
            return NULL;
        }
        pins->handles[pins->count++] = h;
        *end = save;
        if (!save) break;
    }
    return pins;
}

static int same_file(const BY_HANDLE_FILE_INFORMATION *a, const BY_HANDLE_FILE_INFORMATION *b) {
    return a->dwVolumeSerialNumber == b->dwVolumeSerialNumber &&
        a->nFileIndexLow == b->nFileIndexLow && a->nFileIndexHigh == b->nFileIndexHigh;
}

int sg_update_directory_id(const wchar_t *dir, wchar_t *out, size_t capacity) {
    Pins *pins = sg_update_pin(dir);
    BY_HANDLE_FILE_INFORMATION id;
    int ok = pins && metadata(pins->handles[pins->count - 1], &id, 1) &&
        swprintf(out, capacity, L"%08lx-%08lx%08lx",
            (unsigned long)id.dwVolumeSerialNumber, (unsigned long)id.nFileIndexHigh,
            (unsigned long)id.nFileIndexLow) == 25;
    sg_update_unpin(pins);
    return ok;
}

static int gate_writer(HANDLE handle) {
    LARGE_INTEGER start = {0};
    DWORD count = 0;
    /* A handle opened only for attributes can report the same file ID, but
       cannot write this byte. A second writer cannot open the active gate. */
    return SetFilePointerEx(handle, start, NULL, FILE_BEGIN) &&
           WriteFile(handle, "1", 1, &count, NULL) && count == 1;
}

/* The exclusive write access survives handoff because every process inherits
   the same kernel file object, not a newly opened handle. No path-name gate. */
uintptr_t sg_update_enter(const wchar_t *data, const wchar_t *install, uintptr_t inherited) {
    Pins *root = sg_update_pin(install), *store = sg_update_pin(data);
    uintptr_t result = 0;
    if (root && store) {
        BY_HANDLE_FILE_INFORMATION id, a, b;
        wchar_t path[CAP];
        HANDLE dir = root->handles[root->count - 1];
        if (metadata(dir, &id, 1) &&
            swprintf(path, CAP, L"%ls\\gate-%08lx-%08lx%08lx.lock", data,
                (unsigned long)id.dwVolumeSerialNumber, (unsigned long)id.nFileIndexHigh,
                (unsigned long)id.nFileIndexLow) > 0) {
            SECURITY_ATTRIBUTES security = {sizeof security, NULL, TRUE};
            DWORD access = inherited ? FILE_READ_ATTRIBUTES : GENERIC_READ | GENERIC_WRITE;
            HANDLE h = CreateFileW(path, access, FILE_SHARE_READ | (inherited ? FILE_SHARE_WRITE : 0),
                inherited ? NULL : &security, inherited ? OPEN_EXISTING : OPEN_ALWAYS,
                FILE_FLAG_OPEN_REPARSE_POINT, NULL);
            if (metadata(h, &a, 0) && a.nNumberOfLinks == 1 &&
                (!inherited || (metadata((HANDLE)inherited, &b, 0) && same_file(&a, &b))) &&
                gate_writer(inherited ? (HANDLE)inherited : h)) {
                result = inherited ? inherited : (uintptr_t)h;
            }
            if (h != INVALID_HANDLE_VALUE && result != (uintptr_t)h) CloseHandle(h);
        }
    }
    sg_update_unpin(store);
    sg_update_unpin(root);
    return result;
}

int sg_update_write_new(const wchar_t *path, const void *bytes, DWORD length) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD count = 0;
    int ok = WriteFile(h, bytes, length, &count, NULL) && count == length && FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

extern int sg_update_sha256(const unsigned char *, uint32_t, unsigned char *);

static HANDLE open_verified(const wchar_t *source, const unsigned char *hash,
                            DWORD expected_size, DWORD access) {
    HANDLE h = CreateFileW(source, GENERIC_READ | access, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    if (metadata(h, &info, 0) && info.nNumberOfLinks == 1 && !info.nFileSizeHigh &&
        info.nFileSizeLow && info.nFileSizeLow <= CHECK_SIZE &&
        (!expected_size || info.nFileSizeLow == expected_size)) {
        unsigned char *bytes = HeapAlloc(GetProcessHeap(), 0, info.nFileSizeLow);
        if (bytes) {
            unsigned char actual[32];
            DWORD read;
            int ok = ReadFile(h, bytes, info.nFileSizeLow, &read, NULL) &&
                read == info.nFileSizeLow && sg_update_sha256(bytes, read, actual) &&
                memcmp(actual, hash, sizeof actual) == 0;
            HeapFree(GetProcessHeap(), 0, bytes);
            if (ok) return h;
        }
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return INVALID_HANDLE_VALUE;
}

uintptr_t sg_update_hold_verified(const wchar_t *file, const unsigned char *hash, DWORD size) {
    HANDLE h = open_verified(file, hash, size, 0);
    return h == INVALID_HANDLE_VALUE ? 0 : (uintptr_t)h;
}

static int rename_handle(HANDLE h, const wchar_t *destination) {
    wchar_t full[CAP];
    if (!full_path(destination, full)) return 0;
    DWORD n = (DWORD)(wcslen(full) * sizeof *full);
    DWORD size = (DWORD)offsetof(FILE_RENAME_INFO, FileName) + n + sizeof *full;
    FILE_RENAME_INFO *op = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    if (!op) return 0;
    op->FileNameLength = n;
    memcpy(op->FileName, full, n);
    int ok = SetFileInformationByHandle(h, FileRenameInfo, op, size) != 0;
    HeapFree(GetProcessHeap(), 0, op);
    return ok;
}

int sg_update_move_verified(const wchar_t *source, const wchar_t *destination,
                            const unsigned char *hash) {
    HANDLE h = open_verified(source, hash, 0, DELETE);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = rename_handle(h, destination);
    CloseHandle(h);
    return ok;
}

int sg_update_move_owned_dir(const wchar_t *source, const wchar_t *destination,
                            const wchar_t *expected) {
    wchar_t id[26];
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE h = CreateFileW(source, FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = metadata(h, &info, 1) &&
        swprintf(id, 26, L"%08lx-%08lx%08lx", (unsigned long)info.dwVolumeSerialNumber,
            (unsigned long)info.nFileIndexHigh, (unsigned long)info.nFileIndexLow) == 25 &&
        wcscmp(id, expected) == 0 && rename_handle(h, destination);
    CloseHandle(h);
    return ok;
}

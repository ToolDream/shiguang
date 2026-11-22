#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0a00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <wchar.h>

static int quoted(wchar_t *dest, size_t cap, const wchar_t *value) {
    size_t n = wcslen(value), trailing = 0;
    if (n >= 8192 || n + 3 > cap || wcspbrk(value, L"\"\r\n")) return 0;
    while (trailing < n && value[n - trailing - 1] == L'\\') ++trailing;
    if (n + trailing + 3 > cap) return 0;
    *dest++ = L'"';
    memcpy(dest, value, n * sizeof *dest);
    wmemset(dest + n, L'\\', trailing);
    dest[n + trailing] = L'"';
    dest[n + trailing + 1] = 0;
    return 1;
}

static HANDLE launch(const wchar_t *exe, const wchar_t *cwd, const wchar_t *command_args,
                     HANDLE handles[], size_t count) {
    wchar_t command[32768];
    if (!quoted(command, sizeof command / sizeof *command, exe)) return NULL;
    size_t count_chars = wcslen(command);
    if (wcslen(command_args) + count_chars + 1 >= sizeof command / sizeof *command) return NULL;
    wcscpy(command + count_chars, command_args);
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    if (!bytes) return NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST list = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!list) return NULL;
    HANDLE process = NULL;
    if (InitializeProcThreadAttributeList(list, 1, 0, &bytes)) {
        STARTUPINFOEXW startup = {0};
        startup.StartupInfo.cb = sizeof startup;
        startup.lpAttributeList = list;
        PROCESS_INFORMATION child;
        if (UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      handles, count * sizeof *handles, NULL, NULL) &&
            CreateProcessW(exe, command, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                           NULL, cwd, &startup.StartupInfo, &child)) {
            CloseHandle(child.hThread);
            process = child.hProcess;
        }
        DeleteProcThreadAttributeList(list);
    }
    HeapFree(GetProcessHeap(), 0, list);
    return process;
}


void sg_update_close(uintptr_t handle) { if (handle) CloseHandle((HANDLE)handle); }

int sg_update_run_helper(const wchar_t *helper, const wchar_t *work, const wchar_t *dir,
                         uintptr_t gate) {
    if (!gate) return 0;
    HANDLE parent = OpenProcess(SYNCHRONIZE, TRUE, GetCurrentProcessId());
    HANDLE child = NULL;
    if (parent) {
        wchar_t target[16384], args[16600];
        if (quoted(target, sizeof target / sizeof *target, dir) &&
            swprintf(args, sizeof args / sizeof *args,
                L" --install %llu %llu %ls", (unsigned long long)(uintptr_t)parent,
                (unsigned long long)gate, target) >= 0) {
            HANDLE inherited[] = {(HANDLE)gate, parent};
            child = launch(helper, work, args, inherited, 2);
        }
        CloseHandle(parent);
    }
    if (!child) return 0;
    CloseHandle(child);
    return 1;
}

int sg_update_wait_parent(uintptr_t parent) {
    HANDLE handle = (HANDLE)parent;
    DWORD result = WaitForSingleObject(handle, 45000);
    CloseHandle(handle);
    return result == WAIT_OBJECT_0;
}

int sg_update_start_child(const wchar_t *exe, const wchar_t *dir, uintptr_t gate,
                          uintptr_t *proc, uintptr_t *ready, uintptr_t *go, uintptr_t *ack) {
    SECURITY_ATTRIBUTES security = {sizeof security, NULL, TRUE};
    HANDLE r = CreateEventW(&security, TRUE, FALSE, NULL);
    HANDLE g = CreateEventW(&security, TRUE, FALSE, NULL);
    HANDLE a = CreateEventW(&security, TRUE, FALSE, NULL);
    HANDLE child = NULL;
    if (r && g && a && gate) {
        wchar_t args[160];
        if (swprintf(args, sizeof args / sizeof *args,
            L" --update-ready %llu %llu %llu %llu",
            (unsigned long long)(uintptr_t)r, (unsigned long long)(uintptr_t)g,
            (unsigned long long)(uintptr_t)a, (unsigned long long)gate) >= 0) {
            HANDLE inherited[] = {r, g, a, (HANDLE)gate};
            child = launch(exe, dir, args, inherited, 4);
        }
    }
    if (!child) {
        if (r) CloseHandle(r);
        if (g) CloseHandle(g);
        if (a) CloseHandle(a);
        return 0;
    }
    *proc = (uintptr_t)child;
    *ready = (uintptr_t)r;
    *go = (uintptr_t)g;
    *ack = (uintptr_t)a;
    return 1;
}

static int await_child(uintptr_t proc, uintptr_t event, DWORD timeout) {
    HANDLE wait[] = {(HANDLE)proc, (HANDLE)event};
    return WaitForMultipleObjects(2, wait, FALSE, timeout) == WAIT_OBJECT_0 + 1 &&
           WaitForSingleObject((HANDLE)proc, 0) == WAIT_TIMEOUT;
}

int sg_update_child_ready(uintptr_t proc, uintptr_t ready) { return await_child(proc, ready, 15000); }
int sg_update_child_ack(uintptr_t proc, uintptr_t ack) { return await_child(proc, ack, 10000); }
int sg_update_child_alive(uintptr_t proc) { return WaitForSingleObject((HANDLE)proc, 0) == WAIT_TIMEOUT; }
int sg_update_release_child(uintptr_t go) { return SetEvent((HANDLE)go) != 0; }

int sg_update_child_finish(uintptr_t proc, uintptr_t ready, uintptr_t go, uintptr_t ack, int commit) {
    int ok = 1;
    if (!commit) {
        if (WaitForSingleObject((HANDLE)proc, 0) == WAIT_TIMEOUT) TerminateProcess((HANDLE)proc, 1);
        ok = WaitForSingleObject((HANDLE)proc, 5000) == WAIT_OBJECT_0;
    }
    CloseHandle((HANDLE)ack);
    CloseHandle((HANDLE)go);
    CloseHandle((HANDLE)ready);
    CloseHandle((HANDLE)proc);
    return ok;
}

int sg_update_startup_ready(uintptr_t ready, uintptr_t go) {
    HANDLE r = (HANDLE)ready, g = (HANDLE)go;
    int ok = SetEvent(r) && WaitForSingleObject(g, 20000) == WAIT_OBJECT_0;
    CloseHandle(r);
    CloseHandle(g);
    return ok;
}

int sg_update_startup_ack(uintptr_t ack) {
    int ok = SetEvent((HANDLE)ack) != 0;
    CloseHandle((HANDLE)ack);
    return ok;
}

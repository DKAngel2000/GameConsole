// CrashProbe.c — verifies the native UEH records a crash into hooks.log.
#include <windows.h>
#include <stdio.h>
int wmain(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;
    void (__cdecl* hooks)(int, int, int, int, int, const wchar_t*, const wchar_t*) =
        (void (__cdecl*)(int, int, int, int, int, const wchar_t*, const wchar_t*))(void*)
        GetProcAddress(LoadLibraryW(swprintf(path, MAX_PATH, L"%s\\GameHooks.dll", dir) ? path : path), "GCInstallHooks");
    void (__cdecl* veh)(int, int) = (void (__cdecl*)(int, int))(void*)
        GetProcAddress(GetModuleHandleW(L"GameHooks.dll"), "GCInstallVeh");
    if (hooks) hooks(0, 0, 0, 0, 100, L"", (swprintf(path, MAX_PATH, L"%s\\probe_hooks.log", dir), path));
    if (veh) veh(1, 0);
    *(volatile int*)0 = 1;   // unhandled AV: UEH records, process dies
    return 0;
}

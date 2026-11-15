// StressTest.c — multithreaded host for GameHooks.dll / GameConsole stress
// testing. Loads Bootstrap.dll (which auto-starts GameConsole.dll), then
// hammers the hooked APIs from many threads: file creates/reads/writes and
// NtProtectVirtualMemory churn. Verifies the native hook layer survives
// concurrency (the managed version crashed here in-game).
//
// Usage: StressTest.exe [seconds]   (default 12)
#include <windows.h>
#include <stdio.h>

static volatile LONG g_stop = 0;

static DWORD WINAPI Hammer(LPVOID n)
{
    int id = (int)(INT_PTR)n;
    wchar_t path[300];
    swprintf(path, 300, L"stress_t%d.dat", id);
    char buf[64];
    DWORD w;
    while (!InterlockedCompareExchange(&g_stop, 0, 0))
    {
        // create + write + read + protect churn, via the CRT-less Win32 path
        HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE)
        {
            WriteFile(f, buf, sizeof(buf), &w, NULL);
            SetFilePointer(f, 0, NULL, FILE_BEGIN);
            ReadFile(f, buf, sizeof(buf), &w, NULL);
            // flip page protection (execute involved → hook logs it)
            SYSTEM_INFO si; GetSystemInfo(&si);
            DWORD old;
            VirtualProtect((BYTE*)((uintptr_t)path & ~(uintptr_t)(si.dwPageSize - 1)), si.dwPageSize,
                           PAGE_EXECUTE_READWRITE, &old);
            CloseHandle(f);
        }
        DeleteFileW(path);
        Sleep(2);
    }
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    int secs = argc > 1 ? _wtoi(argv[1]) : 12;
    wchar_t dir[MAX_PATH], bpath[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;
    swprintf(bpath, MAX_PATH, L"%s\\Bootstrap.dll", dir);
    HMODULE b = LoadLibraryW(bpath);
    wprintf(L"bootstrap loaded: %p\n", (void*)b);
    Sleep(3000);   // let the worker install hooks first

    wprintf(L"starting 6 hammer threads for %d s...\n", secs);
    HANDLE th[6];
    for (int i = 0; i < 6; i++) th[i] = CreateThread(NULL, 0, Hammer, (LPVOID)(INT_PTR)i, 0, NULL);
    Sleep(secs * 1000);
    InterlockedExchange(&g_stop, 1);
    WaitForMultipleObjects(6, th, TRUE, 5000);
    for (int i = 0; i < 6; i++) CloseHandle(th[i]);

    // exercise the native VEH capture: ODS + a first-chance access violation
    OutputDebugStringA("stress test ODS line");
    __try { *(volatile int*)0 = 42; } __except (EXCEPTION_EXECUTE_HANDLER) { }
    wprintf(L"raised ODS + handled AV; waiting 1s for drain...\n");
    Sleep(1000);
    wprintf(L"stress done, exiting\n");
    return 0;
}

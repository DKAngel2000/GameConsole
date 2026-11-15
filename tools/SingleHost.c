// SingleHost.c - test host for the single-DLL GameConsole.dll.
//
// The game folder will contain ONE dll (GameConsole.dll) that self-starts from
// DllMain; artifact.json's load_dlls just names it. This host mimics that: it
// LoadLibraryW's GameConsole.dll from its own folder and then exercises the
// hooked APIs plus ODS and a first-chance exception so the whole pipeline can be
// checked without launching the game.
//
// Build: cl /nologo /O2 /W3 SingleHost.c /Fe:SingleHost.exe
#include <windows.h>
#include <stdio.h>

static volatile LONG g_stop = 0;

static DWORD WINAPI Hammer(LPVOID n)
{
    int id = (int)(INT_PTR)n;
    wchar_t path[300];
    swprintf(path, 300, L"singletest_t%d.dat", id);
    char buf[64];
    DWORD w;
    while (!InterlockedCompareExchange(&g_stop, 0, 0))
    {
        HANDLE f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE)
        {
            WriteFile(f, buf, sizeof(buf), &w, NULL);
            SetFilePointer(f, 0, NULL, FILE_BEGIN);
            ReadFile(f, buf, sizeof(buf), &w, NULL);
            DWORD old;
            VirtualProtect((void*)&g_stop, sizeof(g_stop), PAGE_EXECUTE_READWRITE, &old);
            VirtualProtect((void*)&g_stop, sizeof(g_stop), old, &old);
            CloseHandle(f);
        }
        DeleteFileW(path);
        Sleep(3);
    }
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    int secs = argc > 1 ? _wtoi(argv[1]) : 10;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;
    swprintf(path, MAX_PATH, L"%s\\GameConsole.dll", dir);

    HMODULE gc = LoadLibraryW(path);   // self-starts via DllMain
    wprintf(L"GameConsole.dll loaded: %p\n", (void*)gc);
    if (!gc) { wprintf(L"load failed: %lu\n", GetLastError()); return 1; }

    wprintf(L"starting 4 hammer threads + ODS/exception probes for %d s\n", secs);
    HANDLE th[4];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, Hammer, (LPVOID)(INT_PTR)i, 0, NULL);
    Sleep(secs * 1000);
    InterlockedExchange(&g_stop, 1);
    WaitForMultipleObjects(4, th, TRUE, 5000);
    for (int i = 0; i < 4; i++) CloseHandle(th[i]);

    OutputDebugStringA("single host ODS line");
    __try { *(volatile int*)0 = 7; } __except (EXCEPTION_EXECUTE_HANDLER) { }
    wprintf(L"probes done, waiting for drain...\n");
    Sleep(1000);
    wprintf(L"done\n");
    return 0;
}

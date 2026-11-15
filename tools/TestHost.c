// Test host: mimics artifact.dll's load_dlls behavior - LoadLibrary only,
// no Init call. GameConsole should start via Bootstrap.dll's DllMain.
#include <stdio.h>
#include <windows.h>
int wmain(int argc, wchar_t** argv)
{
    (void)argc; (void)argv;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0;
    swprintf(path, MAX_PATH, L"%s\\Bootstrap.dll", dir);
    HMODULE b = LoadLibraryW(path);
    wprintf(L"bootstrap loaded: %p\n", (void*)b);
    wprintf(L"waiting 8s so the worker + hooks run...\n");
    Sleep(8000);
    // cause a file read from THIS module to see if hooks log it
    HANDLE f = CreateFileW(L"hooktest.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) { char buf[16]; DWORD rd; ReadFile(f, buf, 16, &rd, NULL); CloseHandle(f); }
    wprintf(L"did a test file read. waiting 3s...\n");
    Sleep(3000);
    wprintf(L"done\n");
    return 0;
}

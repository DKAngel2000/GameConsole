// Resolve <image>+<hex-offset> to a symbol using the image's PDB.
// Usage: symlookup.exe <path-to-dll-or-exe> <hex-offset>
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#pragma comment(lib, "dbghelp.lib")

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { wprintf(L"usage: symlookup <image> <hexOffset>\n"); return 1; }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
    if (!SymInitializeW(GetCurrentProcess(), NULL, FALSE)) { wprintf(L"SymInitialize failed %lu\n", GetLastError()); return 1; }
    DWORD64 base = SymLoadModuleExW(GetCurrentProcess(), NULL, argv[1], NULL, 0, 0, NULL, 0);
    if (!base) { wprintf(L"SymLoadModuleEx failed %lu\n", GetLastError()); return 1; }
    DWORD64 addr = base + wcstoull(argv[2], NULL, 16);
    wchar_t buf[sizeof(SYMBOL_INFOW) + 512] = { 0 };
    SYMBOL_INFOW* si = (SYMBOL_INFOW*)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFOW);
    si->MaxNameLen = 250;
    DWORD64 disp = 0;
    if (SymFromAddrW(GetCurrentProcess(), addr, &disp, si))
        wprintf(L"SYMBOL: %s + 0x%llx\n", si->Name, (unsigned long long)disp);
    else
        wprintf(L"no symbol, err=%lu\n", GetLastError());
    IMAGEHLP_LINEW64 line = { 0 }; line.SizeOfStruct = sizeof(line);
    DWORD disp2 = 0;
    if (SymGetLineFromAddrW64(GetCurrentProcess(), addr, &disp2, &line))
        wprintf(L"LINE: %s:%lu\n", line.FileName, line.LineNumber);
    return 0;
}

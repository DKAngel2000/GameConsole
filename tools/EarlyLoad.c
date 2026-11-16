// tools/EarlyLoad.c - start the game with GameConsole.dll already loaded, so the
// log begins at instruction zero instead of ~4.8 s in.
//
// Why: the elevated PB0 run showed the protection's whole startup inside the
// first 1.6 s of the process - RTCore64.sys loaded at +0.45 s, the two watchdog
// helpers (`rundll32 "…\dse.dll",DseWatchdog 724 0 1` and
// `rundll32 "…\artifact.dll",Watchdog <gamepid>`) at +0.75 s, the three
// hypervisor images and the DSE restore at +1.0 s, service `artifact` RUNNING at
// +1.6 s - while the loader only brought GameConsole.dll in at +4.76 s. Nothing
// in-process can see the SCM calls or the DSE window from that position, and the
// loader's ordering is not ours to change.
//
// This does not modify a single file: it creates the process SUSPENDED, calls
// LoadLibraryW in it from a remote thread, waits for it, and only then resumes the
// first thread. Their loader still runs afterwards and loads the same DLL again -
// LoadLibrary on an already-loaded module just bumps a refcount, so the DLL, its
// hooks and its worker thread are all in place before the first instruction of
// the game executes.
//
// Build:
//   cl /nologo /O2 /W3 EarlyLoad.c /Fe:EarlyLoad.exe
//
// Use (the game runs elevated, so run this from an elevated prompt):
//   EarlyLoad.exe "F:\5068070\platform\steam\PBZ.exe"
//   EarlyLoad.exe -dll "F:\...\GameConsole.dll" "F:\...\PlanetZoo2.exe" -somearg
//
// Without -dll it uses GameConsole.dll next to the target exe, else next to this
// tool. Exit code is the game's exit code.

#include <windows.h>
#include <stdio.h>

static void fail(const WCHAR* what)
{
    wprintf(L"EarlyLoad: %s failed (error %lu)\n", what, GetLastError());
}

int wmain(int argc, wchar_t** argv)
{
    WCHAR cmd[4096], dll[MAX_PATH * 2], exeDir[MAX_PATH];
    SIZE_T dllLen;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    LPVOID remote;
    SIZE_T written = 0;
    HANDLE th;
    DWORD wr = 0, exitCode = 0;
    HMODULE k32;
    FARPROC loadLib;
    int i, first = 1;

    if (argc < 2)
    {
        wprintf(L"usage: EarlyLoad.exe [-dll <GameConsole.dll>] <game.exe> [args...]\n");
        return 2;
    }
    dll[0] = 0;
    cmd[0] = 0;
    for (i = 1; i < argc; i++)
    {
        if (!_wcsicmp(argv[i], L"-dll") && i + 1 < argc) { wcsncpy_s(dll, _countof(dll), argv[++i], _TRUNCATE); continue; }
        if (cmd[0]) wcscat_s(cmd, _countof(cmd), L" ");
        if (!first || wcschr(argv[i], L' '))
        {
            wcscat_s(cmd, _countof(cmd), L"\"");
            wcscat_s(cmd, _countof(cmd), argv[i]);
            wcscat_s(cmd, _countof(cmd), L"\"");
        }
        else wcscat_s(cmd, _countof(cmd), argv[i]);
        first = 0;
    }
    if (!cmd[0]) { wprintf(L"EarlyLoad: no target exe\n"); return 2; }

    if (!dll[0])
    {
        WCHAR* slash;
        wcsncpy_s(exeDir, _countof(exeDir), cmd, _TRUNCATE);
        if (exeDir[0] == L'"') { memmove(exeDir, exeDir + 1, wcslen(exeDir)); }
        slash = wcsrchr(exeDir, L'\\');
        if (slash) *slash = 0;
        _snwprintf_s(dll, _countof(dll), _TRUNCATE, L"%s\\GameConsole.dll", exeDir);
        if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES)
        {
            GetModuleFileNameW(NULL, dll, _countof(dll));
            slash = wcsrchr(dll, L'\\');
            if (slash) *(slash + 1) = 0;
            wcscat_s(dll, _countof(dll), L"GameConsole.dll");
        }
    }
    dllLen = (wcslen(dll) + 1) * sizeof(WCHAR);
    wprintf(L"EarlyLoad: target  %s\n", cmd);
    wprintf(L"EarlyLoad: inject  %s\n", dll);
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES)
    {
        wprintf(L"EarlyLoad: DLL not found - aborting (the game would run uninstrumented)\n");
        return 2;
    }

    k32 = GetModuleHandleW(L"kernel32.dll");
    loadLib = GetProcAddress(k32, "LoadLibraryW");
    if (!loadLib) { fail(L"GetProcAddress(LoadLibraryW)"); return 1; }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                        NULL, NULL, &si, &pi))
    {
        fail(L"CreateProcessW");
        return 1;
    }
    wprintf(L"EarlyLoad: created pid %lu suspended\n", pi.dwProcessId);

    remote = VirtualAllocEx(pi.hProcess, NULL, dllLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { fail(L"VirtualAllocEx"); TerminateProcess(pi.hProcess, 1); return 1; }
    if (!WriteProcessMemory(pi.hProcess, remote, dll, dllLen, &written) || written != dllLen)
    {
        fail(L"WriteProcessMemory"); TerminateProcess(pi.hProcess, 1); return 1;
    }
    th = CreateRemoteThread(pi.hProcess, NULL, 0,
                            (LPTHREAD_START_ROUTINE)loadLib, remote, 0, NULL);
    if (!th) { fail(L"CreateRemoteThread"); TerminateProcess(pi.hProcess, 1); return 1; }
    WaitForSingleObject(th, 30000);
    GetExitCodeThread(th, &wr);
    CloseHandle(th);
    VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
    wprintf(L"EarlyLoad: LoadLibraryW returned 0x%08lX%s\n", wr,
            wr ? L" (loaded)" : L" (FAILED - the game runs uninstrumented)");

    ResumeThread(pi.hThread);
    wprintf(L"EarlyLoad: main thread resumed\n");
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    wprintf(L"EarlyLoad: pid %lu exited with %lu\n", pi.dwProcessId, exitCode);
    CloseHandle(pi.hProcess);
    return (int)exitCode;
}

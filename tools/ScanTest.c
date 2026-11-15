// ScanTest.c - verifies GameConsole's read-only ntdll hook survey actually
// detects inline hooks. Loads GameConsole.dll (self-starts), then installs TWO
// real FF25 detours on ntdll!NtOpenFile and ntdll!NtQueryInformationFile so
// the 10-second heartbeat rescan must report exactly those two as NEW sites
// "hooked by other module". It also registers a real VEH and UEH from the exe,
// so with HookExe=true the log must show "veh add" / "ueh set" hook lines and
// the handler chain must grow from 1 to 2 with both handlers named.
//
// x64 gotcha this harness must respect: a patched stub can only use FF25 with
// a data slot within +/-2GB of the stub, and rel32 jumps cannot span the ~13GB
// gap between ntdll and this exe. So the slot + the copy-prologue trampoline
// are VirtualAlloc'd within 1GB BELOW ntdll, and the slot holds the absolute
// address of the C handler (which can live anywhere).
// Build: cl /nologo /O2 /W3 ScanTest.c /Fe:ScanTest.exe
#include <windows.h>
#include <stdio.h>
#pragma comment(lib, "advapi32.lib")

static void Step(const char* s)
{
    FILE* f = fopen("progress.txt", "a");
    if (f)
    {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds, s);
        fclose(f);
    }
}

static BYTE*  g_near;        // RWX region near ntdll: slots + trampolines
static void** g_slots;       // g_near + 0:    handler pointers
static BYTE*  g_tramps;      // g_near + 0x1000: copied prologues
static BYTE*  g_trampOpen;
static BYTE*  g_trampQif;

typedef NTSTATUS(NTAPI* t_NtOpenFile)(void*, ULONG, void*, void*, ULONG, ULONG);
typedef NTSTATUS(NTAPI* t_NtQif)(void*, void*, void*, ULONG, int);

static NTSTATUS NTAPI FakeOpen(void* a, ULONG b, void* c, void* d, ULONG e, ULONG f)
{
    return ((t_NtOpenFile)(void*)g_trampOpen)(a, b, c, d, e, f);
}
static NTSTATUS NTAPI FakeQif(void* a, void* b, void* c, ULONG d, int e)
{
    return ((t_NtQif)(void*)g_trampQif)(a, b, c, d, e);
}

// A real VEH and UEH registered from this exe: with HookExe=true the exe's IAT
// is instrumented, so both registrations must show up as [HOOK] veh/ueh lines,
// and GameConsole's chain walk must name this handler in the VEH chain.
static LONG NTAPI TestVeh(EXCEPTION_POINTERS* p) { (void)p; return EXCEPTION_CONTINUE_SEARCH; }
static LONG WINAPI TestUeh(EXCEPTION_POINTERS* p) { (void)p; return EXCEPTION_CONTINUE_SEARCH; }

// Find free RWX memory within 1GB below 'target' (page granularity steps).
static BYTE* AllocNear(void* target, SIZE_T len)
{
    ULONGLONG lo = (ULONGLONG)target - 0x40000000ULL;
    ULONGLONG a = ((ULONGLONG)target - len) & ~0xFFFFULL;
    while (a > lo)
    {
        void* p = VirtualAlloc((void*)(ULONG_PTR)a, len, MEM_RESERVE | MEM_COMMIT,
                               PAGE_EXECUTE_READWRITE);
        if (p) return (BYTE*)p;
        a -= 0x10000;
    }
    return NULL;
}

// FF25 detour: stub jmp -> slot (near ntdll) -> C handler (anywhere).
// Trampoline: original 16 bytes + FF25 abs-jmp back to stub+16.
static int InstallDetour(void* target, void* handler, int which)
{
    BYTE* t = (BYTE*)target;
    BYTE* tramp = g_tramps + (size_t)which * 0x40;
    void** slot = &g_slots[which];
    INT32 d;
    DWORD old, old2;
    if (!g_near) return 0;
    memcpy(tramp, t, 16);
    tramp[16] = 0xFF; tramp[17] = 0x25; *(DWORD*)(tramp + 18) = 0;
    *(void**)(tramp + 22) = t + 16;
    *slot = handler;
    d = (INT32)((BYTE*)slot - (t + 6));          // slot must be +/-2GB of stub
    if ((BYTE*)slot > t + 6 && (ULONGLONG)((BYTE*)slot - (t + 6)) > 0x7FFFFFFFULL) return 0;
    if ((BYTE*)slot < t + 6 && (ULONGLONG)(t + 6 - (BYTE*)slot) > 0x7FFFFFFFULL) return 0;
    VirtualProtect(t, 16, PAGE_EXECUTE_READWRITE, &old);
    t[0] = 0xFF; t[1] = 0x25; *(DWORD*)(t + 2) = d;
    t[14] = 0x90; t[15] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), t, 16);
    VirtualProtect(t, 16, old, &old2);
    return 1;
}

int wmain(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    HMODULE nt;
    BYTE* nearMem;
    int ok1, ok2;
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    { wchar_t* slash = wcsrchr(dir, L'\\'); if (slash) *slash = 0; }
    swprintf(path, MAX_PATH, L"%s\\GameConsole.dll", dir);
    Step("wmain: starting");
    if (!LoadLibraryW(path)) { wprintf(L"GameConsole load failed\n"); Step("wmain: LOAD FAILED"); return 1; }

    Sleep(2500);   // let the console start and do its first survey
    Step("wmain: past initial sleep");
    // register through the exe's own import table (the HookExe target)
    AddVectoredExceptionHandler(1, TestVeh);
    SetUnhandledExceptionFilter(TestUeh);
    Step("wmain: veh+ueh registered");
    // a module that loads AFTER GameConsole: HookModules=version.dll must show
    // "not loaded yet", then "loaded late - instrumented now" on a retry tick
    LoadLibraryW(L"version.dll");
    Step("wmain: version.dll loaded late");
    // registry probe: must show as [HOOK] regkey CREATED + regset with the value
    {
        HKEY k;
        DWORD disp = 0;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\GameConsoleScanTest", 0, NULL, 0,
                            KEY_WRITE, NULL, &k, &disp) == ERROR_SUCCESS)
        {
            const wchar_t* v = L"\\SystemRoot\\System32\\drivers\\hyperkd.sys";
            RegSetValueExW(k, L"ImagePath", 0, REG_SZ, (const BYTE*)v,
                           (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
            RegCloseKey(k);
            Step("wmain: registry probe written");
        }
    }
    nt = GetModuleHandleW(L"ntdll.dll");
    nearMem = AllocNear(nt, 0x20000);
    if (!nearMem) { Step("wmain: no alloc near ntdll"); return 1; }
    g_near = nearMem; g_slots = (void**)nearMem; g_tramps = nearMem + 0x1000;
    g_trampOpen = g_tramps + 0x00; g_trampQif = g_tramps + 0x40;
    // Call each resolved pointer once BEFORE patching: with GpaThunks=true the
    // pointer is a logging thunk, so this feeds the gpa counters (call + last
    // caller must show ScanTest.exe). The calls fail harmlessly in-kernel
    // (invalid handle/parameters) - nothing user-mode faults.
    { void* gp = (void*)GetProcAddress(nt, "NtOpenFile"); if (gp) ((t_NtOpenFile)gp)(NULL, 0, NULL, NULL, 0, 0); }
    { void* gp = (void*)GetProcAddress(nt, "NtQueryInformationFile"); if (gp) ((t_NtQif)gp)(NULL, NULL, NULL, 0, 0); }
    ok1 = InstallDetour((void*)GetProcAddress(nt, "NtOpenFile"), (void*)FakeOpen, 0);
    ok2 = InstallDetour((void*)GetProcAddress(nt, "NtQueryInformationFile"), (void*)FakeQif, 1);
    { char b[80]; sprintf(b, "wmain: detours installed ok1=%d ok2=%d", ok1, ok2); Step(b); }
    Sleep(13000);   // heartbeat rescan (10 s) discovers them as NEW sites
    Step("wmain: returning");
    return 0;
}

// tools/HostWatcher.c - watch the protection driver being loaded and unloaded,
// and the driver-signature-enforcement flip that goes with it, from *outside*
// the game.
//
// Why this exists: the hypervisor driver is started before the game process
// exists and stopped after it exits (user-confirmed, and consistent with every
// game log so far: each run sees `[SVC] installed driver: artifact ... currently
// LOADED` in its first second and nothing else). A DLL inside the game can only
// ever report the already-loaded state - the SCM calls and the DSE transition
// happen in another process, at a time it cannot observe. Only a watcher that is
// already running can see them.
//
// Everything is read-only: two NtQuerySystemInformation classes, SCM enumeration
// plus one QueryServiceConfig per newly seen driver service, and console/file
// output. No handle to the driver, no writes to anyone's memory, nothing a
// protection could reasonably treat as tampering.
//
// Build:
//   cl /nologo /O2 /W3 HostWatcher.c /Fe:HostWatcher.exe
//
// Use:
//   HostWatcher.exe                            # 200 ms poll -> HostWatcher.log
//   HostWatcher.exe 100                        # poll every 100 ms
//   HostWatcher.exe 200 hyperkd.sys,vgk.sys    # watch other driver basenames
//   HostWatcher.exe 200 "" C:\out.log          # log path
//
// Start it *before* launching the title, play, quit the game, then Ctrl+C. The
// timeline shows kernel modules appearing/disappearing, driver-service state and
// DSE transitions with timestamps - the install trace the game log cannot hold.

#include <windows.h>
#include <winsvc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

// ---------------------------------------------------------------- queries

typedef LONG (WINAPI* t_NtQSI)(ULONG, void*, ULONG, ULONG*);
static t_NtQSI pQsi;

// class 11 SystemModuleInformation
typedef struct {
    HANDLE Section;
    PVOID  MappedBase;
    PVOID  ImageBase;
    ULONG  ImageSize;
    ULONG  Flags;
    USHORT LoadOrderIndex;
    USHORT InitOrderIndex;
    USHORT LoadCount;
    USHORT OffsetToFileName;
    UCHAR  FullPathName[256];
} HW_MOD;
typedef struct { ULONG NumberOfModules; HW_MOD Modules[1]; } HW_MODS;

// class 103 SystemCodeIntegrityInformation
typedef struct { ULONG Length; ULONG Options; } HW_CI;

#define MAXMODS 1024
#define MAXSVCS 1024
#define MAXPATH_ 520
#define MAXNAME 260

typedef struct {
    WCHAR name[MAXNAME];       // basename, upper case
    WCHAR path[MAXPATH_];
    ULONG size;
    USHORT order;
} MODENT;

typedef struct {
    WCHAR name[MAXNAME];       // service key name
    WCHAR image[MAXPATH_];     // ImagePath from QueryServiceConfig (may be empty)
    DWORD state;
    int   announced;           // matched the watch list at least once
    int   present;             // seen in the current enumeration
} SVCENT;

static MODENT g_mods[MAXMODS];
static int    g_nMods;
static SVCENT g_svcs[MAXSVCS];
static int    g_nSvcs;

static ULONG  g_ci = 0xFFFFFFFFu;
static int    g_ciUnavailable;
static FILE*  g_out;

static void Emit(const WCHAR* text)
{
    SYSTEMTIME t;
    WCHAR ts[24];
    GetLocalTime(&t);
    _snwprintf_s(ts, _countof(ts), _TRUNCATE, L"%02u:%02u:%02u.%03u ",
                 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    wprintf(L"%s%s\n", ts, text);
    fflush(stdout);
    if (g_out)
    {
        fwprintf(g_out, L"%s%s\n", ts, text);
        fflush(g_out);
    }
}

static void BaseNameOf(const WCHAR* path, WCHAR* out, size_t cch)
{
    const WCHAR* b = path;
    const WCHAR* p;
    if (!path) { out[0] = 0; return; }
    for (p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') b = p + 1;
    wcsncpy_s(out, cch, b, _TRUNCATE);
    _wcsupr_s(out, cch);
}

// Same decoding as the in-game [SYS] line: documented bits by name, the rest as
// "undecoded" rather than guessed at.
static void CiText(ULONG o, WCHAR* out, size_t cch)
{
    static const WCHAR* names[13] = {
        L"enabled", L"test-signing", L"umci", L"umci-audit", L"umci-exclusions",
        L"test-build", L"preproduction", L"debug-mode", L"flighting",
        L"hvci-kmci", L"hvci-kmci-audit", L"hvci-kmci-strict", L"hvci-ium"
    };
    WCHAR tmp[240];
    int i;
    tmp[0] = 0;
    for (i = 0; i < 13; i++)
    {
        if (!(o & (1u << i))) continue;
        if (tmp[0]) wcscat_s(tmp, _countof(tmp), L" ");
        wcscat_s(tmp, _countof(tmp), names[i]);
    }
    if (o >> 13)
    {
        WCHAR tail[48];
        _snwprintf_s(tail, _countof(tail), _TRUNCATE, L"%sundecoded 0x%X",
                     tmp[0] ? L" " : L"", o >> 13);
        wcscat_s(tmp, _countof(tmp), tail);
    }
    if (!tmp[0]) wcscpy_s(tmp, _countof(tmp), L"none");
    wcsncpy_s(out, cch, tmp, _TRUNCATE);
}

static int WatchListMatch(const WCHAR* upperName, const WCHAR* extraCsv)
{
    static const WCHAR* def[] = { L"HYPERKD.SYS", L"HVSERVICE.SYS", L"ARTIFACT" };
    WCHAR buf[MAXPATH_];
    WCHAR* ctx = NULL;
    WCHAR* tok;
    int i;
    if (!upperName || !upperName[0]) return 0;
    for (i = 0; i < (int)(sizeof(def) / sizeof(def[0])); i++)
        if (!wcscmp(upperName, def[i])) return 1;
    if (!extraCsv || !extraCsv[0]) return 0;
    wcsncpy_s(buf, _countof(buf), extraCsv, _TRUNCATE);
    for (tok = wcstok_s(buf, L",", &ctx); tok; tok = wcstok_s(NULL, L",", &ctx))
    {
        WCHAR up[MAXNAME];
        wcsncpy_s(up, _countof(up), tok, _TRUNCATE);
        _wcsupr_s(up, _countof(up));
        if (!wcscmp(up, upperName)) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- modules

static void PollModules(void)
{
    static BYTE buf[16 << 20];
    static MODENT fresh[MAXMODS];      // ~1.6 MB: keep it off the 1 MB stack
    ULONG ret = 0;
    HW_MODS* list = (HW_MODS*)buf;
    int nFresh = 0, i, j;
    WCHAR line[700];

    if (pQsi(11, buf, sizeof(buf), &ret) != 0) return;
    if (list->NumberOfModules == 0 || list->NumberOfModules > MAXMODS) return;
    for (i = 0; i < (int)list->NumberOfModules; i++)
    {
        WCHAR wpath[MAXPATH_], base[MAXNAME];
        MultiByteToWideChar(CP_ACP, 0, (const char*)list->Modules[i].FullPathName, -1,
                            wpath, _countof(wpath));
        BaseNameOf(wpath, base, _countof(base));
        wcsncpy_s(fresh[nFresh].name, MAXNAME, base, _TRUNCATE);
        wcsncpy_s(fresh[nFresh].path, MAXPATH_, wpath, _TRUNCATE);
        fresh[nFresh].size = list->Modules[i].ImageSize;
        fresh[nFresh].order = list->Modules[i].LoadOrderIndex;
        nFresh++;
    }

    for (i = 0; i < nFresh; i++)
    {
        int found = 0;
        for (j = 0; j < g_nMods && !found; j++)
            if (!wcscmp(g_mods[j].name, fresh[i].name)) found = 1;
        if (!found)
        {
            _snwprintf_s(line, _countof(line), _TRUNCATE,
                         L"KERNEL  + %-24s size 0x%-7X load-order %-4u %s",
                         fresh[i].name, fresh[i].size, (unsigned)fresh[i].order, fresh[i].path);
            Emit(line);
        }
    }
    for (j = 0; j < g_nMods; j++)
    {
        int found = 0;
        for (i = 0; i < nFresh && !found; i++)
            if (!wcscmp(g_mods[j].name, fresh[i].name)) found = 1;
        if (!found)
        {
            _snwprintf_s(line, _countof(line), _TRUNCATE,
                         L"KERNEL  - %-24s (was size 0x%X, load-order %u)",
                         g_mods[j].name, g_mods[j].size, (unsigned)g_mods[j].order);
            Emit(line);
        }
    }
    memcpy(g_mods, fresh, sizeof(MODENT) * (size_t)nFresh);
    g_nMods = nFresh;
}

// ---------------------------------------------------------------- services

static SVCENT* SvcEntry(const WCHAR* name)
{
    int i;
    for (i = 0; i < g_nSvcs; i++)
        if (!wcscmp(g_svcs[i].name, name)) return &g_svcs[i];
    if (g_nSvcs >= MAXSVCS) return NULL;
    memset(&g_svcs[g_nSvcs], 0, sizeof(SVCENT));
    wcsncpy_s(g_svcs[g_nSvcs].name, MAXNAME, name, _TRUNCATE);
    g_svcs[g_nSvcs].state = 0xFFFFFFFFu;
    return &g_svcs[g_nSvcs++];
}

static const WCHAR* StateText(DWORD st)
{
    switch (st)
    {
    case SERVICE_RUNNING:        return L"RUNNING";
    case SERVICE_STOPPED:        return L"STOPPED";
    case SERVICE_START_PENDING:  return L"START_PENDING";
    case SERVICE_STOP_PENDING:   return L"STOP_PENDING";
    default:                     return L"other";
    }
}

static void PollServices(const WCHAR* extraCsv)
{
    static BYTE buf[256 << 10];
    SC_HANDLE scm;
    DWORD needed = 0, returned = 0, i;
    WCHAR line[900];

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return;
    if (!EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_DRIVER, SERVICE_STATE_ALL,
                               buf, sizeof(buf), &needed, &returned, NULL, NULL))
    {
        CloseServiceHandle(scm);
        return;
    }
    for (i = 0; i < returned; i++)
    {
        ENUM_SERVICE_STATUS_PROCESSW* e = &((ENUM_SERVICE_STATUS_PROCESSW*)buf)[i];
        SVCENT* s = SvcEntry(e->lpServiceName);
        WCHAR base[MAXNAME], keyUp[MAXNAME], dispUp[MAXNAME];
        int isNew;
        if (!s) continue;
        isNew = (s->state == 0xFFFFFFFFu);

        if (isNew || !s->image[0])
        {
            // ImagePath is only available from the service's own config, so it is
            // fetched once per service. This is also how the key name ("artifact")
            // is separated from the driver file name ("hyperkd.sys").
            SC_HANDLE svc = OpenServiceW(scm, e->lpServiceName, SERVICE_QUERY_CONFIG);
            if (svc)
            {
                DWORD need2 = 0;
                LPQUERY_SERVICE_CONFIGW cfg = NULL;
                QueryServiceConfigW(svc, NULL, 0, &need2);
                cfg = (LPQUERY_SERVICE_CONFIGW)malloc(need2 ? need2 : 512);
                if (cfg && QueryServiceConfigW(svc, cfg, need2, &need2))
                    wcsncpy_s(s->image, MAXPATH_, cfg->lpBinaryPathName ? cfg->lpBinaryPathName : L"", _TRUNCATE);
                free(cfg);
                CloseServiceHandle(svc);
            }
        }

        BaseNameOf(e->lpServiceName, keyUp, _countof(keyUp));
        BaseNameOf(e->lpDisplayName, dispUp, _countof(dispUp));
        BaseNameOf(s->image, base, _countof(base));
        s->present = 1;

        if (!s->announced)
        {
            if (!WatchListMatch(keyUp, extraCsv) && !WatchListMatch(dispUp, extraCsv) &&
                !WatchListMatch(base, extraCsv))
            {
                s->state = e->ServiceStatusProcess.dwCurrentState;
                continue;   // not interesting, but keep its state current
            }
            s->announced = 1;
            _snwprintf_s(line, _countof(line), _TRUNCATE,
                         L"SERVICE %-16s %s (matches watch list) image=%s",
                         e->lpServiceName, StateText(e->ServiceStatusProcess.dwCurrentState),
                         s->image[0] ? s->image : L"(no ImagePath)");
            Emit(line);
        }
        else if (s->state != e->ServiceStatusProcess.dwCurrentState)
        {
            _snwprintf_s(line, _countof(line), _TRUNCATE,
                         L"SERVICE %-16s %s -> %s",
                         e->lpServiceName, StateText(s->state),
                         StateText(e->ServiceStatusProcess.dwCurrentState));
            Emit(line);
        }
        s->state = e->ServiceStatusProcess.dwCurrentState;
    }

    // A protection that tears its service down on exit simply stops appearing in
    // the enumeration - report that too, or the teardown half of the timeline is
    // invisible.
    {
        int j;
        for (j = 0; j < g_nSvcs; j++)
        {
            if (!g_svcs[j].present)
            {
                if (g_svcs[j].announced && g_svcs[j].state != 0xFFFFFFFFu)
                {
                    _snwprintf_s(line, _countof(line), _TRUNCATE,
                                 L"SERVICE %-16s no longer registered (was %s, image=%s)",
                                 g_svcs[j].name, StateText(g_svcs[j].state),
                                 g_svcs[j].image[0] ? g_svcs[j].image : L"(none)");
                    Emit(line);
                    g_svcs[j].state = 0xFFFFFFFFu;
                }
                continue;
            }
        }
    }
    for (i = 0; i < (DWORD)g_nSvcs; i++) g_svcs[i].present = 0;
    CloseServiceHandle(scm);
}

// ---------------------------------------------------------------- DSE

static void PollCi(void)
{
    HW_CI ci;
    ULONG ret = 0;
    WCHAR names[240], line[700];
    if (g_ciUnavailable) return;
    memset(&ci, 0, sizeof(ci));
    ci.Length = sizeof(ci);
    if (pQsi(103, &ci, sizeof(ci), &ret) != 0 || ci.Length < sizeof(ci))
    {
        g_ciUnavailable = 1;
        Emit(L"DSE     unavailable on this build - code integrity state cannot be read");
        return;
    }
    if (g_ci == ci.Options) return;
    CiText(ci.Options, names, _countof(names));
    if (g_ci == 0xFFFFFFFFu)
    {
        _snwprintf_s(line, _countof(line), _TRUNCATE,
                     L"DSE     baseline 0x%08X (%s) - %s", (unsigned)ci.Options, names,
                     (ci.Options & 1) ? L"driver signing enforced (DSE on)"
                                      : L"driver signing NOT enforced (DSE off)");
    }
    else
    {
        const WCHAR* what = L"";
        if (!(g_ci & 1) && (ci.Options & 1)) what = L" - DSE was turned back ON";
        else if ((g_ci & 1) && !(ci.Options & 1)) what = L" - DSE was turned OFF (an unsigned driver can load now)";
        _snwprintf_s(line, _countof(line), _TRUNCATE,
                     L"DSE     0x%08X -> 0x%08X (%s)%s",
                     (unsigned)g_ci, (unsigned)ci.Options, names, what);
    }
    g_ci = ci.Options;
    Emit(line);
}

// ---------------------------------------------------------------- main

static volatile LONG g_stop;

static BOOL WINAPI OnCtrl(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_stop, 1);
    return TRUE;
}

int wmain(int argc, wchar_t** argv)
{
    int pollMs = 50;
    int n = 0;
    const WCHAR* extra = L"";
    const WCHAR* logPath = L"HostWatcher.log";
    WCHAR line[800];

    if (argc > 1 && argv[1][0]) pollMs = _wtoi(argv[1]);
    if (pollMs < 20) pollMs = 20;
    if (argc > 2) extra = argv[2];
    if (argc > 3 && argv[3][0]) logPath = argv[3];

    pQsi = (t_NtQSI)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    if (!pQsi)
    {
        wprintf(L"HostWatcher: ntdll!NtQuerySystemInformation not found\n");
        return 1;
    }
    if (logPath && logPath[0] == L'-') logPath = NULL;
    if (logPath) { if (_wfopen_s(&g_out, logPath, L"a, ccs=UTF-16LE") != 0) g_out = NULL; }
    SetConsoleCtrlHandler(OnCtrl, TRUE);

    _snwprintf_s(line, _countof(line), _TRUNCATE,
                 L"HostWatcher: polling every %d ms, extra watch \"%s\" - Ctrl+C to stop",
                 pollMs, (extra && extra[0]) ? extra : L"(none)");
    Emit(line);

    while (!g_stop)
    {
        // DSE on every poll (it is one cheap query and the off window can be
        // short); the module list and the service table on every tenth poll,
        // because enumerating them is the expensive part of the loop.
        PollCi();
        if (n++ % 10 == 0) { PollModules(); PollServices(extra); }
        Sleep((DWORD)pollMs);
    }

    Emit(L"HostWatcher: stopped");
    if (g_out) fclose(g_out);
    return 0;
}

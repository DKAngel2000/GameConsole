// GameConsole.c - ONE plain-C DLL replacing the old .NET Native AOT build plus
// the native helper DLLs. Loaded by a game loader that only calls LoadLibraryW,
// so it self-starts: DllMain only spawns a worker thread (never work under the
// loader lock) and that thread does everything - console, session log, monitors,
// log tailing, exception/ODS/module capture and the (optional) hook layer.
//
// Reused VERBATIM from GameHooks.c (the old native hook layer):
//   - the lock-free Vyukov event ring (GCEv/EvPush/EvPop with the init fix)
//   - the native VEH (VehNative) + unhandled-exception filter (UehNative)
//   - the Ldr module-notification callback (ModNotify)
//   - the IAT hook layer (HookIatName/HookIatByValue/HookIatApi + every HkIat*
//     handler + the KERNELBASE interposition layer + the toolhelp walk for the
//     KERNELBASE/ntdll copies + FindWorkingNtdll)
//   - the legacy ntdll stub patching (PatchEx/AllocTramp/SuspendAll/ResumeAll)
//
// Everything that used to go to hooks.log now travels through the ring as a
// fully formatted wide "hook line" (event kind 6) and is printed by the worker.
//
// Build: build.cmd (cl /nologo /LD /O2 /W3 GameConsole.c /Fe:GameConsole.dll)

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>
#include <wctype.h>
#include <psapi.h>
#include <winsvc.h>                   // service enumeration (ScanServices)
#include <strsafe.h>
#include <intrin.h>

#pragma comment(lib, "psapi.lib")     // GetProcessMemoryInfo
#pragma comment(lib, "user32.lib")    // ShowWindow

// ---------------------------------------------------------------- ring event
// One event record shared by every context that must not do I/O (hook handlers,
// VEH/UEH, module notifications). text holds ANSI bytes for kind 0 and wide
// chars for everything else; it was enlarged to 1024 wide chars so long paths
// (and fully formatted hook lines, kind 6) fit.
typedef struct {
    unsigned __int64 code, addr;
    unsigned __int64 frames[8];
    unsigned __int64 info1, info2;
    unsigned __int64 dr[6];  // Dr0,Dr1,Dr2,Dr3,Dr6,Dr7 at a single-step/breakpoint
    unsigned long    tid;
    unsigned long    kind;   // 0=ODS ansi,1=ODS wide,2=first-chance,3=crash,4=mod load,5=mod unload,6=hook line
    WCHAR            text[1024];
} GCEv;

#define GCEV_N 256
typedef struct { volatile unsigned __int64 seq; GCEv e; } GCEvCell;
static GCEvCell g_ev[GCEV_N];
static volatile unsigned __int64 g_evHead = 0, g_evTail = 0;
static int g_ringInit = 0;

static void RingInit(void)
{
    int i;
    if (g_ringInit) return;
    for (i = 0; i < GCEV_N; i++) g_ev[i].seq = (unsigned __int64)i;   // Vyukov init
    g_ringInit = 1;
}

static int EvPush(const GCEv* e)
{
    unsigned __int64 pos = g_evHead;
    GCEvCell* c;
    for (;;)
    {
        unsigned __int64 s;
        __int64 d;
        c = &g_ev[pos & (GCEV_N - 1)];
        s = c->seq;
        d = (__int64)s - (__int64)pos;
        if (d == 0)
        {
            if (InterlockedCompareExchange64((volatile LONG64*)&g_evHead, (LONG64)(pos + 1), (LONG64)pos) == (LONG64)pos) break;
        }
        else if (d < 0) return 0;          // ring full, drop the event
        else pos = s;
    }
    c->e = *e;
    c->seq = pos + 1;
    return 1;
}

static int EvPop(GCEv* out)
{
    unsigned __int64 pos = g_evTail;
    GCEvCell* c;
    for (;;)
    {
        unsigned __int64 s;
        __int64 d;
        c = &g_ev[pos & (GCEV_N - 1)];
        s = c->seq;
        d = (__int64)s - (__int64)(pos + 1);
        if (d == 0)
        {
            if (InterlockedCompareExchange64((volatile LONG64*)&g_evTail, (LONG64)(pos + 1), (LONG64)pos) == (LONG64)pos) break;
        }
        else if (d < 0) return 0;          // empty
        else pos = s;
    }
    *out = c->e;
    c->seq = pos + GCEV_N;
    return 1;
}

// ---------------------------------------------------------------- settings
static int ConsoleVisible, ShowTimestamps, ShowColors, ShowLegend, ShowStartupInfo, HoldOnCrash, AllocConsoleFlag = 1;
static int SaveLogFile, SaveTaggedLines, SaveRawLines;
static int WriteTraceFile = 1, WriteCrashFile = 1;
static int CaptureOds, RedirectCrt, RedirectStdHandles, EnableCrashHandler;
static int ShowOds, ShowLogFiles, ShowModules, ShowCrash, ShowExceptions, ShowThreads, ShowMemory,
           ShowNetwork, ShowFileSystem, ShowHeartbeat, ShowInfo, ShowConsoleMessages, ShowSharedData,
           ShowSystem;
static int OdsShowCaller;
static int TailLogFiles, IncludeUnityPlayerLog, TailTxtLogs, TailSkipExisting;
static int ModulesPresent, ModulesLoaded, ModulesUnloaded, ModulesHideSystem, ModulesShowPath;
static int LogAllExceptions, ExcCrashCodes, ExcStackTrace, ExcCpp, ExcClr, ExcBreakpoints, ExcOther;
static int MonThreads, ThreadsPresent, ThreadsStarted, ThreadsExited;
static int MonMemory, MemBaseline;
static int MonNetwork, NetNew, NetChanges, NetClosed;
static int MonFiles, MonTempDir, FsCreated, FsDeleted, FsRenamed;
static int MonHeartbeat;
static int HookApis, HookFileReads, HookFileWrites, HookMemPatches, HookOnlyGameModules, HookLegacyNtdll;
static int ShowHooks;
static int ConsoleCols = 300, ConsoleRows = 20000, MaxCrashReports = 25, MaxOtherExceptions = 500,
           LogScanDepth = 3, HeartbeatSeconds = 10, MaxHookReports = 2000;
static int ScanOtherHooks = 1, ScanOtherHooksMax = 64;
static int ShadowDetailMax = 24;     // shadow-export lines listed per module
static int ScanLdr = 1;              // PEB loader-list vs toolhelp32 survey
static int ScanEh = 1;               // VEH chain survey
static int ScanServices = 1;         // SCM service scan
static int ScanDrivers = 1;          // loaded kernel modules
static int ServiceWatchAll = 0;      // list everything, not just the watch list
static int HookExe = 1;              // also instrument the game exe's IAT
static int HookHiddenModules = 0;    // rewrite a hidden module's IAT (off: a crash suspect)
static int HookDelayMs = 0;          // defer all IAT writes N ms (see the setting's comment)
static WCHAR ServiceWatch[400] = L"denuvo,hypervisor,hvservice,hyperkd,simplesvm,vgk,vgc,vanguard,easyanticheat,battleye,beservice,bedaisy,nprotect,gameguard,xigncode,arxan,vmprotect,themis,xtrap";
#define MAX_SHADOWSEEN 64
static ULONG_PTR g_shadowSeen[MAX_SHADOWSEEN]; static int g_nShadowSeen = 0;
static int CensusAll = 1, CensusAllMax = 300;
static int GpaThunks = 1;
static int WatchSharedData = 1;      // KUSER_SHARED_DATA mailbox watch
static int MemGuardScan = 1;         // report guard/noaccess pages
static int MemGuardAll = 0;          // list low-address (stack/heap) guard pages too
static int ScanShadow = 1;           // survey the shadow DLLs + exec non-image prologues
static int CsvExport = 1;            // write a machine-readable CSV alongside the log

static WCHAR g_consoleTitle[260] = L"Game Debug Console";
static WCHAR g_hookModules[600] = L"";
static WCHAR g_logDirectory[520] = L"";

typedef struct { const char* key; const char* def; const char* comment; } Opt;

static const Opt Opts[] = {
    { "#", "", "Console window" },
    { "ConsoleVisible", "true", "Show the console window. false = hidden (the saved log file keeps working)" },
    { "AllocConsole", "true", "false = no console at all (log-file-only mode). Diagnostic for compat-shim conflicts" },
    { "ConsoleTitle", "Game Debug Console", "Title of the console window" },
    { "ConsoleCols", "300", "Console buffer width, 80-1000. Wide = long lines do not wrap in the saved log" },
    { "ConsoleRows", "20000", "Console buffer height, 200-30000. When nearly full it is wiped after saving" },
    { "ShowTimestamps", "true", "Prefix tagged lines with HH:mm:ss.fff" },
    { "ShowColors", "true", "Colour the tags (ANSI codes)" },
    { "ShowLegend", "true", "Print the tag legend at startup" },
    { "ShowStartupInfo", "true", "Print exe path, command line, working directory and OS at startup" },
    { "HoldOnCrash", "true", "On an unhandled crash, freeze the process so the console stays readable" },
    { "#", "", "Saving" },
    { "SaveLogFile", "true", "Save the console to a file" },
    { "LogDirectory", "", "Folder for saved logs. Empty = a logs folder next to GameConsole.dll" },
    { "SaveTaggedLines", "true", "Save lines written by GameConsole (the ones with a [tag])" },
    { "SaveRawLines", "true", "Save lines written straight to the console by the game or mods (marked [raw])" },
    { "WriteTraceFile", "true", "Write GameConsole.log in %TEMP% (startup steps, handy if something fails to start)" },
    { "WriteCrashFile", "true", "Write GameConsole_crash.log in %TEMP% (crash reports)" },
    { "#", "", "Capture (these only listen or redirect output, nothing is patched)" },
    { "CaptureOutputDebugString", "true", "Listen for OutputDebugString calls" },
    { "RedirectCrt", "true", "Point the game's printf/stdout/stderr (UCRT) at the console" },
    { "RedirectStdHandles", "true", "Point the process standard handles at the console (Win32 writes to stdout/stderr)" },
    { "EnableCrashHandler", "true", "Install an unhandled-exception filter that logs the final crash" },
    { "#", "", "Categories shown. false hides the category from the console AND the saved file" },
    { "Show_ODS", "true", "[ODS] OutputDebugString text" },
    { "Show_LogFiles", "true", "[file.log] tailed log files" },
    { "Show_Modules", "true", "[MOD] DLL load/unload" },
    { "Show_Crash", "true", "[CRASH] access violations, stack overflows and similar" },
    { "Show_Exceptions", "true", "[EXC] other exceptions" },
    { "Show_Threads", "true", "[THR] threads" },
    { "Show_Memory", "true", "[MEM] new executable memory" },
    { "Show_Network", "true", "[NET] TCP connections" },
    { "Show_FileSystem", "true", "[FS] files created/deleted/renamed" },
    { "Show_Heartbeat", "true", "[HB] periodic counts" },
    { "Show_SharedData", "true", "[SD] KUSER_SHARED_DATA changes (the Dr-armed mailbox)" },
    { "Show_System", "true", "[SYS]/[KRNL]/[SVC] hypervisor, kernel modules and services" },
    { "Show_Info", "true", "[INFO] startup information" },
    { "Show_Console", "true", "[console] GameConsole's own status messages" },
    { "Show_Hooks", "true", "[HOOK] hooked Nt* API activity (file reads/writes, memory-protection changes)" },
    { "#", "", "OutputDebugString" },
    { "OdsShowCaller", "true", "Name the module that called OutputDebugString in the tag" },
    { "#", "", "Log files" },
    { "TailLogFiles", "true", "Follow *.log files in the game folder while the game runs" },
    { "TailUnityPlayerLog", "true", "Also follow Unity Player.log under AppData LocalLow" },
    { "TailTxtFilesWithLogInName", "true", "Also follow .txt files with log in the name" },
    { "TailSkipExistingContent", "false", "true = only show lines added after the file was first seen" },
    { "LogScanDepth", "3", "How many subfolder levels under the game folder to search (0-10)" },
    { "#", "", "Modules" },
    { "ModulesPresent", "true", "List the modules already loaded at startup" },
    { "ModulesLoaded", "true", "Log DLLs as they load" },
    { "ModulesUnloaded", "true", "Log DLLs as they unload" },
    { "ModulesHideSystem", "false", "Hide DLLs from the Windows folder (shows only game and mod DLLs)" },
    { "ModulesShowPath", "true", "Show the full path of each present module" },
    { "#", "", "Exceptions" },
    { "LogAllExceptions", "true", "Log non-crash exceptions (C++ throws, breakpoints and so on)" },
    { "ExceptionCrashCodes", "true", "Log access violations and similar when they are thrown (first chance)" },
    { "ExceptionStackTrace", "true", "Include a stack trace in crash reports" },
    { "ExceptionCpp", "true", "Include C++ throws" },
    { "ExceptionClr", "true", "Include .NET CLR exceptions" },
    { "ExceptionBreakpoints", "true", "Include breakpoints and single-steps" },
    { "ExceptionOther", "true", "Include any other exception code" },
    { "MaxCrashReports", "25", "Cap on first-chance crash-code reports (some are harmless)" },
    { "MaxOtherExceptions", "500", "Cap on other exception reports" },
    { "#", "", "Threads" },
    { "MonThreads", "true", "Watch threads" },
    { "ThreadsPresent", "true", "List threads that already exist at startup" },
    { "ThreadsStarted", "true", "Log new threads with their start module" },
    { "ThreadsExited", "true", "Log threads that exit" },
    { "#", "", "Executable memory" },
    { "MonMemory", "true", "Watch for executable memory not owned by any DLL or EXE" },
    { "MemoryBaseline", "true", "Print how many such regions exist at startup" },
    { "#", "", "Network" },
    { "MonNetwork", "true", "Watch this process's IPv4 TCP connections" },
    { "NetNew", "true", "Log new connections" },
    { "NetChanges", "true", "Log connection state changes" },
    { "NetClosed", "true", "Log connections that close" },
    { "#", "", "Files" },
    { "MonGameFolder", "true", "Watch the game folder (recursive) for created/deleted/renamed files" },
    { "MonTempDir", "true", "Watch the %TEMP% folder too" },
    { "FsCreated", "true", "Log created files and folders" },
    { "FsDeleted", "true", "Log deleted files and folders" },
    { "FsRenamed", "true", "Log renamed files and folders" },
    { "#", "", "Heartbeat" },
    { "MonHeartbeat", "true", "Print thread/handle/memory counts periodically" },
    { "HeartbeatSeconds", "10", "Seconds between heartbeats (1-3600)" },
    { "#", "", "Hooks (opt-in: patches ntdll Nt* exports in-process to log them)" },
    { "HookApis", "true", "Install lightweight ntdll hooks. Set false for the old purely passive design" },
    { "HookFileReads", "true", "Log NtReadFile calls (file reads)" },
    { "HookFileWrites", "true", "Log NtCreateFile / NtOpenFile / NtWriteFile calls" },
    { "HookMemPatches", "true", "Log NtProtectVirtualMemory calls when execute rights are involved (code patching)" },
    { "HookOnlyGameModules", "true", "true = skip callers inside the Windows folder (game/mod DLLs only)" },
    { "HookLegacyNtdll", "false", "Also allow the legacy ntdll stub patching when HookModules is empty. Dangerous: races other mods' ntdll hooks" },
    { "HookModules", "", "Comma-separated module names whose IAT is instrumented, e.g. artifact.dll. Empty = no per-module IAT hooks, but the KERNELBASE interposition layer is still installed and logs ALL callers" },
    { "HookExe", "false", "Also instrument the game executable's own import table. OFF by default: a Denuvo-protected exe checks its own imports, and repointing them can make it fail its own tamper check (seen as 'corrupt pak index' in UE titles). Turn on only when you need the exe's own first-hop calls" },
    { "HookHiddenModules", "false", "Instrument the import table of a module that is in no loader list (found only by caller address, e.g. a protection helper). OFF by default: such a module is deliberately hidden from the loader, and repointing its imports while it runs has killed the game at that exact moment three times out of three. It is still detected, censused and reported when this is false" },
    { "HookDelayMs", "0", "Wait this many ms after startup before the first IAT write. 0 = install immediately (normal, loaded by the mod loader). Set it (e.g. 8000) when the DLL is injected at instruction zero with tools\\EarlyLoad.c: a protected module that finds its import table repointed *before* its own startup fails its tamper check and aborts the session ('something went wrong' + a support.codefusion.technology/antitamper link). Everything read-only (surveys, DSE sampling, module log) still runs from t=0" },
    { "MaxHookReports", "2000", "Cap on hook log lines so a chatty caller cannot flood the console" },
    { "#", "", "Read-only survey: what OTHER code has already inline-hooked ntdll" },
    { "ScanOtherHooks", "true", "Read-only ntdll survey: report EVERY export whose entry is a jump instead of a clean syscall stub, classified - hooked by other module / same-image alias / NON-EXPORT same-image target (cave). Runs at startup and re-runs each heartbeat (new sites only). Never writes anything" },
    { "ScanOtherHooksMax", "400", "Max [SCAN] detail lines per image per survey"},
    { "ShadowDetailMax", "24", "Shadow-DLL survey: max export-redirect detail lines per shadow module (the per-module summary is always written; higher = more per-module coverage)" },
    { "CensusAll", "true", "Binding census of every loaded module's import table: tallies each import's bound target and details the ones NOT bound to loader-loaded images - i.e. Denuvo-style manual-mapped copies. Runs at startup and for modules that appear later" },
    { "CensusAllMax", "300", "Cap on detail lines from the all-module census" },
    { "GpaThunks", "true", "GetProcAddress calls made through HookModules' import table are logged AND return a logging thunk (counts calls + last caller, reported each heartbeat). Set false if a game verifies the returned pointer against its import table" },
    { "ScanShadow", "true", "Read-only survey of the manually-mapped shadow DLLs' exports (are they trampolines into the protected image?) plus a prologue decode of executable non-image regions. Never writes anything" },
    { "ScanLdr", "true", "Walk the PEB's real loader list read-only and report modules toolhelp32 does not show (manually-mapped DLLs with a hand-built loader entry) and loader entries pointing at non-MEM_IMAGE memory" },
    { "ScanEh", "true", "Report the process's real vectored-exception-handler chain (who registered which handler, in what order) - read-only walk from GameConsole's own registration - and log every later VEH/UEH registration via IAT hooks" },
    { "ScanServices", "true", "Enumerate Windows services read-only and report the ones matching ServiceWatch (name/display/path), plus new ones that appear while the game runs" },
    { "ScanDrivers", "true", "Enumerate loaded kernel drivers read-only (NtQuerySystemInformation) and report the ones matching ServiceWatch - the kernel/hypervisor component a protection service installs" },
    { "ServiceWatch", "denuvo,hypervisor,hvservice,hyperkd,simplesvm,vgk,vgc,vanguard,easyanticheat,battleye,beservice,bedaisy,nprotect,gameguard,xigncode,arxan,vmprotect,themis,xtrap", "Comma-separated words matched case-insensitively against service names, display names, binary paths, kernel driver paths and the CPUID hypervisor vendor" },
    { "ServiceWatchAll", "false", "Also list EVERY running service / loaded kernel driver, not just the ones matching ServiceWatch (noisy)" },
    { "WatchSharedData", "true", "Watch the last bytes of KUSER_SHARED_DATA (0x7FFE0FE0..0x1000) for changes - artifact arms Dr3 there, so it may be a handshake mailbox" },
    { "MemGuardScan", "true", "Report committed non-image pages carrying PAGE_GUARD or PAGE_NOACCESS (anti-debug tripwires)" },
    { "MemGuardAll", "false", "Also list low-address (thread-stack/heap) guard pages - usually noise; the protection's own tripwires live in the high module range and are always listed" },
    { "CsvExport", "true", "Write a machine-readable GameConsole_<pid>.csv next to the log (census/survey/hook/mailbox rows)" },
    { NULL, NULL, NULL }
};

typedef struct { char key[64]; char val[600]; } IniKV;
static IniKV g_ini[160];
static int g_nIni = 0;

static const char* DefOf(const char* k)
{
    int i;
    for (i = 0; Opts[i].key; i++) if (_stricmp(Opts[i].key, k) == 0) return Opts[i].def;
    return "";
}
static const char* IniGet(const char* k)
{
    int i;
    for (i = 0; i < g_nIni; i++) if (_stricmp(g_ini[i].key, k) == 0) return g_ini[i].val;
    return NULL;
}
static const char* V(const char* k)
{
    const char* v = IniGet(k);
    return v ? v : DefOf(k);
}
static int B(const char* k)
{
    char v[64]; int i;
    const char* s = V(k);
    for (i = 0; i < 63 && s[i]; i++) v[i] = (char)tolower((unsigned char)s[i]);
    v[i] = 0;
    if (!strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "on")) return 1;
    if (!strcmp(v, "false") || !strcmp(v, "0") || !strcmp(v, "no") || !strcmp(v, "off")) return 0;
    return strcmp(DefOf(k), "true") == 0;
}
static int I(const char* k)
{
    const char* s = V(k);
    int n;
    while (*s == ' ' || *s == '\t') s++;
    if ((*s >= '0' && *s <= '9') || ((*s == '-' || *s == '+') && s[1] >= '0' && s[1] <= '9'))
        n = atoi(s);
    else
        n = atoi(DefOf(k));
    return n;
}
static void TrimToW(const char* src, WCHAR* dst, int cch)
{
    char tmp[600]; int k = 0, i;
    const char* s = src;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    for (i = 0; s[i] && i < 599; i++) tmp[k++] = s[i];
    while (k > 0 && (tmp[k - 1] == ' ' || tmp[k - 1] == '\t' || tmp[k - 1] == '\r' || tmp[k - 1] == '\n')) k--;
    tmp[k] = 0;
    MultiByteToWideChar(CP_UTF8, 0, tmp, -1, dst, cch);
    dst[cch - 1] = 0;
}
static void TrimToA(const char* src, char* dst, int cch)
{
    int k = 0, i;
    const char* s = src;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    for (i = 0; s[i] && k < cch - 1; i++) dst[k++] = s[i];
    while (k > 0 && (dst[k - 1] == ' ' || dst[k - 1] == '\t' || dst[k - 1] == '\r' || dst[k - 1] == '\n')) k--;
    dst[k] = 0;
}

static WCHAR g_dllDir[520] = L"";
static WCHAR g_sysDir[520] = L"";
static WCHAR g_iniPath[520] = L"";
static HINSTANCE g_hinst = NULL;
static DWORD g_pid = 0;

// ---------------------------------------------------------------- string appender for the ini
typedef struct { char* buf; size_t cap; size_t len; } SB;
static void SbAdd(SB* sb, const char* s)
{
    while (*s && sb->len + 1 < sb->cap) sb->buf[sb->len++] = *s++;
    sb->buf[sb->len] = 0;
}

static void WriteIni(const char* path, int exists)
{
    char buf[32768]; SB sb; int i, anyMissing = 0; FILE* f;
    sb.buf = buf; sb.cap = sizeof(buf); sb.len = 0; buf[0] = 0;
    if (!exists)
    {
        SbAdd(&sb, "; GameConsole settings. Edit values and restart the game. Values are true/false unless noted.\r\n");
        SbAdd(&sb, "; Missing keys fall back to the defaults shown here. Delete this file to regenerate it.\r\n");
    }
    else
    {
        SbAdd(&sb, "\r\n");
        SbAdd(&sb, "; ===== options added by a newer GameConsole build =====\r\n");
    }
    for (i = 0; Opts[i].key; i++)
    {
        if (Opts[i].key[0] == '#' && Opts[i].key[1] == 0)
        {
            if (!exists) { SbAdd(&sb, "\r\n; ===== "); SbAdd(&sb, Opts[i].comment); SbAdd(&sb, " =====\r\n"); }
            continue;
        }
        if (exists && IniGet(Opts[i].key)) continue;
        anyMissing = 1;
        if (Opts[i].comment[0]) { SbAdd(&sb, "; "); SbAdd(&sb, Opts[i].comment); SbAdd(&sb, "\r\n"); }
        SbAdd(&sb, Opts[i].key); SbAdd(&sb, "="); SbAdd(&sb, Opts[i].def); SbAdd(&sb, "\r\n");
    }
    if (exists && !anyMissing) return;   // nothing to append
    f = fopen(path, exists ? "ab" : "wb");
    if (!f) return;
    fwrite(buf, 1, sb.len, f);
    fclose(f);
}

static void LoadSettings(void)
{
    FILE* f; char raw[700]; int exists;
    GetModuleFileNameW(g_hinst, g_dllDir, 520);
    {
        WCHAR* s = wcsrchr(g_dllDir, L'\\');
        if (s) *s = 0;
    }
    GetWindowsDirectoryW(g_sysDir, 520);
    StringCchPrintfW(g_iniPath, 520, L"%s\\GameConsole.ini", g_dllDir);

    f = _wfopen(g_iniPath, L"rb");
    exists = (f != NULL);
    if (f)
    {
        while (fgets(raw, sizeof(raw), f))
        {
            char* p = raw; char* eq; char* k; char* v; char* c; int n;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == 0 || *p == '\r' || *p == '\n' || *p == ';' || *p == '#' || *p == '[') continue;
            eq = strchr(p, '=');
            if (!eq || eq == p) continue;
            *eq = 0;
            k = p; v = eq + 1;
            n = (int)strlen(v);
            while (n > 0 && (v[n - 1] == '\r' || v[n - 1] == '\n')) v[--n] = 0;
            c = strstr(v, " ;");
            if (c) *c = 0;
            if (g_nIni < 160)
            {
                TrimToA(k, g_ini[g_nIni].key, 64);
                TrimToA(v, g_ini[g_nIni].val, 600);
                g_nIni++;
            }
        }
        fclose(f);
    }
    {
        char iniPathA[1300];
        WideCharToMultiByte(CP_UTF8, 0, g_iniPath, -1, iniPathA, sizeof(iniPathA), NULL, NULL);
        WriteIni(iniPathA, exists);
    }

    ConsoleVisible = B("ConsoleVisible");
    AllocConsoleFlag = B("AllocConsole");
    TrimToW(V("ConsoleTitle"), g_consoleTitle, 260);
    if (g_consoleTitle[0] == 0) StringCchCopyW(g_consoleTitle, 260, L"Game Debug Console");
    ConsoleCols = I("ConsoleCols"); if (ConsoleCols < 80) ConsoleCols = 80; if (ConsoleCols > 1000) ConsoleCols = 1000;
    ConsoleRows = I("ConsoleRows"); if (ConsoleRows < 200) ConsoleRows = 200; if (ConsoleRows > 30000) ConsoleRows = 30000;
    ShowTimestamps = B("ShowTimestamps"); ShowColors = B("ShowColors");
    ShowLegend = B("ShowLegend"); ShowStartupInfo = B("ShowStartupInfo"); HoldOnCrash = B("HoldOnCrash");

    SaveLogFile = B("SaveLogFile"); TrimToW(V("LogDirectory"), g_logDirectory, 520);
    SaveTaggedLines = B("SaveTaggedLines"); SaveRawLines = B("SaveRawLines");
    WriteTraceFile = B("WriteTraceFile"); WriteCrashFile = B("WriteCrashFile");

    CaptureOds = B("CaptureOutputDebugString"); RedirectCrt = B("RedirectCrt");
    RedirectStdHandles = B("RedirectStdHandles"); EnableCrashHandler = B("EnableCrashHandler");

    ShowOds = B("Show_ODS"); ShowLogFiles = B("Show_LogFiles"); ShowModules = B("Show_Modules");
    ShowCrash = B("Show_Crash"); ShowExceptions = B("Show_Exceptions"); ShowThreads = B("Show_Threads");
    ShowMemory = B("Show_Memory"); ShowNetwork = B("Show_Network"); ShowFileSystem = B("Show_FileSystem");
    ShowHeartbeat = B("Show_Heartbeat"); ShowInfo = B("Show_Info"); ShowConsoleMessages = B("Show_Console");
    ShowSharedData = B("Show_SharedData");
    ShowSystem = B("Show_System");
    ShowHooks = B("Show_Hooks");

    OdsShowCaller = B("OdsShowCaller");

    TailLogFiles = B("TailLogFiles"); IncludeUnityPlayerLog = B("TailUnityPlayerLog");
    TailTxtLogs = B("TailTxtFilesWithLogInName"); TailSkipExisting = B("TailSkipExistingContent");
    LogScanDepth = I("LogScanDepth"); if (LogScanDepth < 0) LogScanDepth = 0; if (LogScanDepth > 10) LogScanDepth = 10;

    ModulesPresent = B("ModulesPresent"); ModulesLoaded = B("ModulesLoaded"); ModulesUnloaded = B("ModulesUnloaded");
    ModulesHideSystem = B("ModulesHideSystem"); ModulesShowPath = B("ModulesShowPath");

    LogAllExceptions = B("LogAllExceptions"); ExcCrashCodes = B("ExceptionCrashCodes");
    ExcStackTrace = B("ExceptionStackTrace"); ExcCpp = B("ExceptionCpp"); ExcClr = B("ExceptionClr");
    ExcBreakpoints = B("ExceptionBreakpoints"); ExcOther = B("ExceptionOther");
    MaxCrashReports = I("MaxCrashReports"); MaxOtherExceptions = I("MaxOtherExceptions");

    MonThreads = B("MonThreads"); ThreadsPresent = B("ThreadsPresent");
    ThreadsStarted = B("ThreadsStarted"); ThreadsExited = B("ThreadsExited");
    MonMemory = B("MonMemory"); MemBaseline = B("MemoryBaseline");
    MonNetwork = B("MonNetwork"); NetNew = B("NetNew"); NetChanges = B("NetChanges"); NetClosed = B("NetClosed");
    MonFiles = B("MonGameFolder"); MonTempDir = B("MonTempDir");
    FsCreated = B("FsCreated"); FsDeleted = B("FsDeleted"); FsRenamed = B("FsRenamed");
    MonHeartbeat = B("MonHeartbeat"); HeartbeatSeconds = I("HeartbeatSeconds");
    if (HeartbeatSeconds < 1) HeartbeatSeconds = 1; if (HeartbeatSeconds > 3600) HeartbeatSeconds = 3600;
    HookApis = B("HookApis"); HookFileReads = B("HookFileReads"); HookFileWrites = B("HookFileWrites");
    HookMemPatches = B("HookMemPatches"); HookOnlyGameModules = B("HookOnlyGameModules");
    HookLegacyNtdll = B("HookLegacyNtdll");
    MaxHookReports = I("MaxHookReports"); TrimToW(V("HookModules"), g_hookModules, 600);
    ScanOtherHooks = B("ScanOtherHooks"); ScanOtherHooksMax = I("ScanOtherHooksMax");
    ShadowDetailMax = I("ShadowDetailMax");
    CensusAll = B("CensusAll"); CensusAllMax = I("CensusAllMax");
    GpaThunks = B("GpaThunks");
    ScanShadow = B("ScanShadow"); WatchSharedData = B("WatchSharedData");
    ScanLdr = B("ScanLdr");
    ScanEh = B("ScanEh"); ScanServices = B("ScanServices"); ScanDrivers = B("ScanDrivers");
    ServiceWatchAll = B("ServiceWatchAll");
    HookExe = B("HookExe"); HookHiddenModules = B("HookHiddenModules");
    HookDelayMs = I("HookDelayMs");
    TrimToW(V("ServiceWatch"), ServiceWatch, 400);
    MemGuardScan = B("MemGuardScan"); CsvExport = B("CsvExport"); MemGuardAll = B("MemGuardAll");
}

// ---------------------------------------------------------------- output
static HANDLE g_hOut = NULL;
static HANDLE g_hIn = NULL;
static CRITICAL_SECTION g_wrLock;
static int g_wrLockInit = 0;

static const WCHAR* Color(const WCHAR* tag)
{
    if (wcsncmp(tag, L"[CRASH", 6) == 0) return L"\x1b[91m";
    if (wcsncmp(tag, L"[ODS", 4) == 0) return L"\x1b[96m";
    if (wcsncmp(tag, L"[MOD", 4) == 0) return L"\x1b[90m";
    if (wcsncmp(tag, L"[console", 8) == 0) return L"\x1b[92m";
    if (wcsncmp(tag, L"[INFO", 5) == 0) return L"\x1b[92m";
    if (wcsncmp(tag, L"[THR", 4) == 0) return L"\x1b[95m";
    if (wcsncmp(tag, L"[MEM", 4) == 0) return L"\x1b[95m";
    if (wcsncmp(tag, L"[NET", 4) == 0) return L"\x1b[94m";
    if (wcsncmp(tag, L"[FS", 3) == 0) return L"\x1b[97m";
    if (wcsncmp(tag, L"[HB", 3) == 0) return L"\x1b[90m";
    if (wcsncmp(tag, L"[SD", 3) == 0) return L"\x1b[93m";
    if (wcsncmp(tag, L"[EXC", 4) == 0) return L"\x1b[33m";
    if (wcsncmp(tag, L"[HOOK", 5) == 0) return L"\x1b[35m";
    if (wcscmp(tag, L"[hooks.log]") == 0) return L"\x1b[35m";
    return L"\x1b[93m";
}

static int CategoryOn(const WCHAR* tag)
{
    if (wcsncmp(tag, L"[ODS", 4) == 0) return ShowOds;
    if (wcsncmp(tag, L"[CRASH", 6) == 0) return ShowCrash;
    if (wcsncmp(tag, L"[EXC", 4) == 0) return ShowExceptions;
    if (wcscmp(tag, L"[EH]") == 0) return ShowExceptions;
    if (wcscmp(tag, L"[MOD]") == 0) return ShowModules;
    if (wcscmp(tag, L"[THR]") == 0) return ShowThreads;
    if (wcscmp(tag, L"[MEM]") == 0) return ShowMemory;
    if (wcscmp(tag, L"[NET]") == 0) return ShowNetwork;
    if (wcscmp(tag, L"[FS]") == 0) return ShowFileSystem;
    if (wcscmp(tag, L"[HB]") == 0) return ShowHeartbeat;
    if (wcscmp(tag, L"[SD]") == 0) return ShowSharedData;
    if (wcscmp(tag, L"[INFO]") == 0) return ShowInfo;
    if (wcscmp(tag, L"[console]") == 0) return ShowConsoleMessages;
    if (wcscmp(tag, L"[SYS]") == 0 || wcscmp(tag, L"[KRNL]") == 0 || wcscmp(tag, L"[SVC]") == 0) return ShowSystem;
    if (wcsncmp(tag, L"[HOOK", 5) == 0) return ShowHooks;
    if (wcscmp(tag, L"[hooks.log]") == 0) return ShowHooks;
    return ShowLogFiles;
}

// Renders one console line exactly the way Write() does: colour, timestamp,
// tag, reset, text. Shared with the crash path so a synchronously written
// crash line is recognised as "ours" by the console scraper.
static void FormatTagLineEx(const WCHAR* tag, const WCHAR* text, WCHAR* out, int cch, int color)
{
    WCHAR tb[4096], ts[32];
    const WCHAR* col, *rst;
    size_t len;
    StringCchCopyW(tb, 4096, text ? text : L"");
    len = wcslen(tb);
    while (len > 0 && (tb[len - 1] == L'\r' || tb[len - 1] == L'\n')) tb[--len] = 0;

    ts[0] = 0;
    if (ShowTimestamps)
    {
        SYSTEMTIME t; GetLocalTime(&t);
        StringCchPrintfW(ts, 32, L"%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    }
    col = color ? Color(tag) : L"";
    rst = color ? L"\x1b[0m" : L"";
    StringCchPrintfW(out, (size_t)cch, L"%s%s%s%s %s\n", col, ts, tag, rst, tb);
}

static void FormatTagLine(const WCHAR* tag, const WCHAR* text, WCHAR* out, int cch)
{
    FormatTagLineEx(tag, text, out, cch, ShowColors);
}

static HANDLE g_logHandle = INVALID_HANDLE_VALUE;
static WCHAR g_logPath[520] = L"";
static DWORD g_conWidth = 0;   // console buffer width (cached at setup)

// Length in console CELLS (ANSI color escapes occupy none). Used to keep a
// tagged line from wrapping in the console: wrapped continuations would be
// mis-scraped as raw game output with a wrong timestamp.
static int VisibleLen(const WCHAR* s, int max)
{
    int cells = 0, i;
    for (i = 0; s[i]; i++)
    {
        if (s[i] == L'\r' || s[i] == L'\n') return -1;   // line ends within the cap
        if (s[i] == 0x1b)
        {
            while (s[i] && s[i] != L'm') i++;   // skip CSI ... m
            if (!s[i]) break;
            continue;
        }
        if (++cells >= max) return i;           // string index where it fits
    }
    return -1;                                  // fits without truncation
}

static void Write(const WCHAR* tag, const WCHAR* text)
{
    WCHAR line[4096], plain[4096];
    char utf[16384], putf[16384];
    int n, pn; DWORD w;
    if (!CategoryOn(tag)) return;
    FormatTagLine(tag, text, line, 4096);
    // keep the console row from wrapping (see VisibleLen) - the saved log
    // gets the full untruncated plain-text line below regardless
    if (g_conWidth > 8)
    {
        int cut = VisibleLen(line, (int)g_conWidth - 1);
        if (cut >= 0)
        {
            line[cut] = 0;
            StringCchCatW(line, 4096, L"\n");   // the cut dropped the terminator
        }
    }
    n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf, sizeof(utf) - 1, NULL, NULL);
    if (n <= 1) return;
    if (g_wrLockInit) EnterCriticalSection(&g_wrLock);
    // tagged lines are appended to the saved log DIRECTLY - the console
    // scrape only relays raw game output. This makes the log immune to
    // console-buffer wipes, cursor jumps and wrap re-scrapes. SaveTaggedLines
    // still controls whether GameConsole's own lines are saved at all. The
    // log copy is PLAIN TEXT: no ANSI color escapes, no console-width
    // truncation - the log is the artifact of record.
    if (SaveLogFile && SaveTaggedLines && g_logHandle != INVALID_HANDLE_VALUE)
    {
        FormatTagLineEx(tag, text, plain, 4096, 0);
        pn = WideCharToMultiByte(CP_UTF8, 0, plain, -1, putf, sizeof(putf) - 1, NULL, NULL);
        if (pn > 1) WriteFile(g_logHandle, putf, (DWORD)(pn - 1), &w, NULL);
    }
    if (g_hOut)
        WriteFile(g_hOut, utf, (DWORD)(n - 1), &w, NULL);
    if (g_wrLockInit) LeaveCriticalSection(&g_wrLock);
}

// hook-line writes from the install path (worker thread): a normal [HOOK] line.
static void HookNote(const WCHAR* msg) { Write(L"[HOOK]", msg); }

// ---------------------------------------------------------------- trace / crash file
static void TempPath(WCHAR* out, int cch, const WCHAR* file)
{
    GetTempPathW(cch, out);
    StringCchCatW(out, cch, file);
}

static void Trace(const WCHAR* msg)
{
    WCHAR path[520], line[1400]; char utf[4096]; int n; SYSTEMTIME t; HANDLE h; DWORD w;
    if (!WriteTraceFile) return;
    TempPath(path, 520, L"GameConsole.log");
    h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (h == INVALID_HANDLE_VALUE) return; SetFilePointer(h, 0, NULL, FILE_END); }
    GetLocalTime(&t);
    StringCchPrintfW(line, 1400, L"%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf, sizeof(utf) - 1, NULL, NULL);
    if (n > 1) WriteFile(h, utf, (DWORD)(n - 1), &w, NULL);
    CloseHandle(h);
}

static void CrashFileAppend(const WCHAR* msg)
{
    WCHAR path[520], line[2600]; char utf[8192]; int n; SYSTEMTIME t; HANDLE h; DWORD w;
    if (!WriteCrashFile) return;
    TempPath(path, 520, L"GameConsole_crash.log");
    h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (h == INVALID_HANDLE_VALUE) return; SetFilePointer(h, 0, NULL, FILE_END); }
    GetLocalTime(&t);
    StringCchPrintfW(line, 2600, L"%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf, sizeof(utf) - 1, NULL, NULL);
    if (n > 1) WriteFile(h, utf, (DWORD)(n - 1), &w, NULL);
    CloseHandle(h);
}

// ---------------------------------------------------------------- describe
static int CallerRangeFor(ULONG_PTR addr, ULONG_PTR* base, ULONG_PTR* end, const WCHAR** name, int* sys);

// Lock-free (caller-range table); may run inside hooks.
static void Describe(void* addr, WCHAR* out, int cch)
{
    ULONG_PTR base = 0;
    const WCHAR* name = NULL;
    if (CallerRangeFor((ULONG_PTR)addr, &base, NULL, &name, NULL) && name && name[0])
        StringCchPrintfW(out, cch, L"%s+0x%llX", name, (unsigned long long)((ULONG_PTR)addr - base));
    else
        StringCchPrintfW(out, cch, L"0x%llX (no module)", (unsigned long long)(ULONG_PTR)addr);
}

static int IsPlumbingName(const WCHAR* name)
{
    static const WCHAR* skip[] = {
        L"ntdll.dll", L"KERNELBASE.dll", L"kernel32.dll", L"win32u.dll", L"ucrtbase.dll",
        L"VCRUNTIME140.dll", L"VCRUNTIME140_1.dll", L"MSVCP140.dll", L"combase.dll",
        L"rpcrt4.dll", L"mscoree.dll", L"GameConsole.dll", L"GameHooks.dll", NULL };
    int i;
    for (i = 0; skip[i]; i++) if (_wcsicmp(name, skip[i]) == 0) return 1;
    return 0;
}

static void FirstCaller(const unsigned __int64* frames, WCHAR* out, int cch)
{
    int i;
    for (i = 0; i < 8; i++)
    {
        WCHAR d[560], nm[560]; const WCHAR* p; int k = 0;
        if (!frames[i]) break;
        Describe((void*)(ULONG_PTR)frames[i], d, 560);
        for (p = d; *p && *p != L'+' && k < 559; p++) nm[k++] = *p;
        nm[k] = 0;
        if (IsPlumbingName(nm)) continue;
        StringCchCopyW(out, cch, d);
        return;
    }
    StringCchCopyW(out, cch, L"?");
}

static int IsSystemPath(const WCHAR* p)
{
    if (!p || !g_sysDir[0]) return 0;
    return _wcsnicmp(p, g_sysDir, wcslen(g_sysDir)) == 0;
}

static int IsCrashCode(unsigned c)
{
    return c == 0xC0000005u || c == 0xC000001Du || c == 0xC0000094u || c == 0xC0000096u ||
           c == 0xC00000FDu || c == 0xC0000374u || c == 0xC0000409u;
}
static const WCHAR* ExcName(unsigned c)
{
    switch (c)
    {
    case 0xC0000005u: return L"ACCESS_VIOLATION";
    case 0xC000001Du: return L"ILLEGAL_INSTRUCTION";
    case 0xC0000094u: return L"INT_DIVIDE_BY_ZERO";
    case 0xC0000096u: return L"PRIV_INSTRUCTION";
    case 0xC00000FDu: return L"STACK_OVERFLOW";
    case 0xC0000374u: return L"HEAP_CORRUPTION";
    case 0xC0000409u: return L"STACK_BUFFER_OVERRUN";
    default: return L"OTHER";
    }
}

static void StringCchLowerW(WCHAR* s)
{
    for (; *s; s++) *s = (WCHAR)towlower(*s);
}

// ---------------------------------------------------------------- capture helpers
typedef USHORT (NTAPI* t_CaptureStack)(ULONG, ULONG, void**, ULONG*);
static t_CaptureStack pStack = NULL;

static void CaptureFrames(unsigned __int64* out8)
{
    void* fr[12]; USHORT n; int i, k = 0;
    if (!pStack) pStack = (t_CaptureStack)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "RtlCaptureStackBackTrace");
    n = pStack ? pStack(0, 12, fr, NULL) : 0;
    for (i = 2; i < (int)n && k < 8; i++) out8[k++] = (unsigned __int64)fr[i];
    while (k < 8) out8[k++] = 0;
}

static __declspec(thread) int inVeh = 0;
static __declspec(thread) int inHook = 0;

// Registry lines emitted on this thread. The KERNELBASE layer and the
// module-level registry bridge both watch the same operations, so the bridge
// snapshots this around a call and stays quiet when the layer already reported
// it (a shadow KERNELBASE#1 route does not bump it, which is the case the
// bridge exists for).
static __declspec(thread) int t_regLogged = 0;
static int g_ods = 1, g_hold = 0, g_vehOn = 0;
static void* g_vehCookie = NULL;          // node returned by AddVectoredExceptionHandler
static volatile LONG g_crashSynced = 0;   // UEH already wrote the report synchronously

// A fully formatted crash report, matching the C# FormatCrashEvent output.
static void FormatCrash(const GCEv* e, WCHAR* out, int cch)
{
    WCHAR d[560], tmp[600];
    int i;
    out[0] = 0;
    StringCchPrintfW(tmp, 600, L"UNHANDLED 0x%08X %s at ", (unsigned)e->code, ExcName((unsigned)e->code));
    StringCchCatW(out, cch, tmp);
    Describe((void*)(ULONG_PTR)e->addr, d, 560);
    StringCchCatW(out, cch, d);
    StringCchPrintfW(tmp, 600, L", thread %lu", e->tid);
    StringCchCatW(out, cch, tmp);
    if (e->code == 0xC0000005u)
    {
        const WCHAR* kind = e->info1 == 0 ? L"read" : e->info1 == 1 ? L"write" : L"execute";
        StringCchPrintfW(tmp, 600, L", %s of 0x%llX", kind, (unsigned long long)e->info2);
        StringCchCatW(out, cch, tmp);
    }
    for (i = 0; i < 8 && e->frames[i] != 0; i++)
    {
        StringCchCatW(out, cch, L"\n    ");
        Describe((void*)(ULONG_PTR)e->frames[i], d, 560);
        StringCchCatW(out, cch, d);
    }
}

// ---------------------------------------------------------------- VEH / UEH / module notify
static LONG NTAPI VehNative(EXCEPTION_POINTERS* p)
{
    if (inVeh) return 0;
    inVeh = 1;
    {
        PEXCEPTION_RECORD r = p->ExceptionRecord;
        ULONG code = r->ExceptionCode;
        if (code != 0x406D1388)   // SetThreadName chatter: never record
        {
            GCEv e; memset(&e, 0, sizeof(e));
            e.tid = GetCurrentThreadId();
            e.code = code;
            CaptureFrames(e.frames);
            if ((code == 0x40010006 || code == 0x4001000A) && r->NumberParameters >= 2)
            {
                if (g_ods)
                {
                    ULONG_PTR len = r->ExceptionInformation[0];
                    const char* s = (const char*)r->ExceptionInformation[1];
                    if (s && len > 1 && len < 0x10000)
                    {
                        ULONG cb;
                        e.kind = (code == 0x4001000A) ? 1u : 0u;
                        cb = (ULONG)(len - 1) * (e.kind ? 2 : 1);
                        if (e.kind && cb > 2044) cb = 2044; else if (!e.kind && cb > 2046) cb = 2046;
                        memcpy(e.text, s, cb);
                        ((char*)e.text)[cb] = 0; ((char*)e.text)[cb + 1] = 0;
                        EvPush(&e);
                    }
                }
            }
            else
            {
                e.kind = 2;
                e.addr = (unsigned __int64)r->ExceptionAddress;
                if (code == 0xC0000005 && r->NumberParameters >= 2)
                { e.info1 = r->ExceptionInformation[0]; e.info2 = r->ExceptionInformation[1]; }
                if ((code == 0x80000004u || code == 0x80000003u) && p->ContextRecord)
                {
                    // Snapshot the debug registers at the trap: on a single-step,
                    // Dr6 says WHY (bit 14 = single-step, bits 0-3 = which Dr fired)
                    // and Dr0-3/Dr7 say what was armed. This is how a hardware
                    // breakpoint "firing" is distinguished from one merely armed.
                    const BYTE* c = (const BYTE*)p->ContextRecord;
                    __try
                    {
                        e.dr[0] = *(const DWORD64*)(c + 0x48);
                        e.dr[1] = *(const DWORD64*)(c + 0x50);
                        e.dr[2] = *(const DWORD64*)(c + 0x58);
                        e.dr[3] = *(const DWORD64*)(c + 0x60);
                        e.dr[4] = *(const DWORD64*)(c + 0x68);
                        e.dr[5] = *(const DWORD64*)(c + 0x70);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) { }
                }
                EvPush(&e);
            }
        }
    }
    inVeh = 0;
    return 0;   // EXCEPTION_CONTINUE_SEARCH
}

// Best-effort synchronous console line so a crash is visible even if the
// process dies before the worker drains the ring.
static void CrashWriteSync(const WCHAR* report, const WCHAR* terminate)
{
    WCHAR line[4400]; char utf[14000]; int n; DWORD w;
    if (!g_hOut || !CategoryOn(L"[CRASH]")) return;
    FormatTagLine(L"[CRASH]", report, line, 4400);
    n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf, sizeof(utf) - 1, NULL, NULL);
    if (n > 1) WriteFile(g_hOut, utf, (DWORD)(n - 1), &w, NULL);
    if (terminate)
    {
        FormatTagLine(L"[CRASH]", terminate, line, 4400);
        n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf, sizeof(utf) - 1, NULL, NULL);
        if (n > 1) WriteFile(g_hOut, utf, (DWORD)(n - 1), &w, NULL);
    }
}

static LONG NTAPI UehNative(EXCEPTION_POINTERS* p)
{
    PEXCEPTION_RECORD r = p->ExceptionRecord;
    GCEv e; memset(&e, 0, sizeof(e));
    e.tid = GetCurrentThreadId();
    e.code = r->ExceptionCode;
    e.kind = 3;
    e.addr = (unsigned __int64)r->ExceptionAddress;
    if (e.code == 0xC0000005 && r->NumberParameters >= 2)
    { e.info1 = r->ExceptionInformation[0]; e.info2 = r->ExceptionInformation[1]; }
    CaptureFrames(e.frames);
    EvPush(&e);
    g_crashSynced = 1;   // the worker's drain must not print this one again

    __try
    {
        WCHAR report[2048], term[300];
        FormatCrash(&e, report, 2048);
        StringCchPrintfW(term, 300, L"process is about to terminate%s",
                         g_hold ? L" (held open, close this window or kill the game)" : L"");
        CrashWriteSync(report, term);
        CrashFileAppend(report);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }

    if (g_hold) Sleep(INFINITE);   // HoldOnCrash: keep the console window up
    return 0;   // EXCEPTION_CONTINUE_SEARCH
}

typedef struct { USHORT Len, Max; WCHAR* Buf; } UNI;
typedef struct { ULONG Flags; UNI* FullDllName; UNI* BaseDllName; void* DllBase; ULONG SizeOfImage; } LDRDATA;

static void NTAPI ModNotify(ULONG reason, void* pData, void* ctx)
{
    (void)ctx;
    if (!pData) return;
    {
        LDRDATA* d = (LDRDATA*)pData;
        GCEv e; memset(&e, 0, sizeof(e));
        UNI* src;
        e.tid = GetCurrentThreadId();
        e.kind = (reason == 1) ? 4u : 5u;
        e.addr = (unsigned __int64)d->DllBase;
        src = d->FullDllName ? d->FullDllName : d->BaseDllName;
        if (src && src->Buf && src->Len)
        {
            ULONG cb = src->Len;
            if (cb > 2044) cb = 2044;
            memcpy(e.text, src->Buf, cb);
            ((char*)e.text)[cb] = 0; ((char*)e.text)[cb + 1] = 0;
        }
        EvPush(&e);
    }
}

// ---------------------------------------------------------------- hook layer (from GameHooks.c)
typedef LONG NTSTATUS;
typedef struct { USHORT Len, Max; WCHAR* Buf; } UNI2;
typedef struct { ULONG Len, Pad; HANDLE Root; UNI2* Name; ULONG Attr; } OA;
typedef struct { NTSTATUS Status; ULONG_PTR Information; } IOSB;

typedef NTSTATUS (NTAPI* t_NtOpenFile)  (PHANDLE, ACCESS_MASK, OA*, IOSB*, ULONG, ULONG);
typedef NTSTATUS (NTAPI* t_NtCreateFile)(PHANDLE, ACCESS_MASK, OA*, IOSB*, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef NTSTATUS (NTAPI* t_NtRw)        (HANDLE, HANDLE, void*, void*, IOSB*, void*, ULONG, PLARGE_INTEGER, PULONG);
typedef NTSTATUS (NTAPI* t_NtProtect)   (HANDLE, void**, SIZE_T*, ULONG, PULONG);
typedef LONG     (NTAPI* t_NtQueryInfoFile)(HANDLE, IOSB*, void*, ULONG, int);

static void* tOpen = NULL;
static void* tCreate = NULL;
static void* tRead = NULL;
static void* tWrite = NULL;
static void* tProtect = NULL;

static t_NtQueryInfoFile pQIF = NULL;

static volatile LONG g_count = 0;
static LONG g_cap = 2000;
static int g_reads = 1, g_writes = 1, g_prot = 1, g_onlyGame = 1;
static WCHAR g_filter[600] = L"";
static WCHAR g_win[300] = L"";
static int g_hooksActive = 0;
static BYTE* trampNext = NULL;
static BYTE* trampEnd  = NULL;

static HANDLE g_susp[1024]; static DWORD g_suspTid[1024]; static int g_nsusp = 0;

// Push a fully formatted hook line into the ring. Called from hook handlers on
// arbitrary game threads: no file I/O, no allocation, no runtime entry.
static void EmitHook(const WCHAR* msg)
{
    GCEv e; size_t len;
    if (!g_hooksActive || !g_hOut) return;
    if (InterlockedIncrement(&g_count) > g_cap) return;
    memset(&e, 0, sizeof(e));
    e.tid = GetCurrentThreadId();
    e.kind = 6;
    len = wcslen(msg);
    if (len > 1022) len = 1022;
    memcpy(e.text, msg, len * sizeof(WCHAR));
    e.text[len] = 0;
    EvPush(&e);
}

static int IsSkipName(const WCHAR* n)
{
    static const WCHAR* skip[] = {
        L"ntdll.dll", L"kernelbase.dll", L"kernel32.dll", L"win32u.dll", L"ucrtbase.dll",
        L"vcruntime140.dll", L"vcruntime140_1.dll", L"msvcp140.dll", L"combase.dll",
        L"rpcrt4.dll", L"mscoree.dll", L"apphelp.dll", L"gameconsole.dll", L"gamehooks.dll", NULL };
    int i;
    for (i = 0; skip[i]; i++) if (_wcsicmp(n, skip[i]) == 0) return 1;
    return 0;
}

// ---------------------------------------------------------------- caller ranges
// Caller attribution must be LOCK-FREE. This runs inside hooks, and
// GetModuleHandleExW / GetModuleFileNameW take the loader lock: a loader thread
// that holds that lock and waits on a thread sitting in one of our hooks
// deadlocks, and the game hangs (seen as a hang at startup once the exe's own
// IAT and the KERNELBASE layer were instrumented). So the worker keeps a plain
// table of module ranges (base/end/name) and the hook path only reads it.
// A caller outside the table (a hidden module such as dse.dll, which is in no
// loader list) is queued BY ADDRESS; the worker resolves it with the loader
// APIs - safe there - reports it, adds it to the table so every later call is
// named too, and instruments it if it is a pending HookModules target.
typedef struct { ULONG_PTR base, end; WCHAR name[64]; int sys, manual; } CallerRange;
#define MAX_CRANGE 768
static CallerRange g_crange[MAX_CRANGE];
static volatile LONG g_nCrange = 0;

static int CallerRangeFor(ULONG_PTR addr, ULONG_PTR* base, ULONG_PTR* end, const WCHAR** name, int* sys)
{
    int i, n = (int)g_nCrange;
    if (n > MAX_CRANGE) n = MAX_CRANGE;
    for (i = 0; i < n; i++)
    {
        ULONG_PTR b = g_crange[i].base;
        if (addr >= b && addr < g_crange[i].end)
        {
            if (base) *base = b;
            if (end) *end = g_crange[i].end;
            if (name) *name = g_crange[i].name;
            if (sys) *sys = g_crange[i].sys;
            return 1;
        }
    }
    return 0;
}

// worker thread only
static void CallerAddRange(ULONG_PTR base, ULONG_PTR size, const WCHAR* name, int sys, int manual)
{
    int n = (int)g_nCrange, i;
    if (!base) return;
    for (i = 0; i < n; i++) if (g_crange[i].base == base) return;
    if (n >= MAX_CRANGE) return;
    g_crange[n].base = base;
    g_crange[n].end = base + (size ? size : 0x1000);
    StringCchCopyW(g_crange[n].name, 64, (name && name[0]) ? name : L"(unknown)");
    g_crange[n].sys = sys;
    g_crange[n].manual = manual;
    InterlockedIncrement(&g_nCrange);          // publish
}

// worker thread only: rebuild from toolhelp (which does list the Denuvo shadow
// copies, under their real system names) while keeping resolved hidden modules
static void RefreshCallerRanges(void)
{
    CallerRange keep[64];
    int nk = 0, n = 0, i;
    HANDLE snap;
    MODULEENTRY32W me;
    if (!g_win[0]) GetWindowsDirectoryW(g_win, 300);
    if (!g_pid) g_pid = GetCurrentProcessId();
    for (i = 0; i < (int)g_nCrange && nk < 64; i++)
        if (g_crange[i].manual) keep[nk++] = g_crange[i];
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, g_pid);
    if (snap != INVALID_HANDLE_VALUE)
    {
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me))
        {
            do
            {
                if (n < MAX_CRANGE)
                {
                    g_crange[n].base = (ULONG_PTR)me.hModule;
                    g_crange[n].end = (ULONG_PTR)me.hModule + me.modBaseSize;
                    StringCchCopyW(g_crange[n].name, 64, me.szModule);
                    g_crange[n].sys = (g_win[0] && _wcsnicmp(me.szExePath, g_win, wcslen(g_win)) == 0);
                    g_crange[n].manual = 0;
                    n++;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    for (i = 0; i < nk && n < MAX_CRANGE; i++)
    {
        int j, dup = 0;
        for (j = 0; j < n; j++) if (g_crange[j].base == keep[i].base) { dup = 1; break; }
        if (!dup) g_crange[n++] = keep[i];
    }
    if (n) InterlockedExchange(&g_nCrange, n);
}

// Hook path: remember an unresolved caller address. No locks, no loader calls.
#define MAX_CADDR 16
static ULONG_PTR g_caddr[MAX_CADDR];
static volatile LONG g_nCaddr = 0;

static void NoteCallerAddr(ULONG_PTR addr)
{
    LONG n = g_nCaddr;
    int i;
    if (!addr || n >= MAX_CADDR) return;
    for (i = 0; i < n; i++) if (g_caddr[i] == addr) return;
    g_caddr[n] = addr;
    InterlockedIncrement(&g_nCaddr);
}

// Lock-free: no loader-lock calls here (see the caller-range table above).
static int DescribeCaller(WCHAR* out, int cch)
{
    void* frames[12];
    USHORT n, i;
    if (!pStack) return 0;
    n = pStack(0, 12, frames, NULL);
    for (i = 1; i < n; i++)
    {
        ULONG_PTR base = 0;
        const WCHAR* name = NULL;
        int sys = 0;
        NoteCallerAddr((ULONG_PTR)frames[i]);          // queued for the worker
        if (!CallerRangeFor((ULONG_PTR)frames[i], &base, NULL, &name, &sys)) continue;
        if (!name || !name[0]) continue;
        if (IsSkipName(name)) continue;
        if (g_onlyGame && sys) continue;
        if (g_filter[0])
        {
            int ok = 0; const WCHAR* p = g_filter;
            while (*p)
            {
                const WCHAR* e2; int cl;
                while (*p == L' ') p++;
                e2 = wcschr(p, L',');
                cl = e2 ? (int)(e2 - p) : (int)wcslen(p);
                while (cl > 0 && p[cl - 1] == L' ') cl--;
                if (cl > 0 && _wcsnicmp(name, p, cl) == 0 && name[cl] == 0) { ok = 1; break; }
                p = e2 ? e2 + 1 : p + wcslen(p);
            }
            if (!ok) continue;
        }
        StringCchPrintfW(out, cch, L"%s+0x%llX", name,
                         (unsigned long long)((ULONG_PTR)frames[i] - base));
        return 1;
    }
    return 0;
}

static const WCHAR* NtPath(const WCHAR* p)
{
    if (p[0] == L'\\' && p[1] == L'?' && p[2] == L'?' && p[3] == L'\\') return p + 4;
    return p;
}

static void HandleName(HANDLE h, WCHAR* out, int cch)
{
    char raw[600]; IOSB iob; ULONG chars;
    out[0] = L'?'; out[1] = 0;
    if (!h || !pQIF) return;
    if (pQIF(h, &iob, raw, sizeof(raw), 9 /*FileNameInformation*/) != 0) return;
    chars = *(ULONG*)raw / 2;
    if (!chars) return;
    if (chars > (sizeof(raw) - 4) / 2) chars = (sizeof(raw) - 4) / 2;
    if (chars > (ULONG)(cch - 1)) chars = (ULONG)(cch - 1);
    memcpy(out, raw + 4, chars * 2);
    out[chars] = 0;
}

static NTSTATUS NTAPI HkOpenFile(PHANDLE fh, ACCESS_MASK acc, OA* oa, IOSB* iosb, ULONG share, ULONG opts)
{
    NTSTATUS st = ((t_NtOpenFile)tOpen)(fh, acc, oa, iosb, share, opts);
    if (!inHook && g_writes && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1800], path[760];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            if (oa && oa->Name && oa->Name->Buf && oa->Name->Len)
                StringCchPrintfW(path, 760, L"%.*s", (int)(oa->Name->Len / 2), oa->Name->Buf);
            else { path[0] = L'?'; path[1] = 0; }
            StringCchPrintfW(msg, 1800, L"open   st=0x%08X acc=0x%08X \"%s\"  <- %s",
                             (unsigned)st, (unsigned)acc, NtPath(path), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static NTSTATUS NTAPI HkCreateFile(PHANDLE fh, ACCESS_MASK acc, OA* oa, IOSB* iosb, LARGE_INTEGER* alloc,
                                   ULONG attr, ULONG share, ULONG disp, ULONG opts, void* ea, ULONG eaLen)
{
    NTSTATUS st = ((t_NtCreateFile)tCreate)(fh, acc, oa, iosb, alloc, attr, share, disp, opts, ea, eaLen);
    if (!inHook && g_writes && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1800], path[760];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            if (oa && oa->Name && oa->Name->Buf && oa->Name->Len)
                StringCchPrintfW(path, 760, L"%.*s", (int)(oa->Name->Len / 2), oa->Name->Buf);
            else { path[0] = L'?'; path[1] = 0; }
            StringCchPrintfW(msg, 1800, L"create st=0x%08X acc=0x%08X \"%s\"  <- %s",
                             (unsigned)st, (unsigned)acc, NtPath(path), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static NTSTATUS NTAPI HkReadFile(HANDLE h, HANDLE ev, void* apc, void* apcCtx, IOSB* iosb,
                                 void* buf, ULONG len, LARGE_INTEGER* off, ULONG* key)
{
    NTSTATUS st = ((t_NtRw)tRead)(h, ev, apc, apcCtx, iosb, buf, len, off, key);
    if (!inHook && g_reads && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1500], path[400];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            unsigned long long got;
            HandleName(h, path, 400);
            got = (st >= 0 && iosb) ? (unsigned long long)iosb->Information : 0;
            StringCchPrintfW(msg, 1500, L"read   st=0x%08X got=%llu \"%s\"  <- %s", (unsigned)st, got, path, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static NTSTATUS NTAPI HkWriteFile(HANDLE h, HANDLE ev, void* apc, void* apcCtx, IOSB* iosb,
                                  void* buf, ULONG len, LARGE_INTEGER* off, ULONG* key)
{
    NTSTATUS st = ((t_NtRw)tWrite)(h, ev, apc, apcCtx, iosb, buf, len, off, key);
    if (!inHook && g_writes && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1500], path[400];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            unsigned long long got;
            HandleName(h, path, 400);
            got = (st >= 0 && iosb) ? (unsigned long long)iosb->Information : 0;
            StringCchPrintfW(msg, 1500, L"write  st=0x%08X got=%llu \"%s\"  <- %s", (unsigned)st, got, path, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static NTSTATUS NTAPI HkProtect(HANDLE proc, void** base, SIZE_T* size, ULONG newp, ULONG* oldp)
{
    NTSTATUS st = ((t_NtProtect)tProtect)(proc, base, size, newp, oldp);
    if (!inHook && g_prot && g_hooksActive && g_count < g_cap && base && size && oldp && ((newp | *oldp) & 0xF0) != 0)
    {
        WCHAR caller[300], msg[600];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 600, L"prot   st=0x%08X @0x%llX+0x%llX old=0x%X new=0x%X  <- %s",
                             (unsigned)st, (unsigned long long)(ULONG_PTR)*base,
                             (unsigned long long)*size, (unsigned)*oldp, (unsigned)newp, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

// ---------------------------------------------------------------- IAT hooks
typedef HANDLE (WINAPI* t_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL   (WINAPI* t_ReadFileW)  (HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL   (WINAPI* t_WriteFileW) (HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL   (WINAPI* t_DeleteFileW)(LPCWSTR);
typedef LONG     (NTAPI* t_NtRwIat)  (HANDLE, HANDLE, void*, void*, void*, void*, ULONG, void*, ULONG*);

static t_CreateFileW pCreateFileW_ = NULL;
static t_ReadFileW   pReadFileW_   = NULL;
static t_WriteFileW  pWriteFileW_  = NULL;
static t_DeleteFileW pDeleteFileW_ = NULL;
static t_NtRwIat     pIatNtRead_   = NULL;
static t_NtRwIat     pIatNtWrite_  = NULL;
typedef void* (WINAPI* t_FindFirstFileExW)(LPCWSTR, int, void*, int, void*, DWORD);
static t_FindFirstFileExW pFFF_ = NULL;
typedef HMODULE (WINAPI* t_LoadLibW)(LPCWSTR);
typedef HMODULE (WINAPI* t_LoadLibA)(LPCSTR);
static t_LoadLibW pLoadLibW_ = NULL;
static t_LoadLibA pLoadLibA_ = NULL;
typedef FARPROC (WINAPI* t_GetProcAddr)(HMODULE, LPCSTR);
static t_GetProcAddr pGPA_ = NULL;
typedef LONG     (NTAPI* t_NtOpenFileIat)(void*, ULONG, void*, void*, ULONG, ULONG);
static t_NtOpenFileIat pIatNtOpen_ = NULL;
typedef LONG     (NTAPI* t_NtTermIat)(HANDLE, LONG);
static t_NtTermIat pIatNtTerm_ = NULL;
typedef LONG     (NTAPI* t_NtCreateIat)(void*, ULONG, void*, void*, void*, ULONG, ULONG, ULONG, ULONG, void*, ULONG);
static t_NtCreateIat pIatNtCreate_ = NULL;
typedef LONG     (NTAPI* t_NtProtIat)(HANDLE, void**, SIZE_T*, ULONG, ULONG*);
static t_NtProtIat pIatNtProt_ = NULL;
static volatile LONG g_rwKb = 0;
typedef void*    (WINAPI* t_CreateServiceW)(void*, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD, DWORD, LPCWSTR, ...);
static t_CreateServiceW pCreateSvc_ = NULL;
typedef BOOL     (WINAPI* t_StartServiceW)(void*, DWORD, void*);
static t_StartServiceW pStartSvc_ = NULL;
typedef LPVOID   (WINAPI* t_VirtualAlloc)(LPVOID, SIZE_T, DWORD, DWORD);
static t_VirtualAlloc pVAlloc_ = NULL;
typedef BOOL     (WINAPI* t_VirtualProtect)(LPVOID, SIZE_T, DWORD, PDWORD);
static t_VirtualProtect pVProt_ = NULL;
typedef BOOL     (WINAPI* t_VirtualFree)(LPVOID, SIZE_T, DWORD);
static t_VirtualFree pVFree_ = NULL;
typedef BOOL     (WINAPI* t_CreateProcessW)(LPCWSTR, LPWSTR, void*, void*, BOOL, DWORD, void*, LPCWSTR, void*, void*);
static t_CreateProcessW pCProc_ = NULL;
typedef void*    (WINAPI* t_ShellExecuteW)(void*, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, int);
static t_ShellExecuteW pShExec_ = NULL;
typedef BOOL     (WINAPI* t_SetThreadContext)(HANDLE, const void*);
static t_SetThreadContext pSetTC_ = NULL;
typedef BOOL     (WINAPI* t_GetThreadContext)(HANDLE, void*);
static t_GetThreadContext pGetTC_ = NULL;
typedef DWORD    (WINAPI* t_SuspendThread)(HANDLE);
static t_SuspendThread pSuspT_ = NULL;
typedef DWORD    (WINAPI* t_ResumeThread)(HANDLE);
static t_ResumeThread pResT_ = NULL;
typedef HANDLE   (WINAPI* t_OpenThread)(DWORD, BOOL, DWORD);
static t_OpenThread pOpenT_ = NULL;
typedef BOOL     (WINAPI* t_TerminateProcess)(HANDLE, UINT);
static t_TerminateProcess pTermP_ = NULL;
typedef BOOL     (WINAPI* t_CopyFileExW)(LPCWSTR, LPCWSTR, void*, void*, BOOL*, DWORD);
static t_CopyFileExW pCopyF_ = NULL;
typedef void*    (WINAPI* t_DeleteService)(void*);
static t_DeleteService pDelSvc_ = NULL;
typedef void*    (WINAPI* t_OpenSCManagerW)(LPCWSTR, LPCWSTR, DWORD);
static t_OpenSCManagerW pOpenSCM_ = NULL;
typedef int      (WINAPI* t_MessageBoxW)(void*, LPCWSTR, LPCWSTR, UINT);
static t_MessageBoxW pMsgBox_ = NULL;
typedef BOOL     (WINAPI* t_IsDebuggerPresent)(void);
static t_IsDebuggerPresent pIsDbg_ = NULL;
typedef BOOL     (WINAPI* t_CreateDirectoryW)(LPCWSTR, void*);
static t_CreateDirectoryW pCreateDir_ = NULL;

static HANDLE WINAPI HkIatCreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    HANDLE h = pCreateFileW_(name, acc, share, sa, disp, flags, tmpl);
    if (!inHook && g_writes && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1800];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            DWORD err = (h == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
            StringCchPrintfW(msg, 1800, L"create st=0x%08X acc=0x%08X \"%s\"  <- %s",
                             (unsigned)err, (unsigned)acc, (name && *name) ? NtPath(name) : L"?", caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return h;
}

static BOOL WINAPI HkIatReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD got, LPOVERLAPPED ov)
{
    BOOL ok = pReadFileW_(h, buf, len, got, ov);
    if (!inHook && g_reads && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1500], path[400];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            DWORD err; unsigned long long n;
            HandleName(h, path, 400);
            err = ok ? 0 : GetLastError();
            n = (ok && got) ? *got : 0;
            StringCchPrintfW(msg, 1500, L"read   st=0x%08X got=%llu \"%s\"  <- %s", (unsigned)err, n, path, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return ok;
}

static BOOL WINAPI HkIatWriteFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD got, LPOVERLAPPED ov)
{
    BOOL ok = pWriteFileW_(h, buf, len, got, ov);
    if (!inHook && g_writes && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1500], path[400];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            DWORD err; unsigned long long n;
            HandleName(h, path, 400);
            err = ok ? 0 : GetLastError();
            n = (ok && got) ? *got : 0;
            StringCchPrintfW(msg, 1500, L"write  st=0x%08X got=%llu \"%s\"  <- %s", (unsigned)err, n, path, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return ok;
}

static BOOL WINAPI HkIatDeleteFileW(LPCWSTR name)
{
    BOOL ok = pDeleteFileW_(name);
    if (!inHook && g_writes && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1200];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 1200, L"delete st=0x%08X \"%s\"  <- %s",
                             (unsigned)(ok ? 0 : GetLastError()), (name && *name) ? NtPath(name) : L"?", caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return ok;
}

static LONG WINAPI HkIatNtRead(HANDLE h, HANDLE ev, void* apc, void* apcCtx, void* iosb, void* buf, ULONG len, void* off, ULONG* key)
{
    LONG st = pIatNtRead_(h, ev, apc, apcCtx, iosb, buf, len, off, key);
    if (!inHook && g_reads && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1500], path[400];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            unsigned long long got;
            HandleName(h, path, 400);
            got = (st >= 0 && iosb) ? (unsigned long long)((IOSB*)iosb)->Information : 0;
            StringCchPrintfW(msg, 1500, L"read   st=0x%08X got=%llu \"%s\"  <- %s", (unsigned)st, got, path, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static LONG WINAPI HkIatNtWrite(HANDLE h, HANDLE ev, void* apc, void* apcCtx, void* iosb, void* buf, ULONG len, void* off, ULONG* key)
{
    LONG st = pIatNtWrite_(h, ev, apc, apcCtx, iosb, buf, len, off, key);
    if (!inHook && g_writes && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1500], path[400];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            unsigned long long got;
            HandleName(h, path, 400);
            got = (st >= 0 && iosb) ? (unsigned long long)((IOSB*)iosb)->Information : 0;
            StringCchPrintfW(msg, 1500, L"write  st=0x%08X got=%llu \"%s\"  <- %s", (unsigned)st, got, path, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static void* WINAPI HkIatFindFirstFileExW(LPCWSTR name, int infoLevel, void* findData, int searchOp, void* searchFilter, DWORD flags)
{
    void* r = pFFF_(name, infoLevel, findData, searchOp, searchFilter, flags);
    if (!inHook && g_writes && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1200];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            DWORD err = (r == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
            StringCchPrintfW(msg, 1200, L"find   st=0x%08X \"%s\"  <- %s",
                             (unsigned)err, (name && *name) ? NtPath(name) : L"?", caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

static HMODULE WINAPI HkIatLoadLibW(LPCWSTR name)
{
    HMODULE m = pLoadLibW_(name);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1200];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 1200, L"dllload st=0x%08X \"%s\"  <- %s",
                             (unsigned)(m ? 0 : GetLastError()), (name && *name) ? name : L"?", caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return m;
}

static HMODULE WINAPI HkIatLoadLibA(LPCSTR name)
{
    HMODULE m = pLoadLibA_(name);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], wname[300], msg[1200];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            MultiByteToWideChar(CP_UTF8, 0, name ? name : "", -1, wname, 300);
            StringCchPrintfW(msg, 1200, L"dllload st=0x%08X \"%s\"  <- %s",
                             (unsigned)(m ? 0 : GetLastError()), *wname ? wname : L"?", caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return m;
}

// ---- option 5: GetProcAddress logging thunks -------------------------------
// A resolved pointer is handed back as a tiny stub that (a) records the
// caller's return address and (b) counts the call, then jumps to the real
// function. The thunk lives in GameConsole's own RWX allocation - nothing in
// the game's or system's code pages is ever written. Resolved pointers that
// are not code (exec section check fails) are returned unchanged, so data
// exports keep working.
#define MAX_GPA_THUNKS 64
typedef struct {
    volatile LONG64 real;        // +0   the real function
    volatile LONG   calls;       // +8   call counter
    volatile LONG   reported;    // +12  calls already reported
    volatile LONG64 lastCaller;  // +16  return address of the most recent call
    char            name[80];    // +24  export name
} GpaThunkData;                  // 104 bytes each

static GpaThunkData* g_td = NULL;     // data blocks
static BYTE* g_tc = NULL;             // thunk code
static volatile LONG g_nThunksA = 0;
static int g_thunkInit = 0;
static int g_thunkPoolWarned = 0;

static void ThunkPoolInit(void)
{
    BYTE* p;
    if (g_thunkInit) return;
    g_thunkInit = 1;
    p = (BYTE*)VirtualAlloc(NULL, 0x4000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!p) return;
    g_td = (GpaThunkData*)p;
    g_tc = p + MAX_GPA_THUNKS * sizeof(GpaThunkData);   // code follows the data
}

static int ThunkNamesEq(const char* a, const char* b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// Returns f if it points into an executable section of a loaded image
// (i.e. it is almost certainly a function), else NULL (a data export).
static void* ThunkTargetIsCode(void* f)
{
    HMODULE img = NULL;
    IMAGE_DOS_HEADER* dos;
    IMAGE_NT_HEADERS* nth;
    IMAGE_SECTION_HEADER* sec;
    ULONG_PTR rva;
    int i;
    if (!f) return NULL;
    // caller-range table, not GetModuleHandleExW: this runs inside the
    // GetProcAddress hook, so it must not take the loader lock
    if (!CallerRangeFor((ULONG_PTR)f, (ULONG_PTR*)&img, NULL, NULL, NULL) || !img)
        return NULL;                                  // not in any image
    __try
    {
        dos = (IMAGE_DOS_HEADER*)img;
        nth = (IMAGE_NT_HEADERS*)((BYTE*)img + dos->e_lfanew);
        if (nth->Signature != IMAGE_NT_SIGNATURE) return NULL;
        rva = (BYTE*)f - (BYTE*)img;
        sec = IMAGE_FIRST_SECTION(nth);
        for (i = 0; i < nth->FileHeader.NumberOfSections; i++)
        {
            DWORD va = sec[i].VirtualAddress;
            DWORD vs = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
            if (rva >= va && rva < va + vs)
                return (sec[i].Characteristics & 0x20000000) ? f : NULL;   // MEM_EXECUTE
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return NULL;
}

// Returns the thunk address handing calls to real function f (named name),
// or NULL when thunking is not possible / the pool is full.
static void* ThunkFor(void* f, const char* name)
{
    int i, idx, k;
    BYTE* c;
    GpaThunkData* d;
    if (!ThunkTargetIsCode(f)) return NULL;
    ThunkPoolInit();
    if (!g_td) return NULL;
    for (i = 0; i < g_nThunksA; i++)
        if (ThunkNamesEq(g_td[i].name, name) && (void*)(ULONG_PTR)g_td[i].real == f)
            return (void*)(g_tc + i * 32);
    idx = (int)InterlockedIncrement(&g_nThunksA) - 1;
    if (idx >= MAX_GPA_THUNKS)
    {
        if (!g_thunkPoolWarned)
        {
            g_thunkPoolWarned = 1;
            EmitHook(L"gpa    thunk pool full - further resolutions return the real address");
        }
        return NULL;
    }
    d = &g_td[idx];
    d->real = (LONG64)(ULONG_PTR)f;
    for (k = 0; k < 79 && name[k]; k++) d->name[k] = name[k];
    d->name[k] = 0;
    c = g_tc + idx * 32;
    c[0]  = 0x50;                                        // push rax
    c[1]  = 0x48; c[2] = 0x8B; c[3] = 0x44; c[4] = 0x24; c[5] = 0x08;   // mov rax,[rsp+8] (return addr)
    c[6]  = 0x48; c[7] = 0x89; c[8] = 0x05;              // mov [rip+rel32],rax -> lastCaller
    *(INT32*)(c + 9)  = (INT32)((BYTE*)&d->lastCaller - (c + 13));
    c[13] = 0x58;                                        // pop rax
    c[14] = 0xF0; c[15] = 0xFF; c[16] = 0x05;            // lock inc dword [rip+rel32] -> calls
    *(INT32*)(c + 17) = (INT32)((BYTE*)&d->calls - (c + 21));
    c[21] = 0xFF; c[22] = 0x25;                          // jmp [rip+rel32] -> real
    *(INT32*)(c + 23) = (INT32)((BYTE*)&d->real - (c + 27));
    for (k = 27; k < 32; k++) c[k] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), c, 32);
    return (void*)c;
}

static FARPROC WINAPI HkIatGPA(HMODULE mod, LPCSTR name)
{
    FARPROC f = pGPA_(mod, name);
    void* thunk = NULL;
    if (GpaThunks && f && name && !inHook) thunk = ThunkFor((void*)f, name);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], wname[300], msg[1200];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            char modn[64]; modn[0] = 0;
            WCHAR wmod[80];
            if (mod && GetModuleFileNameA(mod, modn, 64))
            { char* s = strrchr(modn, '\\'); if (s) memmove(modn, s + 1, strlen(s + 1) + 1); }
            MultiByteToWideChar(CP_UTF8, 0, name ? name : "(ordinal)", -1, wname, 300);
            MultiByteToWideChar(CP_UTF8, 0, modn, -1, wmod, 80);
            StringCchPrintfW(msg, 1200, L"proc   \"%s\" from %s%s  <- %s", wname, *wmod ? wmod : L"?",
                             thunk ? L" (logging thunk handed out)" : L"", caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return thunk ? (FARPROC)thunk : f;
}

static LONG WINAPI HkIatNtOpen(void* fh, ULONG acc, OA* oa, void* iosb, ULONG share, ULONG opts)
{
    LONG st = pIatNtOpen_(fh, acc, oa, iosb, share, opts);
    if (!inHook && g_writes && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1800], path[760];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            if (oa && oa->Name && oa->Name->Buf && oa->Name->Len)
                StringCchPrintfW(path, 760, L"%.*s", (int)(oa->Name->Len / 2), oa->Name->Buf);
            else { path[0] = L'?'; path[1] = 0; }
            StringCchPrintfW(msg, 1800, L"open   st=0x%08X acc=0x%08X \"%s\"  <- %s",
                             (unsigned)st, (unsigned)acc, NtPath(path), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static LONG WINAPI HkIatKbNtCreate(void* fh, ULONG acc, OA* oa, void* iosb, void* alloc, ULONG attr,
                                   ULONG share, ULONG disp, ULONG opts, void* ea, ULONG eaLen)
{
    LONG st = pIatNtCreate_(fh, acc, oa, iosb, alloc, attr, share, disp, opts, ea, eaLen);
    if (!inHook && g_writes && g_hooksActive && g_count < g_cap && InterlockedIncrement(&g_rwKb) <= g_cap / 8)
    {
        WCHAR caller[300], msg[1800], path[760];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            if (oa && oa->Name && oa->Name->Buf && oa->Name->Len)
                StringCchPrintfW(path, 760, L"%.*s", (int)(oa->Name->Len / 2), oa->Name->Buf);
            else { path[0] = L'?'; path[1] = 0; }
            StringCchPrintfW(msg, 1800, L"create st=0x%08X acc=0x%08X \"%s\"  <- %s",
                             (unsigned)st, (unsigned)acc, NtPath(path), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static LONG WINAPI HkIatKbNtOpen(void* fh, ULONG acc, OA* oa, void* iosb, ULONG share, ULONG opts)
{
    LONG st = pIatNtOpen_(fh, acc, oa, iosb, share, opts);
    if (!inHook && g_writes && g_hooksActive && g_count < g_cap && InterlockedIncrement(&g_rwKb) <= g_cap / 8)
    {
        WCHAR caller[300], msg[1800], path[760];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            if (oa && oa->Name && oa->Name->Buf && oa->Name->Len)
                StringCchPrintfW(path, 760, L"%.*s", (int)(oa->Name->Len / 2), oa->Name->Buf);
            else { path[0] = L'?'; path[1] = 0; }
            StringCchPrintfW(msg, 1800, L"open   st=0x%08X acc=0x%08X \"%s\"  <- %s",
                             (unsigned)st, (unsigned)acc, NtPath(path), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static LONG WINAPI HkIatKbNtProt(HANDLE proc, void** base, SIZE_T* size, ULONG newp, ULONG* oldp)
{
    LONG st = pIatNtProt_(proc, base, size, newp, oldp);
    if (!inHook && g_hooksActive && g_count < g_cap && base && size && oldp && ((newp | *oldp) & 0xF0) != 0 && newp != *oldp)
    {
        WCHAR caller[300], msg[700];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 700, L"prot   st=0x%08X @0x%llX+0x%llX old=0x%X new=0x%X  <- %s",
                             (unsigned)st, (unsigned long long)(ULONG_PTR)*base,
                             (unsigned long long)*size, (unsigned)*oldp, (unsigned)newp, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

// ---- option 4: wider KERNELBASE interposition set -------------------------
// NtQueryInformationProcess: classes 7/30/31 are the classic debugger probes
// (ProcessDebugPort / ProcessDebugObjectHandle / ProcessDebugFlags).
// NtSetInformationThread: class 0x11 is ThreadHideFromDebugger.
// NtCreateThreadEx: createFlags bit 0x4 = hide-from-debugger thread.
// NtAllocateVirtualMemory: who is reserving executable memory.
typedef LONG (WINAPI* t_NtQIP)(HANDLE, ULONG, void*, ULONG, ULONG*);
typedef LONG (WINAPI* t_NtSIT)(HANDLE, ULONG, void*, ULONG);
typedef LONG (WINAPI* t_NtCTEX)(void**, ULONG, void*, HANDLE, void*, void*, ULONG, ULONG_PTR, SIZE_T, SIZE_T, void*);
typedef LONG (WINAPI* t_NtAVM)(HANDLE, void**, ULONG_PTR*, SIZE_T*, ULONG, ULONG);
static t_NtQIP  pIatNtQIP_ = NULL;
static t_NtSIT  pIatNtSIT_ = NULL;
static t_NtCTEX pIatNtCTEX_ = NULL;
static t_NtAVM  pIatNtAVM_ = NULL;

// NtCreateKey / NtSetValueKey: the registry is where a service and a kernel
// driver are registered (ImagePath, Type, Start), so the KERNELBASE layer
// covers it too. NtCreateKey is reported only when a key was actually CREATED
// (not merely opened) - low noise, and it points straight at "who installed
// this". NtSetValueKey reports the key (resolved with NtQueryKey), the value
// name and the data, so an ImagePath write names the .sys file.
// winternl.h's UNICODE_STRING is not available under WIN32_LEAN_AND_MEAN, so
// declare the same layout locally (a guarded string, not the loader's view).
typedef struct { USHORT Length, MaximumLength; WCHAR* Buffer; } GC_UNI;
typedef LONG (WINAPI* t_NtCreateKey)(HANDLE*, ULONG, void*, ULONG, GC_UNI*, ULONG, ULONG*);
typedef LONG (WINAPI* t_NtSetValueKey)(HANDLE, GC_UNI*, ULONG, ULONG, void*, ULONG);
typedef LONG (WINAPI* t_NtQueryKey)(HANDLE, ULONG, void*, ULONG, ULONG*);
static t_NtCreateKey pIatNtCKey_ = NULL;
static t_NtSetValueKey pIatNtSVK_ = NULL;
static t_NtQueryKey pNtQK_ = NULL;
static int WatchMatch(const WCHAR* text);           // defined with the service watch

typedef struct {
    ULONG Length;
    HANDLE RootDirectory;
    GC_UNI* ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} GC_OA2;

static void RegPathOfKey(HANDLE key, WCHAR* out, int cch)
{
    BYTE buf[1024];
    ULONG ret = 0, n;
    int off, c;
    out[0] = 0;
    // pNtQK_ is resolved once at startup (InstallKernelbaseInterposition).
    // Never call GetProcAddress here: this runs inside a registry hook, and the
    // hook path must not touch the loader (hard rule 6).
    if (!pNtQK_ || !key) return;
    if (pNtQK_(key, 3 /* KeyNameInformation */, buf, sizeof(buf), &ret) != 0) return;
    n = *(ULONG*)buf;                                  // length in bytes
    if (n > sizeof(buf) - 16) n = sizeof(buf) - 16;
    c = (int)(n / sizeof(WCHAR));
    if (c > cch - 1) c = cch - 1;
    if (c <= 0) return;
    // the name sits right after NameLength; tolerate a padded layout by picking
    // whichever candidate starts with a plausible path character
    for (off = 4; off <= 8; off += 4)
    {
        WCHAR f = 0;
        __try { f = *(WCHAR*)(buf + off); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        if (f == L'\\' || (f >= L'0' && f <= L'Z'))
        {
            __try
            {
                memcpy(out, buf + off, (size_t)c * sizeof(WCHAR));
                out[c] = 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
            return;
        }
    }
}

// One reporting path for a registry event, shared by the KERNELBASE layer and
// the module-level bridge below, so both produce identical lines. t_regLogged
// counts what has already been reported on this thread.
static void RegLogKeyEvent(LONG st, const WCHAR* path, DWORD d)
{
    if (inHook || !g_hooksActive || g_count >= g_cap) return;
    if (d != 1 /* REG_CREATED_NEW_KEY */ && !WatchMatch(path)) return;
    {
        WCHAR caller[300], msg[1000];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 1000, L"regkey st=0x%08X %s \"%s\"  <- %s",
                             (unsigned)st, d == 1 ? L"CREATED" : L"opened",
                             path && path[0] ? path : L"?", caller);
            EmitHook(msg);
            t_regLogged++;
        }
        inHook = 0;
    }
}

static void RegLogValueEvent(HANDLE key, LONG st, const WCHAR* valueName,
                             ULONG type, const void* data, ULONG size)
{
    WCHAR caller[300], msg[1400], path[520], vn[260], val[340];
    int c;
    if (inHook || !g_hooksActive || g_count >= g_cap) return;
    RegPathOfKey(key, path, 520);
    vn[0] = 0;
    if (valueName)
    {
        __try
        {
            c = (int)wcslen(valueName);
            if (c > 255) c = 255;
            memcpy(vn, valueName, (size_t)c * sizeof(WCHAR));
            vn[c] = 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { vn[0] = 0; }
    }
    val[0] = 0;
    if (data && size)
    {
        if ((type == 1 /* REG_SZ */ || type == 2 /* REG_EXPAND_SZ */) && size >= 2)
        {
            int nc = (int)(size / sizeof(WCHAR));
            if (nc > 160) nc = 160;
            __try
            {
                int k;
                for (k = 0; k < nc; k++)
                {
                    WCHAR ch = ((const WCHAR*)data)[k];
                    val[k] = (ch >= 0x20 && ch < 0x7F) || ch == 0 ? ch : L'.';
                    if (ch == 0) break;
                }
                val[k] = 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { val[0] = 0; }
        }
        else if (type == 4 /* REG_DWORD */ && size == 4)
        {
            __try { StringCchPrintfW(val, 340, L"%lu", (unsigned long)*(const DWORD*)data); }
            __except (EXCEPTION_EXECUTE_HANDLER) { val[0] = 0; }
        }
        else
        {
            // REG_BINARY and friends: bounded hex. StringCchPrintfW returns an
            // HRESULT, not a count, so the offset is advanced by hand.
            static const WCHAR hx[] = L"0123456789ABCDEF";
            int k, o2 = 0;
            __try
            {
                for (k = 0; k < 12 && (ULONG)k < size && o2 + 4 < 340; k++)
                {
                    BYTE b = ((const BYTE*)data)[k];
                    if (k) val[o2++] = L' ';
                    val[o2++] = hx[b >> 4];
                    val[o2++] = hx[b & 0xF];
                }
                val[o2] = 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { val[0] = 0; }
        }
    }
    inHook = 1;
    if (DescribeCaller(caller, 300))
    {
        StringCchPrintfW(msg, 1400, L"regset st=0x%08X \"%s\"%s%s type=%lu size=%lu%s%s  <- %s",
                         (unsigned)st, path[0] ? path : L"?",
                         vn[0] ? L"\\" : L"", vn,
                         (unsigned long)type, (unsigned long)size,
                         val[0] ? L" = " : L"", val, caller);
        EmitHook(msg);
        t_regLogged++;
    }
    inHook = 0;
}

// A key's path, taken from NtCreateKey's OBJECT_ATTRIBUTES.
static void RegLogKeyFromOa(LONG st, void* oa_, DWORD d)
{
    WCHAR path[520];
    path[0] = 0;
    __try
    {
        GC_OA2* oa = (GC_OA2*)oa_;
        if (oa && oa->ObjectName && oa->ObjectName->Buffer && oa->ObjectName->Length)
        {
            int n = oa->ObjectName->Length / (int)sizeof(WCHAR);
            if (n > 255) n = 255;
            memcpy(path, oa->ObjectName->Buffer, (size_t)n * sizeof(WCHAR));
            path[n] = 0;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { path[0] = 0; }
    RegLogKeyEvent(st, path, d);
}

// Same, from a value write's UNICODE_STRING name.
static void RegLogValueFromUni(HANDLE key, LONG st, GC_UNI* vname,
                               ULONG type, void* data, ULONG size)
{
    WCHAR vn[260];
    vn[0] = 0;
    __try
    {
        if (vname && vname->Buffer && vname->Length)
        {
            int c = vname->Length / (int)sizeof(WCHAR);
            if (c > 255) c = 255;
            memcpy(vn, vname->Buffer, (size_t)c * sizeof(WCHAR));
            vn[c] = 0;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { vn[0] = 0; }
    RegLogValueEvent(key, st, vn[0] ? vn : NULL, type, data, size);
}

static LONG WINAPI HkIatKbNtCreateKey(HANDLE* h, ULONG acc, void* oa_, ULONG idx,
                                      GC_UNI* cls, ULONG opts, ULONG* disp)
{
    LONG st = pIatNtCKey_(h, acc, oa_, idx, cls, opts, disp);
    RegLogKeyFromOa(st, oa_, disp ? *disp : 0);
    return st;
}

static LONG WINAPI HkIatKbNtSetValueKey(HANDLE key, GC_UNI* vname, ULONG idx,
                                        ULONG type, void* data, ULONG size)
{
    LONG st = pIatNtSVK_(key, vname, idx, type, data, size);
    RegLogValueFromUni(key, st, vname, type, data, size);
    return st;
}

// ---- module-level registry bridge ----------------------------------------
// The KERNELBASE layer above only sees the loader-loaded KERNELBASE image. A
// protection that routes through its own manual-mapped KERNELBASE#1/advapi32#1
// copies bypasses it completely, which is how a driver install could go
// unrecorded. These hook the same operations inside the *monitored module's own
// import table* (hard rule 1: import tables only), which the shadow route
// cannot dodge. Where both paths fire, the snapshot of t_regLogged keeps it to
// a single line.
static t_NtCreateKey pModNtCKey_ = NULL;
static t_NtSetValueKey pModNtSVK_ = NULL;
typedef LONG (WINAPI* t_RegCreateKeyExW)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM,
                                         LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
typedef LONG (WINAPI* t_RegSetValueExW)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);
static t_RegCreateKeyExW pRegCreateKeyExW_ = NULL;
static t_RegSetValueExW pRegSetValueExW_ = NULL;

static LONG WINAPI HkIatModNtCreateKey(HANDLE* h, ULONG acc, void* oa_, ULONG idx,
                                       GC_UNI* cls, ULONG opts, ULONG* disp)
{
    LONG st = pModNtCKey_(h, acc, oa_, idx, cls, opts, disp);
    RegLogKeyFromOa(st, oa_, disp ? *disp : 0);
    return st;
}

static LONG WINAPI HkIatModNtSetValueKey(HANDLE key, GC_UNI* vname, ULONG idx,
                                         ULONG type, void* data, ULONG size)
{
    LONG st = pModNtSVK_(key, vname, idx, type, data, size);
    RegLogValueFromUni(key, st, vname, type, data, size);
    return st;
}

static LONG WINAPI HkIatRegCreateKeyExW(HKEY root, LPCWSTR sub, DWORD reserved,
                                        LPWSTR cls, DWORD opts, REGSAM sam,
                                        LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disp)
{
    int before = t_regLogged;
    LONG st = pRegCreateKeyExW_(root, sub, reserved, cls, opts, sam, sa, out, disp);
    if (t_regLogged == before)
    {
        // resolve the resulting handle, so the line carries the full path even
        // though the caller only gave a subkey name
        WCHAR path[520];
        path[0] = 0;
        if (st == 0 && out && *out) RegPathOfKey(*out, path, 520);
        if (!path[0] && sub)
        {
            __try
            {
                int c = (int)wcslen(sub);
                if (c > 255) c = 255;
                memcpy(path, sub, (size_t)c * sizeof(WCHAR));
                path[c] = 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { path[0] = 0; }
        }
        RegLogKeyEvent(st, path, disp ? *disp : 0);
    }
    return st;
}

static LONG WINAPI HkIatRegSetValueExW(HKEY key, LPCWSTR name, DWORD reserved,
                                       DWORD type, const BYTE* data, DWORD size)
{
    int before = t_regLogged;
    LONG st = pRegSetValueExW_(key, name, reserved, type, data, size);
    if (t_regLogged == before) RegLogValueEvent(key, st, name, type, data, size);
    return st;
}

// NtSetSystemInformation: rare, and exactly the kind of call a protection uses
// for system-wide tweaks (kernel debugger / code integrity / driver loading
// style classes). Logged with class, length and the first dword of the payload.
typedef LONG (WINAPI* t_NtSetSystemInformation)(ULONG, void*, ULONG);
static t_NtSetSystemInformation pIatNtSsi_ = NULL;

static LONG WINAPI HkIatKbNtSsi(ULONG cls, void* info, ULONG len)
{
    LONG st = pIatNtSsi_(cls, info, len);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[800];
        unsigned long long v1 = 0;
        __try { if (info && len >= 4) v1 = *(ULONG*)info; }
        __except (EXCEPTION_EXECUTE_HANDLER) { v1 = 0; }
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 800, L"syst   st=0x%08X class=0x%lX len=%lu first=0x%llX  <- %s",
                             (unsigned)st, (unsigned long)cls, (unsigned long)len, v1, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

// A bounded, SEH-guarded hex preview of a caller-owned buffer: "what was
// actually written" for a cross-process write, "what came back" for an NtQuery.
// The pointer is never trusted and never read past max bytes, so a bad pointer
// costs one fault (caught here) rather than the game.
static int BytesPreview(const void* p, SIZE_T n, WCHAR* out, int cch, int max)
{
    static const WCHAR hex[] = L"0123456789ABCDEF";
    int k, o = 0;
    if (cch < 8) { if (cch > 0) out[0] = 0; return 0; }
    if (!p || !n) { out[0] = 0; return 0; }
    if (n > (SIZE_T)max) n = (SIZE_T)max;
    __try
    {
        out[o++] = L' ';
        out[o++] = L'[';
        for (k = 0; k < (int)n && o + 4 < cch; k++)
        {
            BYTE b = ((const BYTE*)p)[k];
            if (k) out[o++] = L' ';
            out[o++] = hex[b >> 4];
            out[o++] = hex[b & 0xF];
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        StringCchCopyW(out, (size_t)cch, L" [unreadable]");
        return 1;
    }
    if (o + 2 < cch) { out[o++] = L']'; out[o] = 0; }
    else out[cch - 1] = 0;
    return 1;
}

static LONG WINAPI HkIatKbNtQip(HANDLE proc, ULONG cls, void* info, ULONG len, ULONG* retLen)
{
    LONG st = pIatNtQIP_(proc, cls, info, len, retLen);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[900], back[200];
        back[0] = 0;
        if (st == 0) BytesPreview(info, len, back, 200, 16);   // what the OS answered
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 900, L"qip    st=0x%08X class=%lu len=%lu%s  <- %s",
                             (unsigned)st, cls, len, back, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static LONG WINAPI HkIatKbNtSit(HANDLE th, ULONG cls, void* info, ULONG len)
{
    LONG st = pIatNtSIT_(th, cls, info, len);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[700];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 700, L"sit    st=0x%08X class=0x%lX t=0x%llX  <- %s",
                             (unsigned)st, cls, (unsigned long long)(ULONG_PTR)th, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static LONG WINAPI HkIatKbNtCtex(void** th, ULONG acc, void* oa, HANDLE proc, void* start, void* arg, ULONG cflags,
                                 ULONG_PTR zero, SIZE_T stack, SIZE_T maxstack, void* attr)
{
    LONG st = pIatNtCTEX_(th, acc, oa, proc, start, arg, cflags, zero, stack, maxstack, attr);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[800];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 800, L"cthex  st=0x%08X start=0x%llX flags=0x%lX  <- %s",
                             (unsigned)st, (unsigned long long)(ULONG_PTR)start, cflags, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static LONG WINAPI HkIatKbNtAvm(HANDLE proc, void** base, ULONG_PTR* zbits, SIZE_T* size, ULONG type, ULONG prot)
{
    LONG st = pIatNtAVM_(proc, base, zbits, size, type, prot);
    if (!inHook && g_hooksActive && g_count < g_cap && base && size)
    {
        WCHAR caller[300], msg[800];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 800, L"avm    st=0x%08X @0x%llX size=0x%llX type=0x%X prot=0x%X  <- %s",
                             (unsigned)st, (unsigned long long)(base ? (ULONG_PTR)*base : 0),
                             (unsigned long long)(size ? *size : 0), type, prot, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

// ---- wider interposition set: context / thread / process / virtual memory ---
// These catch the anti-debug and mutual-probing surface even when a caller goes
// through the syscall API instead of kernel32, and (rvm/wvm) answer outright
// who is reading or writing whose memory.
typedef LONG (WINAPI* t_NtSCT)(HANDLE, const void*);
typedef LONG (WINAPI* t_NtGCT)(HANDLE, void*);
typedef LONG (WINAPI* t_NtQIT)(HANDLE, ULONG, void*, ULONG, ULONG*);
typedef LONG (WINAPI* t_NtSIP)(HANDLE, ULONG, void*, ULONG);
typedef LONG (WINAPI* t_NtQVM)(HANDLE, const void*, ULONG, void*, SIZE_T, SIZE_T*);
typedef LONG (WINAPI* t_NtRVM)(HANDLE, const void*, void*, SIZE_T, SIZE_T*);
typedef LONG (WINAPI* t_NtWVM)(HANDLE, void*, const void*, SIZE_T, SIZE_T*);
typedef LONG (WINAPI* t_NtOT)(void**, ULONG, void*, void*);
static t_NtSCT pIatNtSCT_ = NULL;
static t_NtGCT pIatNtGCT_ = NULL;
static t_NtQIT pIatNtQIT_ = NULL;
static t_NtSIP pIatNtSIP_ = NULL;
static t_NtQVM pIatNtQVM_ = NULL;
static t_NtRVM pIatNtRVM_ = NULL;
static t_NtWVM pIatNtWVM_ = NULL;
static t_NtOT  pIatNtOT_  = NULL;

// x64 CONTEXT: ContextFlags 0x30, Dr0..Dr7 0x48..0x70 (same layout the
// SetThreadContext hook reads).
static void CtxDrString(const void* ctx, WCHAR* out, int cch)
{
    const BYTE* c = (const BYTE*)ctx;
    out[0] = 0;
    if (!c) return;
    __try
    {
        StringCchPrintfW(out, (size_t)cch, L" ctxf=0x%X Dr0=%llX Dr1=%llX Dr2=%llX Dr3=%llX Dr6=%llX Dr7=%llX",
                         (unsigned)*(const DWORD*)(c + 0x30),
                         (unsigned long long)*(const DWORD64*)(c + 0x48),
                         (unsigned long long)*(const DWORD64*)(c + 0x50),
                         (unsigned long long)*(const DWORD64*)(c + 0x58),
                         (unsigned long long)*(const DWORD64*)(c + 0x60),
                         (unsigned long long)*(const DWORD64*)(c + 0x68),
                         (unsigned long long)*(const DWORD64*)(c + 0x70));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { StringCchPrintfW(out, (size_t)cch, L" <context unreadable>"); }
}

// NtSetContextThread: Dr arming/scrubbing via the syscall path (the kernel32
// SetThreadContext hook only sees the Win32 route).
static LONG WINAPI HkIatKbNtSct(HANDLE th, const void* ctx)
{
    LONG st = pIatNtSCT_(th, ctx);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1000], dr[220];
        inHook = 1;
        CtxDrString(ctx, dr, 220);
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 1000, L"ntsetctx st=0x%08X t=0x%llX%s  <- %s",
                             (unsigned)st, (unsigned long long)(ULONG_PTR)th, dr, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

// NtGetContextThread: who READS the debug registers back (tamper check).
static LONG WINAPI HkIatKbNtGct(HANDLE th, void* ctx)
{
    LONG st = pIatNtGCT_(th, ctx);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1000], dr[220];
        inHook = 1;
        CtxDrString(ctx, dr, 220);
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 1000, L"ntgetctx st=0x%08X t=0x%llX%s  <- %s",
                             (unsigned)st, (unsigned long long)(ULONG_PTR)th, dr, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

// One report line for NtQueryInformationThread, shared by the KERNELBASE layer
// and the module-level hook below (artifact.dll imports it straight from ntdll,
// so the layer - which watches KERNELBASE's own imports - never sees it). The
// tag says which of the two saw the call, because both can and the answer is
// not cosmetic: a `qit` line whose caller is the game exe came through
// KERNELBASE, while `qit(iat)` can only mean the slot inside the monitored
// module - so an installed-but-never-fired module hook is visible as such.
static void LogQit(LONG st, ULONG cls, HANDLE th, const void* info, ULONG len, int viaModule)
{
    if (inHook || !g_hooksActive || g_count >= g_cap) return;
    {
        WCHAR caller[300], msg[900], back[200];
        back[0] = 0;
        if (st == 0) BytesPreview(info, len, back, 200, 16);   // what the OS answered
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 900, L"%s st=0x%08X class=%lu t=0x%llX%s  <- %s",
                             viaModule ? L"qit(iat)" : L"qit    ",
                             (unsigned)st, cls, (unsigned long long)(ULONG_PTR)th, back, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
}
static LONG WINAPI HkIatKbNtQit(HANDLE th, ULONG cls, void* info, ULONG len, ULONG* ret)
{
    LONG st = pIatNtQIT_(th, cls, info, len, ret);
    LogQit(st, cls, th, info, len, 0);
    return st;
}

// ---- ntdll probes imported directly by a monitored module ----------------
// Separate trampolines from the KERNELBASE layer: a module's import can be
// bound to a shadow ntdll copy, so the two originals are not interchangeable.
static t_NtQIT pModNtQIT_ = NULL;

static LONG WINAPI HkIatModNtQit(HANDLE th, ULONG cls, void* info, ULONG len, ULONG* ret)
{
    LONG st = pModNtQIT_(th, cls, info, len, ret);
    LogQit(st, cls, th, info, len, 1);
    return st;
}

// NtQueryVolumeInformationFile: the volume serial number, label and file-system
// name are hardware/system fingerprints - what a licence check binds a machine
// to, which is why a protection asks for them. Classes logged:
// 1 FileFsVolumeInformation, 4 FileFsDeviceInformation, 7 FileFsAttributeInformation.
typedef LONG (WINAPI* t_NtQvif)(HANDLE, void*, void*, ULONG, ULONG);
static t_NtQvif pModNtQvif_ = NULL;

static LONG WINAPI HkIatModNtQvif(HANDLE h, void* iosb, void* info, ULONG len, ULONG cls)
{
    LONG st = pModNtQvif_(h, iosb, info, len, cls);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[700], detail[160];
        detail[0] = 0;
        if (st == 0 && info)
        {
            __try
            {
                if (cls == 1 && len >= 16)
                {
                    // FS_VOLUME_INFO: {LARGE_INTEGER CreationTime; ULONG Serial; ...}
                    StringCchPrintfW(detail, 160, L" serial=0x%08X",
                                     *(unsigned*)((BYTE*)info + 8));
                }
                else if (cls == 4 && len >= 8)
                {
                    // FS_DEVICE_INFO: {ULONG DeviceType; ULONG Characteristics;}
                    StringCchPrintfW(detail, 160, L" dev=%lu char=0x%lX",
                                     *(unsigned*)info, *(unsigned*)((BYTE*)info + 4));
                }
                else if (cls == 7 && len >= 16)
                {
                    // FS_ATTRIBUTE_INFO: {ULONG Attr; LONG MaxNameLen; ULONG NameLen; WCHAR Name[];}
                    ULONG nl = *(ULONG*)((BYTE*)info + 8);
                    if (nl >= 2 && nl <= 64 && 12 + nl <= len)
                    {
                        int k, n = (int)(nl / sizeof(WCHAR));
                        WCHAR fs[40];
                        for (k = 0; k < n && k < 39; k++)
                        {
                            WCHAR ch = *(WCHAR*)((BYTE*)info + 12 + k * 2);
                            fs[k] = (ch >= 0x20 && ch < 0x7F) ? ch : L'.';
                        }
                        fs[k] = 0;
                        StringCchPrintfW(detail, 160, L" fs=\"%s\"", fs);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { detail[0] = 0; }
        }
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 700, L"qvif   st=0x%08X class=%lu%s  <- %s",
                             (unsigned)st, cls, detail, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

// NtSetInformationProcess: class 0x1E ProcessDebugPort, 0x1F ProcessDebugFlags,
// 0x1D ProcessExceptionPort - the "hide me from the debugger" writes.
static LONG WINAPI HkIatKbNtSip(HANDLE proc, ULONG cls, void* info, ULONG len)
{
    LONG st = pIatNtSIP_(proc, cls, info, len);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[700];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 700, L"sip    st=0x%08X class=0x%lX val=0x%llX  <- %s",
                             (unsigned)st, cls, (unsigned long long)(info ? *(ULONG_PTR*)info : 0), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static LONG WINAPI HkIatKbNtQvm(HANDLE proc, const void* addr, ULONG cls, void* info, SIZE_T len, SIZE_T* ret)
{
    LONG st = pIatNtQVM_(proc, addr, cls, info, len, ret);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[900], back[200];
        back[0] = 0;
        if (st == 0 && cls == 0 && info && len >= sizeof(MEMORY_BASIC_INFORMATION))
        {
            // MemoryBasicInformation: the answer a protection actually uses -
            // is this page committed? executable? private (i.e. packed) or image?
            __try
            {
                MEMORY_BASIC_INFORMATION* m = (MEMORY_BASIC_INFORMATION*)info;
                StringCchPrintfW(back, 200, L" base=0x%llX size=0x%llX state=0x%lX prot=0x%lX type=0x%lX",
                                 (unsigned long long)(ULONG_PTR)m->BaseAddress,
                                 (unsigned long long)m->RegionSize,
                                 (unsigned long)m->State, (unsigned long)m->Protect,
                                 (unsigned long)m->Type);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { back[0] = 0; }
        }
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 900, L"qvmm   st=0x%08X class=%lu @0x%llX len=0x%llX%s  <- %s",
                             (unsigned)st, cls, (unsigned long long)(ULONG_PTR)addr,
                             (unsigned long long)len, back, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

// NtReadVirtualMemory / NtWriteVirtualMemory: cross-process memory access -
// "who is reading/writing whose memory" (self reads show proc = -1), plus a
// bounded preview of the bytes: what was read out, or what was written in.
static LONG WINAPI HkIatKbNtRvm(HANDLE proc, const void* addr, void* buf, SIZE_T len, SIZE_T* got)
{
    LONG st = pIatNtRVM_(proc, addr, buf, len, got);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[900], data[220];
        data[0] = 0;
        if (st == 0) BytesPreview(buf, (got && *got) ? *got : len, data, 220, 16);
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 900, L"rvm    st=0x%08X proc=0x%llX @0x%llX len=0x%llX%s  <- %s",
                             (unsigned)st, (unsigned long long)(ULONG_PTR)proc,
                             (unsigned long long)(ULONG_PTR)addr, (unsigned long long)len,
                             data, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static LONG WINAPI HkIatKbNtWvm(HANDLE proc, void* addr, const void* buf, SIZE_T len, SIZE_T* got)
{
    LONG st = pIatNtWVM_(proc, addr, buf, len, got);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[900], data[220];
        data[0] = 0;
        if (buf && len) BytesPreview(buf, len, data, 220, 16);   // the bytes being written
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 900, L"wvm    st=0x%08X proc=0x%llX @0x%llX len=0x%llX%s  <- %s",
                             (unsigned)st, (unsigned long long)(ULONG_PTR)proc,
                             (unsigned long long)(ULONG_PTR)addr, (unsigned long long)len,
                             data, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static LONG WINAPI HkIatKbNtOt(void** th, ULONG acc, void* oa, void* cid)
{
    LONG st = pIatNtOT_(th, acc, oa, cid);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[800];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 800, L"openthr st=0x%08X acc=0x%lX tid=%lu  <- %s",
                             (unsigned)st, acc, (unsigned)(cid ? *(ULONG*)((BYTE*)cid + 8) : 0), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static LONG WINAPI HkIatKbNtRead(HANDLE h, HANDLE ev, void* apc, void* apcCtx, void* iosb, void* buf, ULONG len, void* off, ULONG* key)
{
    LONG st = pIatNtRead_(h, ev, apc, apcCtx, iosb, buf, len, off, key);
    if (!inHook && g_reads && g_hooksActive && g_count < g_cap && InterlockedIncrement(&g_rwKb) <= g_cap / 8)
    {
        WCHAR caller[300], msg[1500], path[400];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            unsigned long long got;
            HandleName(h, path, 400);
            got = (st >= 0 && iosb) ? (unsigned long long)((IOSB*)iosb)->Information : 0;
            StringCchPrintfW(msg, 1500, L"read   st=0x%08X got=%llu \"%s\"  <- %s", (unsigned)st, got, path, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static LONG WINAPI HkIatKbNtWrite(HANDLE h, HANDLE ev, void* apc, void* apcCtx, void* iosb, void* buf, ULONG len, void* off, ULONG* key)
{
    LONG st = pIatNtWrite_(h, ev, apc, apcCtx, iosb, buf, len, off, key);
    if (!inHook && g_writes && g_hooksActive && g_count < g_cap && InterlockedIncrement(&g_rwKb) <= g_cap / 8)
    {
        WCHAR caller[300], msg[1500], path[400];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            unsigned long long got;
            HandleName(h, path, 400);
            got = (st >= 0 && iosb) ? (unsigned long long)((IOSB*)iosb)->Information : 0;
            StringCchPrintfW(msg, 1500, L"write  st=0x%08X got=%llu \"%s\"  <- %s", (unsigned)st, got, path, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

static LONG WINAPI HkIatNtTerm(HANDLE proc, LONG code)
{
    LONG st = pIatNtTerm_(proc, code);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1200];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 1200, L"term   st=0x%08X proc=0x%llX code=%d  <- %s",
                             (unsigned)st, (unsigned long long)(ULONG_PTR)proc, code, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return st;
}

typedef void* (WINAPI* t_CreateServiceW13)(void*, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD, DWORD,
                                           LPCWSTR, LPCWSTR, void*, LPCWSTR, LPCWSTR, LPCWSTR);
static void* WINAPI HkIatCreateServiceW(void* scm, LPCWSTR name, LPCWSTR display, DWORD access, DWORD type,
                                        DWORD start, DWORD errctl, LPCWSTR binary, LPCWSTR group, void* tagId,
                                        LPCWSTR deps, LPCWSTR startName, LPCWSTR password)
{
    void* r = ((t_CreateServiceW13)pCreateSvc_)(scm, name, display, access, type, start, errctl,
                                                binary, group, tagId, deps, startName, password);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1500];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 1500, L"svc    create \"%s\" (\"%s\") binary=\"%s\" st=0x%08X  <- %s",
                             name ? name : L"?", display ? display : L"", binary ? binary : L"?",
                             (unsigned)(r ? 0 : GetLastError()), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

static BOOL WINAPI HkIatStartServiceW(void* svc, DWORD argc, void* argv)
{
    BOOL ok = pStartSvc_(svc, argc, argv);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1200];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 1200, L"svc    start st=0x%08X  <- %s", (unsigned)(ok ? 0 : GetLastError()), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return ok;
}

static LPVOID WINAPI HkIatVirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot)
{
    LPVOID r = pVAlloc_(addr, size, type, prot);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[900];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 900, L"valloc @0x%llX size=0x%llX prot=0x%X st=0x%08X  <- %s",
                             (unsigned long long)(ULONG_PTR)r, (unsigned long long)size, (unsigned)prot,
                             (unsigned)(r ? 0 : GetLastError()), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

static BOOL WINAPI HkIatVirtualProtect(LPVOID addr, SIZE_T size, DWORD newp, PDWORD oldp)
{
    BOOL ok = pVProt_(addr, size, newp, oldp);
    if (!inHook && g_hooksActive && g_count < g_cap && ok && oldp && newp != *oldp)
    {
        WCHAR caller[300], msg[700];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 700, L"vprot  @0x%llX+0x%llX old=0x%X new=0x%X  <- %s",
                             (unsigned long long)(ULONG_PTR)addr, (unsigned long long)size, (unsigned)*oldp, (unsigned)newp, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return ok;
}

static BOOL WINAPI HkIatVirtualFree(LPVOID addr, SIZE_T size, DWORD type)
{
    BOOL ok = pVFree_(addr, size, type);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[700];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 700, L"vfree  @0x%llX size=0x%llX st=0x%08X  <- %s",
                             (unsigned long long)(ULONG_PTR)addr, (unsigned long long)size, (unsigned)(ok ? 0 : GetLastError()), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return ok;
}

static BOOL WINAPI HkIatCreateProcessW(LPCWSTR app, LPWSTR cmd, void* pa, void* ta, BOOL inherit,
                                       DWORD flags, void* env, LPCWSTR dir, void* si, void* pi)
{
    BOOL ok = pCProc_(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1500];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 1500, L"procspawn st=0x%08X \"%s\" \"%s\"  <- %s",
                             (unsigned)(ok ? 0 : GetLastError()), app ? app : L"?", cmd ? cmd : L"?", caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return ok;
}

static void* WINAPI HkIatShellExecuteW(void* hwnd, LPCWSTR verb, LPCWSTR file, LPCWSTR params, LPCWSTR dir, int show)
{
    void* r = pShExec_(hwnd, verb, file, params, dir, show);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1500];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 1500, L"shexec \"%s %s %s\"  <- %s",
                             verb ? verb : L"open", file ? file : L"?", params ? params : L"", caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

static BOOL WINAPI HkIatSetThreadContext(HANDLE t, const void* ctx)
{
    BOOL ok = pSetTC_(t, ctx);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[900], dr[220];
        const BYTE* c = (const BYTE*)ctx;
        DWORD flags = 0;
        DWORD64 dr0 = 0, dr1 = 0, dr2 = 0, dr3 = 0, dr6 = 0, dr7 = 0;
        dr[0] = 0;
        if (c)
        {
            // x64 CONTEXT: ContextFlags at 0x30, Dr0..Dr7 at 0x48..0x70.
            // Reading the values BEFORE they are set distinguishes debug-register
            // scrubbing (all zeros) from hardware-breakpoint arming (non-zero).
            __try
            {
                flags = *(const DWORD*)(c + 0x30);
                dr0 = *(const DWORD64*)(c + 0x48);
                dr1 = *(const DWORD64*)(c + 0x50);
                dr2 = *(const DWORD64*)(c + 0x58);
                dr3 = *(const DWORD64*)(c + 0x60);
                dr6 = *(const DWORD64*)(c + 0x68);
                dr7 = *(const DWORD64*)(c + 0x70);
                StringCchPrintfW(dr, 220, L" ctxf=0x%X Dr0=%llX Dr1=%llX Dr2=%llX Dr3=%llX Dr6=%llX Dr7=%llX",
                                 (unsigned)flags, dr0, dr1, dr2, dr3, dr6, dr7);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { StringCchPrintfW(dr, 220, L" <context unreadable>"); }
        }
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 900, L"setthreadctx st=0x%08X t=0x%llX%s  <- %s",
                             (unsigned)(ok ? 0 : GetLastError()), (unsigned long long)(ULONG_PTR)t, dr, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return ok;
}

static BOOL WINAPI HkIatGetThreadContext(HANDLE t, void* ctx)
{
    BOOL ok = pGetTC_(t, ctx);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[700];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 700, L"getthreadctx st=0x%08X t=0x%llX  <- %s",
                             (unsigned)(ok ? 0 : GetLastError()), (unsigned long long)(ULONG_PTR)t, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return ok;
}

static DWORD WINAPI HkIatSuspendThread(HANDLE t)
{
    DWORD r = pSuspT_(t);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[700];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 700, L"thrsuspend t=0x%llX prev=0x%X  <- %s",
                             (unsigned long long)(ULONG_PTR)t, (unsigned)r, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

static DWORD WINAPI HkIatResumeThread(HANDLE t)
{
    DWORD r = pResT_(t);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[700];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 700, L"thrresume t=0x%llX prev=0x%X  <- %s",
                             (unsigned long long)(ULONG_PTR)t, (unsigned)r, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

static BOOL WINAPI HkIatTerminateProcess(HANDLE p, UINT code)
{
    BOOL ok = pTermP_(p, code);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[700];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 700, L"kill   proc=0x%llX code=%u st=0x%08X  <- %s",
                             (unsigned long long)(ULONG_PTR)p, (unsigned)code, (unsigned)(ok ? 0 : GetLastError()), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return ok;
}

static BOOL WINAPI HkIatCopyFileExW(LPCWSTR exist, LPCWSTR fresh, void* prog, void* data, BOOL* cancel, DWORD flags)
{
    BOOL ok = pCopyF_(exist, fresh, prog, data, cancel, flags);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1500];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 1500, L"copy   \"%s\" -> \"%s\" st=0x%08X  <- %s",
                             exist ? exist : L"?", fresh ? fresh : L"?", (unsigned)(ok ? 0 : GetLastError()), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return ok;
}

static void* WINAPI HkIatDeleteService(void* svc)
{
    void* r = pDelSvc_(svc);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[700];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 700, L"svc    delete st=0x%08X  <- %s", (unsigned)(r ? 0 : GetLastError()), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

static BOOL WINAPI HkIatCreateDirectoryW(LPCWSTR name, void* sa)
{
    BOOL ok = pCreateDir_(name, sa);
    if (!inHook && g_writes && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1200];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 1200, L"mkdir  st=0x%08X \"%s\"  <- %s",
                             (unsigned)(ok ? 0 : GetLastError()), (name && *name) ? NtPath(name) : L"?", caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return ok;
}

static void* WINAPI HkIatOpenSCManagerW(LPCWSTR machine, LPCWSTR db, DWORD access)
{
    void* r = pOpenSCM_(machine, db, access);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[900];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 900, L"svc    openmgr \"%s\" st=0x%08X  <- %s",
                             machine ? machine : L"(local)", (unsigned)(r ? 0 : GetLastError()), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

typedef void*    (WINAPI* t_VehAdd)(ULONG, PVOID);
typedef ULONG    (WINAPI* t_VehRem)(PVOID);
typedef void*    (WINAPI* t_SetUeh)(PVOID);
static t_VehAdd pVehAdd_ = NULL, pVehContAdd_ = NULL;
static t_VehRem pVehRem_ = NULL, pVehContRem_ = NULL;
static t_SetUeh pSetUeh_ = NULL;
static void DescribeModuleOf(void* addr, WCHAR* out, int cch);

static void* WINAPI HkIatVehAdd(ULONG first, PVOID handler)
{
    void* r = pVehAdd_ ? pVehAdd_(first, handler) : NULL;
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], who[130], msg[700];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            DescribeModuleOf(handler, who, 130);
            StringCchPrintfW(msg, 700, L"veh    add %s handler=%s st=0x%08X  <- %s",
                             first ? L"first" : L"last", who, (unsigned)(r ? 0 : GetLastError()), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

static ULONG WINAPI HkIatVehRemove(PVOID cookie)
{
    ULONG r = pVehRem_ ? pVehRem_(cookie) : 0;
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], who[130], msg[600];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            DescribeModuleOf(cookie, who, 130);
            StringCchPrintfW(msg, 600, L"veh    remove %s st=0x%08X  <- %s", who, (unsigned)r, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

static void* WINAPI HkIatVehContAdd(ULONG first, PVOID handler)
{
    void* r = pVehContAdd_ ? pVehContAdd_(first, handler) : NULL;
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], who[130], msg[700];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            DescribeModuleOf(handler, who, 130);
            StringCchPrintfW(msg, 700, L"veh    add continue %s handler=%s st=0x%08X  <- %s",
                             first ? L"first" : L"last", who, (unsigned)(r ? 0 : GetLastError()), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

static ULONG WINAPI HkIatVehContRemove(PVOID cookie)
{
    ULONG r = pVehContRem_ ? pVehContRem_(cookie) : 0;
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], who[130], msg[600];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            DescribeModuleOf(cookie, who, 130);
            StringCchPrintfW(msg, 600, L"veh    remove continue %s st=0x%08X  <- %s", who, (unsigned)r, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

static void* WINAPI HkIatSetUeh(PVOID filter)
{
    void* old = pSetUeh_ ? pSetUeh_(filter) : NULL;
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], who[130], msg[700];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            DescribeModuleOf(filter, who, 130);
            StringCchPrintfW(msg, 700, L"ueh    set filter=%s previous=0x%llX  <- %s",
                             who, (unsigned long long)(ULONG_PTR)old, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return old;
}

static int WINAPI HkIatMessageBoxW(void* hwnd, LPCWSTR text, LPCWSTR caption, UINT type)
{
    int r = pMsgBox_(hwnd, text, caption, type);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[1500];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 1500, L"msgbox \"%s\" / \"%s\"  <- %s",
                             caption ? caption : L"?", text ? text : L"?", caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

static BOOL WINAPI HkIatIsDebuggerPresent(void)
{
    BOOL r = pIsDbg_();
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[700];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 700, L"dbgcheck -> %s  <- %s", r ? L"YES" : L"no", caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

// Memory-walking: the monitored module querying/reading memory is how it
// scouts Denuvo's regions (or its own).
typedef SIZE_T (WINAPI* t_VQ)(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);
typedef SIZE_T (WINAPI* t_VQX)(HANDLE, LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);
typedef BOOL   (WINAPI* t_RPM)(HANDLE, LPCVOID, LPVOID, SIZE_T, SIZE_T*);
static t_VQ  pVQuery_ = NULL;
static t_VQX pVQueryEx_ = NULL;
static t_RPM pRPM_ = NULL;

static SIZE_T WINAPI HkIatVirtualQuery(LPCVOID addr, PMEMORY_BASIC_INFORMATION mbi, SIZE_T len)
{
    SIZE_T r = pVQuery_(addr, mbi, len);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[900];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 900, L"vquery @0x%llX -> base 0x%llX size 0x%llX protect 0x%X type 0x%X  <- %s",
                             (unsigned long long)(ULONG_PTR)addr,
                             (unsigned long long)(mbi && r ? (ULONG_PTR)mbi->BaseAddress : 0),
                             (unsigned long long)(mbi && r ? mbi->RegionSize : 0),
                             (unsigned)(mbi && r ? mbi->Protect : 0), (unsigned)(mbi && r ? mbi->Type : 0), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

static SIZE_T WINAPI HkIatVirtualQueryEx(HANDLE proc, LPCVOID addr, PMEMORY_BASIC_INFORMATION mbi, SIZE_T len)
{
    SIZE_T r = pVQueryEx_(proc, addr, mbi, len);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[900];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 900, L"vqueryex proc=0x%llX @0x%llX -> size 0x%llX protect 0x%X  <- %s",
                             (unsigned long long)(ULONG_PTR)proc, (unsigned long long)(ULONG_PTR)addr,
                             (unsigned long long)(mbi && r ? mbi->RegionSize : 0),
                             (unsigned)(mbi && r ? mbi->Protect : 0), caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

static BOOL WINAPI HkIatReadProcessMemory(HANDLE proc, LPCVOID addr, LPVOID buf, SIZE_T len, SIZE_T* got)
{
    BOOL r = pRPM_(proc, addr, buf, len, got);
    if (!inHook && g_hooksActive && g_count < g_cap)
    {
        WCHAR caller[300], msg[900];
        inHook = 1;
        if (DescribeCaller(caller, 300))
        {
            StringCchPrintfW(msg, 900, L"readproc st=0x%X proc=0x%llX @0x%llX len=0x%llX  <- %s",
                             r ? 0 : (unsigned)GetLastError(), (unsigned long long)(ULONG_PTR)proc,
                             (unsigned long long)(ULONG_PTR)addr, (unsigned long long)len, caller);
            EmitHook(msg);
        }
        inHook = 0;
    }
    return r;
}

static int HookIatName(HMODULE mod, const char* apiName, void* hook, void** orig)
{
    BYTE* base;
    IMAGE_DOS_HEADER* dos;
    IMAGE_NT_HEADERS* nt;
    DWORD rva;
    IMAGE_IMPORT_DESCRIPTOR* imp;
    if (!mod) return 0;
    base = (BYTE*)mod;
    dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return 0;
    imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + rva);
    for (; imp->Name; imp++)
    {
        IMAGE_THUNK_DATA* int_ = imp->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk)
            : (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; int_->u1.Function; int_++, iat++)
        {
            const char* name;
            DWORD oldp = 0, oldp2 = 0;
            if (int_->u1.Ordinal & 0x8000000000000000ull) continue;   // import by ordinal
            name = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + int_->u1.AddressOfData))->Name;
            if (_stricmp(name, apiName) != 0) continue;
            if (!VirtualProtect(iat, 8, PAGE_READWRITE, &oldp)) return 0;
            if (!*orig) *orig = (void*)iat->u1.Function;
            iat->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(iat, 8, oldp, &oldp2);
            return 1;
        }
    }
    return 0;
}

static int HookIatByValue(HMODULE mod, void* expected, void* hook, void** orig)
{
    BYTE* base;
    IMAGE_DOS_HEADER* dos;
    IMAGE_NT_HEADERS* nt;
    DWORD rva;
    IMAGE_IMPORT_DESCRIPTOR* imp;
    if (!mod || !expected || *orig) return 0;
    base = (BYTE*)mod;
    dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return 0;
    imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + rva);
    for (; imp->Name; imp++)
    {
        IMAGE_THUNK_DATA* int_ = imp->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk)
            : (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; int_->u1.Function; int_++, iat++)
        {
            DWORD oldp = 0, oldp2 = 0;
            if ((void*)iat->u1.Function != expected) continue;
            if (!VirtualProtect(iat, 8, PAGE_READWRITE, &oldp)) return 0;
            if (!*orig) *orig = expected;
            iat->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(iat, 8, oldp, &oldp2);
            return 1;
        }
    }
    return 0;
}

static int HookIatApi(HMODULE mod, const char* name, void* hook, void** orig, const WCHAR* modName)
{
    if (HookIatName(mod, name, hook, orig))
    {
        WCHAR wname[64], msg[300];
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64);
        StringCchPrintfW(msg, 300, L"(iat) %s hooked in %s", wname, modName);
        HookNote(msg);
        return 1;
    }
    return 0;
}

static HMODULE FindWorkingNtdll(void)
{
    HMODULE nts[8]; int nnt = 0, i;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, g_pid);
    if (snap != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W me; me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me))
        {
            do
            {
                if (_wcsicmp(me.szModule, L"ntdll.dll") == 0 && nnt < 8)
                {
                    int dup = 0;
                    for (i = 0; i < nnt; i++) if (nts[i] == me.hModule) dup = 1;
                    if (!dup) nts[nnt++] = me.hModule;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    for (i = 0; i < nnt; i++)
        if (GetProcAddress(nts[i], "NtReadFile")) return nts[i];
    return GetModuleHandleW(L"ntdll.dll");
}

// Read-only census of WHERE a module's imports are actually bound, taken
// BEFORE any of them are repointed. Answers the key question: does this
// module's Nt* ordinal imports go through the real ntdll or through the
// private/proxy copy? Two modes:
//   detailAll=1 (HookModules): every ntdll-bound import gets a detail line
//   (named with the copy's base so it can be matched against the [SCAN]
//   proxy line); everything else is tallied per target module.
//   detailAll=0 (all-module census): only imports bound to memory that is
//   NOT a loader-loaded image get detail - i.e. Denuvo-style manual-mapped
//   copies or unlinked code. Loader-image targets are tallied only.
// Never writes anything.
typedef struct { WCHAR name[64]; int n; } BindTally;
typedef struct { BYTE* base; DWORD size; } ScanImg;   // full decl in scan section
static void DescribeModuleOf(void* addr, WCHAR* out, int cch);
static int  ScanInRange(void* a, ScanImg* im);
static void CsvRow(const WCHAR* cat, const WCHAR* a, const WCHAR* b, const WCHAR* c);
static void DecodeExecPrologue(ULONG_PTR a);
static int  g_censusShadow;              // global NON-IMAGE detail counter (session cap)
static int  g_censusShadowSkipped;       // NON-IMAGE bindings past the cap, counted not printed

// Toolhelp knows about the Denuvo-style manually-mapped shadow DLLs (they
// appear as extra modules with the real system names), while GetModuleHandleEx
// does not - it only resolves OS-loader-mapped images. Censusing through this
// table is what lets a NON-IMAGE binding be named: "KERNEL32#1 @0x7FF4FDA80000"
// instead of a bare address. #k is 1-based among same-named modules, ordered
// by ascending base, matching the ntdll#1/#2 convention (shadow copy = #1).
typedef struct { ULONG_PTR base; DWORD size; WCHAR name[64]; int idx; } ModRange;
static ModRange g_modRange[320];
static int g_nModRange = 0;

static void RefreshModuleRanges(void)
{
    HANDLE snap; MODULEENTRY32W me; int i, j;
    g_nModRange = 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, g_pid ? g_pid : GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me))
    {
        do
        {
            int dup = 0;
            for (i = 0; i < g_nModRange; i++)
                if (g_modRange[i].base == (ULONG_PTR)me.hModule) { dup = 1; break; }
            if (dup || g_nModRange >= 320) continue;
            g_modRange[g_nModRange].base = (ULONG_PTR)me.hModule;
            g_modRange[g_nModRange].size = me.modBaseSize;
            StringCchPrintfW(g_modRange[g_nModRange].name, 64, L"%s", me.szModule);
            g_nModRange++;
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    for (i = 0; i < g_nModRange; i++)
    {
        int k = 1;
        for (j = 0; j < g_nModRange; j++)
            if (j != i && _wcsicmp(g_modRange[j].name, g_modRange[i].name) == 0 &&
                g_modRange[j].base < g_modRange[i].base) k++;
        g_modRange[i].idx = k;
    }
}

// Names the module containing addr using the toolhelp table (sees shadow
// copies). Returns 1 and "NAME#k @0xBASE" (or "NAME" when unique) when found,
// else falls back to DescribeModuleOf (0x… for unlinked code).
static int CensusDescribe(void* addr, WCHAR* out, int cch)
{
    ULONG_PTR a = (ULONG_PTR)addr;
    int i, same, k;
    for (i = 0; i < g_nModRange; i++)
        if (a >= g_modRange[i].base && a < g_modRange[i].base + g_modRange[i].size)
        {
            same = 0;
            for (k = 0; k < g_nModRange; k++)
                if (_wcsicmp(g_modRange[k].name, g_modRange[i].name) == 0) same++;
            if (same > 1)
                StringCchPrintfW(out, (size_t)cch, L"%s#%d @0x%llX+0x%llX", g_modRange[i].name,
                                 g_modRange[i].idx, (unsigned long long)g_modRange[i].base,
                                 (unsigned long long)(a - g_modRange[i].base));
            else
                StringCchPrintfW(out, (size_t)cch, L"%s+0x%llX", g_modRange[i].name,
                                 (unsigned long long)(a - g_modRange[i].base));
            return 1;
        }
    DescribeModuleOf(addr, out, cch);
    return 0;
}

static void CensusBindings(BYTE* base, const WCHAR* modName, int detailAll)
{
    IMAGE_DOS_HEADER* dos;
    IMAGE_NT_HEADERS* nth;
    IMAGE_IMPORT_DESCRIPTOR* imp;
    BindTally tally[32];
    int nTally = 0, unbound = 0, total = 0, nNtDetail = 0, nDesc = 0, nWarn = 0, over = 0;
    int skip0 = g_censusShadowSkipped;
    ScanImg nts[8];
    int nnt = 0, i;
    HANDLE snap;
    WCHAR line[1200];

    if (!base) return;
    RefreshModuleRanges();

    // ntdll copies for labeling bound Nt* imports (same order the [SCAN]
    // survey numbers them, and the proxy line prints the base too).
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, g_pid);
    if (snap != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W me; me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me))
        {
            do
            {
                if (_wcsicmp(me.szModule, L"ntdll.dll") == 0 && nnt < 8)
                {
                    int dup = 0;
                    for (i = 0; i < nnt; i++) if (nts[i].base == (BYTE*)me.hModule) dup = 1;
                    if (!dup) { nts[nnt].base = (BYTE*)me.hModule; nts[nnt].size = me.modBaseSize; nnt++; }
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }

    __try
    {
        dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        nth = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nth->Signature != IMAGE_NT_SIGNATURE) return;
        {
            DWORD rva = nth->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
            if (!rva) return;
            imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + rva);
        }

        for (i = 0; i < 64 && imp->Name; i++, imp++)
        {
            ULONGLONG* oft = imp->OriginalFirstThunk ? (ULONGLONG*)(base + imp->OriginalFirstThunk) : NULL;
            ULONGLONG* ft  = (ULONGLONG*)(base + imp->FirstThunk);
            int j;
            if (!ft) continue;
            nDesc++;
            for (j = 0; j < 4096; j++)
            {
                ULONGLONG thk = ft[j];
                void* addr;
                WCHAR who[130], mName[96];
                const char* nm = NULL;
                unsigned ord = 0;
                char nameBuf[128];
                int ti, isNtdll = 0, k, isShadow = 0;

                if (!thk) break;
                total++;
                if (oft)
                {
                    ULONGLONG o = oft[j];
                    if (o & 0x8000000000000000ULL)
                        ord = (unsigned)(o & 0xFFFF);
                    else
                        nm = (const char*)((BYTE*)((IMAGE_IMPORT_BY_NAME*)(base + (DWORD)o))->Name);
                }
                if (!nm) { StringCchPrintfA(nameBuf, 128, "ord%u", ord); nm = nameBuf; }

                addr = (void*)(ULONG_PTR)thk;
                if (!addr) { unbound++; continue; }

                CensusDescribe(addr, who, 130);
                {
                    const WCHAR* plus = wcschr(who, L'+');
                    int cl = plus ? (int)(plus - who) : (int)wcslen(who);
                    StringCchPrintfW(mName, 96, L"%.*s", cl, who);
                }
                for (k = 0; k < nnt; k++)
                    if (ScanInRange(addr, &nts[k])) { isNtdll = 1; break; }

                // shadow/manual-mapped target: the address lives in committed
                // memory the OS loader did not map as an image - i.e. one of
                // the hand-mapped DLL copies or unlinked code
                {
                    MEMORY_BASIC_INFORMATION mbi2;
                    if (VirtualQuery(addr, &mbi2, sizeof(mbi2)) && mbi2.Type != MEM_IMAGE)
                        isShadow = 1;
                }

                if (isShadow && detailAll == 0)
                {
                    // all-module mode: non-image bindings are THE finding -
                    // detail them until the session cap, then just count
                    if (g_censusShadow < CensusAllMax)
                    {
                        WCHAR wnm[130];
                        g_censusShadow++;
                        StringCchPrintfW(line, 1200, L"binding census: %s!%hs -> %s (bound to NON-IMAGE memory: manual-mapped copy or unlinked code)",
                                         modName, nm, who);
                        Write(L"[SCAN]", line);
                        MultiByteToWideChar(CP_ACP, 0, nm, -1, wnm, 130);
                        CsvRow(L"binding-nonimage", modName, wnm, who);
                    }
                    else g_censusShadowSkipped++;
                }
                else if (isNtdll && detailAll && nNtDetail < 32)
                {
                    // the interesting case: which ntdll copy, by base
                    nNtDetail++;
                    StringCchPrintfW(line, 1200, L"binding census: %s!%hs -> ntdll#%d @0x%llX (bound %s)",
                        modName, nm, k + 1,
                        (unsigned long long)(ULONG_PTR)nts[k].base, who);
                    Write(L"[SCAN]", line);
                }

                for (ti = 0; ti < nTally; ti++)
                    if (_wcsicmp(tally[ti].name, mName) == 0) break;
                if (ti == nTally)
                {
                    if (nTally < 32)
                    { StringCchPrintfW(tally[nTally].name, 64, L"%s", mName); tally[nTally].n = 0; nTally++; }
                    else { over++; continue; }   // tally table full: count in (other)
                }
                if (ti < 32) tally[ti].n++;
            }
            if (j == 4096 && nWarn < 4)
            {
                // walked the cap without a terminator: this descriptor's IAT
                // array is not null-terminated (hand-crafted import table) -
                // the walk spilled into whatever follows, so counts near the
                // end are neighbor data, not real imports
                nWarn++;
                StringCchPrintfW(line, 1200, L"binding census: %s import descriptor #%d has NO null terminator in its IAT array (4096 cap hit) - hand-crafted/corrupt import table; trailing counts include neighbor memory",
                                 modName, i);
                Write(L"[SCAN]", line);
            }
        }
        if (i == 64 && imp->Name)
        {
            // more descriptors than the walk covers: never truncate silently
            StringCchPrintfW(line, 1200, L"binding census: %s has MORE than 64 import descriptors - the walk stopped at the cap; the rest are NOT counted",
                             modName);
            Write(L"[SCAN]", line);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Trace(L"binding census fault");
        return;
    }

    {
        int ti, off = 0, sum = 0, skipN = g_censusShadowSkipped - skip0;
        WCHAR mods[900], capNote[96];
        mods[0] = 0;
        capNote[0] = 0;
        if (skipN > 0)
            StringCchPrintfW(capNote, 96, L" - NON-IMAGE detail capped at %d (%d more suppressed)",
                             CensusAllMax, skipN);
        for (ti = 0; ti < nTally; ti++) sum += tally[ti].n;
        for (ti = 0; ti < nTally && off < 800; ti++)
        {
            int w = _snwprintf(mods + off, 900 - off, L"%s%s x%d",
                               ti ? L", " : L"", tally[ti].name, tally[ti].n);
            if (w < 0) break;
            off += w;
        }
        if (over > 0 && off < 860)
            off += _snwprintf(mods + off, 900 - off, L", (other) x%d", over);
        sum += over;
        StringCchPrintfW(line, 1200, L"binding census for %s: %d import%s (%d unbound) in %d descriptor(s) across %d target%s (tally sums to %d)%s: %s",
                         modName, total, total == 1 ? L"" : L"s", unbound, nDesc,
                         nTally + (over ? 1 : 0), nTally + (over ? 1 : 0) == 1 ? L"" : L"s", sum,
                         capNote, nTally || over ? mods : L"none");
        Write(L"[SCAN]", line);
        CsvRow(L"census-summary", modName, mods, L"");
    }
}

// All-module census: every loaded module not yet censused gets one, read-only.
// detailAll=0 keeps detail lines for the interesting cases only: imports
// resolved into memory the loader did not map as an image (Denuvo-style
// manual-mapped DLL copies, unlinked code). New modules that appear later
// (a mod loading a helper) are picked up by the heartbeat tick.
#define MAX_CENSUS_SEEN 512
static ULONG_PTR g_censusSeen[MAX_CENSUS_SEEN]; static int g_nCensusSeen = 0;

static void CensusAllModules(void)
{
    HANDLE snap;
    if (!g_pid) g_pid = GetCurrentProcessId();
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, g_pid);
    if (snap == INVALID_HANDLE_VALUE) return;
    {
        MODULEENTRY32W me; me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me))
        {
            do
            {
                BYTE* b = (BYTE*)me.hModule;
                int i, seen = 0;
                for (i = 0; i < g_nCensusSeen; i++) if (g_censusSeen[i] == (ULONG_PTR)b) { seen = 1; break; }
                if (seen) continue;
                if (g_nCensusSeen < MAX_CENSUS_SEEN) g_censusSeen[g_nCensusSeen++] = (ULONG_PTR)b;
                __try { CensusBindings(b, me.szModule, 0); }
                __except (EXCEPTION_EXECUTE_HANDLER) { Trace(L"all-census fault"); }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
}

// Heartbeat report for the GetProcAddress thunks: any thunk that took calls
// since the last report prints with its per-interval and total counts and the
// last caller. Runs on the worker thread only; the counters themselves are
// touched by game threads (hence the volatile fields).
static void GpaThunkTick(void)
{
    int i;
    if (!g_td) return;
    for (i = 0; i < g_nThunksA && i < MAX_GPA_THUNKS; i++)
    {
        LONG now = g_td[i].calls;
        LONG prev = g_td[i].reported;
        if (now > prev)
        {
            WCHAR caller[300], msg[700];
            caller[0] = 0;
            DescribeModuleOf((void*)(ULONG_PTR)g_td[i].lastCaller, caller, 300);
            StringCchPrintfW(msg, 700, L"gpa    %hs called %d (total %d) last caller %s",
                             g_td[i].name, (int)(now - prev), (int)now,
                             caller[0] ? caller : L"?");
            Write(L"[HOOK]", msg);
            g_td[i].reported = now;
        }
    }
}


// Hook targets named in HookModules that were not loaded yet when the DLL
// started. artifact/dse load *after* GameConsole, so a name that cannot be
// resolved at startup is retried each heartbeat (and, for a module that is
// resolvable only by address - a hidden one like dse.dll - as soon as the
// caller attribution reports where it is).
#define MAX_PENDHOOK 16
static WCHAR g_pendHook[MAX_PENDHOOK][130];
static int g_nPendHook = 0;
static int HookOneModule(HMODULE mod, const WCHAR* modName);
static void InstallKernelbaseInterposition(void);
static int IsPendingHook(const WCHAR* name);
static void HookLateModules(void);

static void HookAddPending(const WCHAR* name)
{
    int i;
    for (i = 0; i < g_nPendHook; i++) if (_wcsicmp(g_pendHook[i], name) == 0) return;
    if (g_nPendHook >= MAX_PENDHOOK) return;
    StringCchCopyW(g_pendHook[g_nPendHook++], 130, name);
}

static void HookClearPending(const WCHAR* name)
{
    int i;
    for (i = 0; i < g_nPendHook; i++)
        if (_wcsicmp(g_pendHook[i], name) == 0)
        {
            if (i != g_nPendHook - 1)
                StringCchCopyW(g_pendHook[i], 130, g_pendHook[g_nPendHook - 1]);
            g_pendHook[--g_nPendHook][0] = 0;
            return;
        }
}

static int IsPendingHook(const WCHAR* name)
{
    int i;
    for (i = 0; i < g_nPendHook; i++) if (_wcsicmp(g_pendHook[i], name) == 0) return 1;
    return 0;
}

static void InstallIatHooks(int reads, int writes, int prot, int cap, const WCHAR* modules)
{
    int hooked = 0;
    const WCHAR* p;
    if (g_hooksActive) return;
    g_reads = reads; g_writes = writes; g_prot = prot;
    g_cap = cap > 0 ? cap : 2000;
    g_pid = GetCurrentProcessId();
    GetWindowsDirectoryW(g_win, 300);
    g_hooksActive = 1;
    if (!pStack) pStack = (t_CaptureStack)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "RtlCaptureStackBackTrace");
    if (!pQIF) pQIF = (t_NtQueryInfoFile)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile");

    p = modules;
    while (p && *p)
    {
        const WCHAR* e2; int cl;
        while (*p == L' ') p++;
        e2 = wcschr(p, L',');
        cl = e2 ? (int)(e2 - p) : (int)wcslen(p);
        while (cl > 0 && p[cl - 1] == L' ') cl--;
        if (cl > 0)
        {
            WCHAR modName[130];
            HMODULE mod;
            StringCchPrintfW(modName, 130, L"%.*s", cl, p);
            mod = GetModuleHandleW(modName);
            if (mod)
            {
                hooked += HookOneModule(mod, modName);
            }
            else
            {
                WCHAR msg[300];
                StringCchPrintfW(msg, 300, L"(iat) %s is not loaded yet - will retry each heartbeat (a mod's helper DLL loads after this one)", modName);
                HookNote(msg);
                HookAddPending(modName);
            }
        }
        p = e2 ? e2 + 1 : NULL;
    }
    HookLateModules();
    {
        int i;
        for (i = 0; i < g_nPendHook; i++)
        {
            WCHAR msg[300];
            StringCchPrintfW(msg, 300, L"(iat) still waiting for %s - it can be instrumented by address as soon as it calls a hooked API", g_pendHook[i]);
            HookNote(msg);
        }
    }
    (void)hooked;

    // ---- KERNELBASE middleman layer ----
    InstallKernelbaseInterposition();
}

// Instrument one module's import table. Separate function so a module that
// only appears later (a mod helper, or a hidden module like dse.dll) can be
// hooked on a retry, not just during startup.
static int HookOneModule(HMODULE mod, const WCHAR* modName)
{
    {
        int hooked = 0;
        if (mod)
        {
                // census FIRST: reads the original bound addresses before any
                // of them are repointed by the hooks below
                CensusBindings((BYTE*)mod, modName, 1);
                if (g_writes) hooked += HookIatApi(mod, "CreateFileW", (void*)HkIatCreateFileW, (void**)&pCreateFileW_, modName);
                if (g_reads)  hooked += HookIatApi(mod, "ReadFile", (void*)HkIatReadFile, (void**)&pReadFileW_, modName);
                if (g_writes) hooked += HookIatApi(mod, "WriteFile", (void*)HkIatWriteFile, (void**)&pWriteFileW_, modName);
                if (g_writes) hooked += HookIatApi(mod, "DeleteFileW", (void*)HkIatDeleteFileW, (void**)&pDeleteFileW_, modName);
                if (g_writes) hooked += HookIatApi(mod, "FindFirstFileExW", (void*)HkIatFindFirstFileExW, (void**)&pFFF_, modName);
                if (g_writes) hooked += HookIatApi(mod, "LoadLibraryW", (void*)HkIatLoadLibW, (void**)&pLoadLibW_, modName);
                if (g_writes) hooked += HookIatApi(mod, "LoadLibraryA", (void*)HkIatLoadLibA, (void**)&pLoadLibA_, modName);
                if (g_writes) hooked += HookIatApi(mod, "GetProcAddress", (void*)HkIatGPA, (void**)&pGPA_, modName);
                {
                    HMODULE nt = FindWorkingNtdll();
                    if (g_reads && !pIatNtRead_)
                    {
                        void* f = (void*)GetProcAddress(nt, "NtReadFile");
                        if (f && HookIatByValue(mod, f, (void*)HkIatNtRead, (void**)&pIatNtRead_))
                        { WCHAR msg[300]; StringCchPrintfW(msg, 300, L"(iat) NtReadFile (ordinal import) hooked in %s", modName); HookNote(msg); hooked++; }
                    }
                    if (g_writes && !pIatNtWrite_)
                    {
                        void* f = (void*)GetProcAddress(nt, "NtWriteFile");
                        if (f && HookIatByValue(mod, f, (void*)HkIatNtWrite, (void**)&pIatNtWrite_))
                        { WCHAR msg[300]; StringCchPrintfW(msg, 300, L"(iat) NtWriteFile (ordinal import) hooked in %s", modName); HookNote(msg); hooked++; }
                    }
                    if (g_writes && !pIatNtOpen_)
                    {
                        void* f = (void*)GetProcAddress(nt, "NtOpenFile");
                        if (f && HookIatByValue(mod, f, (void*)HkIatNtOpen, (void**)&pIatNtOpen_))
                        { WCHAR msg[300]; StringCchPrintfW(msg, 300, L"(iat) NtOpenFile (ordinal import) hooked in %s", modName); HookNote(msg); hooked++; }
                    }
                    if (!pModNtCKey_)
                    {
                        void* f = (void*)GetProcAddress(nt, "NtCreateKey");
                        if (f && HookIatByValue(mod, f, (void*)HkIatModNtCreateKey, (void**)&pModNtCKey_))
                        { WCHAR msg[300]; StringCchPrintfW(msg, 300, L"(iat) NtCreateKey (ordinal/shadow import) hooked in %s", modName); HookNote(msg); hooked++; }
                    }
                    if (!pModNtSVK_)
                    {
                        void* f = (void*)GetProcAddress(nt, "NtSetValueKey");
                        if (f && HookIatByValue(mod, f, (void*)HkIatModNtSetValueKey, (void**)&pModNtSVK_))
                        { WCHAR msg[300]; StringCchPrintfW(msg, 300, L"(iat) NtSetValueKey (ordinal/shadow import) hooked in %s", modName); HookNote(msg); hooked++; }
                    }
                    if (!pIatNtTerm_)
                    {
                        void* f = (void*)GetProcAddress(nt, "NtTerminateProcess");
                        if (f && HookIatByValue(mod, f, (void*)HkIatNtTerm, (void**)&pIatNtTerm_))
                        { WCHAR msg[300]; StringCchPrintfW(msg, 300, L"(iat) NtTerminateProcess hooked in %s", modName); HookNote(msg); hooked++; }
                    }
                    // artifact imports these two straight from ntdll: the
                    // KERNELBASE layer watches KERNELBASE's imports, so without
                    // this the probe is invisible. NtQueryInformationThread is
                    // anti-debug (ThreadHideFromDebugger et al), and
                    // NtQueryVolumeInformationFile is fingerprinting.
                    // By name first, so a module whose IAT is bound to a shadow
                    // ntdll copy is still matched; by value for ordinal imports.
                    hooked += HookIatApi(mod, "NtQueryInformationThread", (void*)HkIatModNtQit, (void**)&pModNtQIT_, modName);
                    hooked += HookIatApi(mod, "NtQueryVolumeInformationFile", (void*)HkIatModNtQvif, (void**)&pModNtQvif_, modName);
                    if (!pModNtQIT_)
                    {
                        void* f = (void*)GetProcAddress(nt, "NtQueryInformationThread");
                        if (f && HookIatByValue(mod, f, (void*)HkIatModNtQit, (void**)&pModNtQIT_))
                        { WCHAR msg[300]; StringCchPrintfW(msg, 300, L"(iat) NtQueryInformationThread (ordinal import) hooked in %s", modName); HookNote(msg); hooked++; }
                    }
                    if (!pModNtQvif_)
                    {
                        void* f = (void*)GetProcAddress(nt, "NtQueryVolumeInformationFile");
                        if (f && HookIatByValue(mod, f, (void*)HkIatModNtQvif, (void**)&pModNtQvif_))
                        { WCHAR msg[300]; StringCchPrintfW(msg, 300, L"(iat) NtQueryVolumeInformationFile (ordinal import) hooked in %s", modName); HookNote(msg); hooked++; }
                    }
                }
                {
                    HMODULE adv = GetModuleHandleW(L"advapi32.dll");
                    if (adv)
                    {
                        if (!pCreateSvc_) { void* f = (void*)GetProcAddress(adv, "CreateServiceW"); if (f) hooked += HookIatApi(mod, "CreateServiceW", (void*)HkIatCreateServiceW, (void**)&pCreateSvc_, modName); }
                        if (!pStartSvc_)  { void* f = (void*)GetProcAddress(adv, "StartServiceW");  if (f) hooked += HookIatApi(mod, "StartServiceW",  (void*)HkIatStartServiceW,  (void**)&pStartSvc_,  modName); }
                    }
                }
                {
                    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
                    HMODULE shl = GetModuleHandleW(L"shell32.dll");
                    if (k32)
                    {
                        if (g_writes) hooked += HookIatApi(mod, "VirtualAlloc", (void*)HkIatVirtualAlloc, (void**)&pVAlloc_, modName);
                        if (g_writes) hooked += HookIatApi(mod, "VirtualProtect", (void*)HkIatVirtualProtect, (void**)&pVProt_, modName);
                        if (g_writes) hooked += HookIatApi(mod, "VirtualFree", (void*)HkIatVirtualFree, (void**)&pVFree_, modName);
                        hooked += HookIatApi(mod, "CreateProcessW", (void*)HkIatCreateProcessW, (void**)&pCProc_, modName);
                        hooked += HookIatApi(mod, "SetThreadContext", (void*)HkIatSetThreadContext, (void**)&pSetTC_, modName);
                        hooked += HookIatApi(mod, "GetThreadContext", (void*)HkIatGetThreadContext, (void**)&pGetTC_, modName);
                        hooked += HookIatApi(mod, "SuspendThread", (void*)HkIatSuspendThread, (void**)&pSuspT_, modName);
                        hooked += HookIatApi(mod, "ResumeThread", (void*)HkIatResumeThread, (void**)&pResT_, modName);
                        hooked += HookIatApi(mod, "TerminateProcess", (void*)HkIatTerminateProcess, (void**)&pTermP_, modName);
                        if (g_writes) hooked += HookIatApi(mod, "CopyFileExW", (void*)HkIatCopyFileExW, (void**)&pCopyF_, modName);
                        if (g_writes) hooked += HookIatApi(mod, "DeleteService", (void*)HkIatDeleteService, (void**)&pDelSvc_, modName);
                        hooked += HookIatApi(mod, "OpenSCManagerW", (void*)HkIatOpenSCManagerW, (void**)&pOpenSCM_, modName);
                        // Registry bridge: a driver is registered by writing the
                        // service key (ImagePath/Type/Start). A protection that
                        // routes through a manual-mapped KERNELBASE#1/advapi32#1
                        // never reaches the KERNELBASE layer, so watch the same
                        // operations in this module's own import table too.
                        hooked += HookIatApi(mod, "NtCreateKey", (void*)HkIatModNtCreateKey, (void**)&pModNtCKey_, modName);
                        hooked += HookIatApi(mod, "ZwCreateKey", (void*)HkIatModNtCreateKey, (void**)&pModNtCKey_, modName);
                        hooked += HookIatApi(mod, "NtSetValueKey", (void*)HkIatModNtSetValueKey, (void**)&pModNtSVK_, modName);
                        hooked += HookIatApi(mod, "ZwSetValueKey", (void*)HkIatModNtSetValueKey, (void**)&pModNtSVK_, modName);
                        hooked += HookIatApi(mod, "RegCreateKeyExW", (void*)HkIatRegCreateKeyExW, (void**)&pRegCreateKeyExW_, modName);
                        hooked += HookIatApi(mod, "RegSetValueExW", (void*)HkIatRegSetValueExW, (void**)&pRegSetValueExW_, modName);
                        hooked += HookIatApi(mod, "MessageBoxW", (void*)HkIatMessageBoxW, (void**)&pMsgBox_, modName);
                        hooked += HookIatApi(mod, "IsDebuggerPresent", (void*)HkIatIsDebuggerPresent, (void**)&pIsDbg_, modName);
                        hooked += HookIatApi(mod, "VirtualQuery", (void*)HkIatVirtualQuery, (void**)&pVQuery_, modName);
                        hooked += HookIatApi(mod, "VirtualQueryEx", (void*)HkIatVirtualQueryEx, (void**)&pVQueryEx_, modName);
                        hooked += HookIatApi(mod, "ReadProcessMemory", (void*)HkIatReadProcessMemory, (void**)&pRPM_, modName);
                        hooked += HookIatApi(mod, "AddVectoredExceptionHandler", (void*)HkIatVehAdd, (void**)&pVehAdd_, modName);
                        hooked += HookIatApi(mod, "SetUnhandledExceptionFilter", (void*)HkIatSetUeh, (void**)&pSetUeh_, modName);
                    }
                    {
                        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
                        if (nt)
                        {
                            hooked += HookIatApi(mod, "RtlAddVectoredExceptionHandler", (void*)HkIatVehAdd, (void**)&pVehAdd_, modName);
                            hooked += HookIatApi(mod, "RtlRemoveVectoredExceptionHandler", (void*)HkIatVehRemove, (void**)&pVehRem_, modName);
                            hooked += HookIatApi(mod, "RtlAddVectoredContinueHandler", (void*)HkIatVehContAdd, (void**)&pVehContAdd_, modName);
                            hooked += HookIatApi(mod, "RtlRemoveVectoredContinueHandler", (void*)HkIatVehContRemove, (void**)&pVehContRem_, modName);
                        }
                    }
                    if (shl) hooked += HookIatApi(mod, "ShellExecuteW", (void*)HkIatShellExecuteW, (void**)&pShExec_, modName);
                    if (g_writes && !pCreateDir_)
                    {
                        void* f = (void*)GetProcAddress(k32 ? k32 : GetModuleHandleW(L"KERNELBASE.dll"), "CreateDirectoryW");
                        if (f && HookIatByValue(mod, f, (void*)HkIatCreateDirectoryW, (void**)&pCreateDir_))
                        { WCHAR msg[300]; StringCchPrintfW(msg, 300, L"(iat) CreateDirectoryW (ordinal import) hooked in %s", modName); HookNote(msg); hooked++; }
                    }
                }
            }
        return hooked;
    }
}

// Try again for hook targets that were not loaded at startup. Runs on the
// worker, where the loader APIs are safe.
static void HookLateModules(void)
{
    int i;
    for (i = 0; i < g_nPendHook; i++)
    {
        HMODULE m = GetModuleHandleW(g_pendHook[i]);
        if (!m) continue;
        {
            WCHAR name[130], msg[400];
            int n;
            StringCchCopyW(name, 130, g_pendHook[i]);
            n = HookOneModule(m, name);
            StringCchPrintfW(msg, 400, L"(iat) %s loaded late - instrumented now (%d imports)", name, n);
            HookNote(msg);
            CsvRow(L"hook-late", name, msg, L"");
            HookClearPending(name);
            i--;
        }
    }
}

// Worker side of the deferred caller attribution (the hook path only queues
// bare addresses): resolve each address, report a module that toolhelp32 does
// not list as hidden, add it to the caller-range table so later calls are named
// immediately, and instrument it if it is a pending HookModules target.
static ULONG_PTR g_hiddenReported2[32]; static int g_nHiddenReported2 = 0;

// SizeOfImage from the mapped PE headers (the shadow/hidden copies keep their
// headers), so a hidden module is reported with its real size rather than the
// size of whichever region happens to start at its base.
static ULONG_PTR ImageSizeAt(ULONG_PTR base)
{
    ULONG_PTR size = 0;
    __try
    {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic == IMAGE_DOS_SIGNATURE)
        {
            IMAGE_NT_HEADERS* nth = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
            if (nth->Signature == IMAGE_NT_SIGNATURE)
                size = nth->OptionalHeader.SizeOfImage;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { size = 0; }
    return size;
}

// Is this base in the process's real PEB loader list at all? Toolhelp32 can miss
// modules - a snapshot taken while the loader is busy, or a relinked list returns
// a short view - and a false "hidden module" is worse than no report at all: on
// the elevated PB0 run of 2026-11-16 20:22 our own GameConsole.dll, ntdll,
// KERNEL32, KERNELBASE and even PBZ.exe itself were all labelled hidden because
// toolhelp's view was stale at that instant (the protection relinks the loader
// lists, so this is a live hazard, not a one-off). "Hidden" must mean absent from
// toolhelp32 *and* absent from the PEB lists - which is what dse.dll is.
static int InPebLoadList(ULONG_PTR base)
{
    typedef struct { LIST_ENTRY Load, Memory, Init; void* DllBase; } ENT;
    LIST_ENTRY* head;
    LIST_ENTRY* cur;
    int found = 0, guard = 0;
    __try
    {
        BYTE* peb = (BYTE*)__readgsqword(0x60);     // gs:[0x60] = PEB (no winternl.h here)
        BYTE* ldr = *(BYTE**)(peb + 0x18);
        head = (LIST_ENTRY*)(ldr + 0x10);           // InLoadOrderModuleList
        cur = head->Flink;
        while (cur && cur != head && guard++ < 20000)
        {
            if ((ULONG_PTR)((ENT*)cur)->DllBase == base) { found = 1; break; }
            cur = cur->Flink;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { found = 0; }
    return found;
}

static void ResolveCallerAddrs(void)
{
    LONG nc = InterlockedExchange(&g_nCaddr, 0);
    int i;
    if (nc > MAX_CADDR) nc = MAX_CADDR;
    // refresh FIRST: "hidden" must mean absent from toolhelp right now, not
    // merely absent from a table that predates a module loading (an ordinary
    // DLL loaded on demand would otherwise be mislabelled hidden)
    if (nc) RefreshCallerRanges();
    for (i = 0; i < nc; i++)
    {
        ULONG_PTR a = g_caddr[i];
        HMODULE m = NULL;
        WCHAR path[520], name[64];
        const WCHAR* b;
        DWORD len, j;
        if (!a) continue;
        if (!GetModuleHandleExW(0x4 | 0x2, (LPCWSTR)a, &m) || !m) continue;
        if (CallerRangeFor((ULONG_PTR)m, NULL, NULL, NULL, NULL)) continue;   // already known
        len = GetModuleFileNameW(m, path, 520);
        b = path;
        for (j = 0; j < len; j++) if (path[j] == L'\\' || path[j] == L'/') b = path + j + 1;
        StringCchCopyW(name, 64, (b && b[0]) ? b : L"(unnamed)");
        {
            MEMORY_BASIC_INFORMATION mb;
            ULONG_PTR size = 0;
            int k, dup = 0;
            memset(&mb, 0, sizeof(mb));
            if (VirtualQuery((void*)m, &mb, sizeof(mb)) && (ULONG_PTR)mb.BaseAddress == (ULONG_PTR)m)
                size = mb.RegionSize;
            {
                ULONG_PTR pe = ImageSizeAt((ULONG_PTR)m);
                if (pe > size) size = pe;
            }
            for (k = 0; k < g_nHiddenReported2; k++) if (g_hiddenReported2[k] == (ULONG_PTR)m) { dup = 1; break; }
            if (!dup && ScanLdr)
            {
                WCHAR line[700];
                if (g_nHiddenReported2 < 32) g_hiddenReported2[g_nHiddenReported2++] = (ULONG_PTR)m;
                if (InPebLoadList((ULONG_PTR)m))
                {
                    // toolhelp did not show it, the real loader list does: a stale
                    // snapshot, not a hidden module. Say so once (silence would be
                    // a silent filter) and name its callers from now on.
                    StringCchPrintfW(line, 700,
                        L"ldr: %s @0x%llX size 0x%llX is in the PEB loader list but was absent from the toolhelp32 snapshot - stale snapshot, NOT a hidden module",
                        name, (unsigned long long)(ULONG_PTR)m, (unsigned long long)size);
                    Write(L"[SCAN]", line);
                    CsvRow(L"ldr-stale", name, line, L"");
                }
                else
                {
                    StringCchPrintfW(line, 700,
                        L"hidden module: %s @0x%llX size 0x%llX type 0x%X protect 0x%X - resolvable by GetModuleHandle, absent from toolhelp32 AND from the PEB loader lists (verified)",
                        name, (unsigned long long)(ULONG_PTR)m, (unsigned long long)size,
                        (unsigned)mb.Type, (unsigned)mb.Protect);
                    Write(L"[SCAN]", line);
                    CsvRow(L"ldr-hidden", name, line, L"");
                }
            }
            CallerAddRange((ULONG_PTR)m, size, name, 0, 1);
            if (InPebLoadList((ULONG_PTR)m)) continue;   // normal module, nothing hidden about it
        }
        if (IsPendingHook(name))
        {
            WCHAR msg[400];
            if (!HookHiddenModules)
            {
                // Reported, censused and named - but deliberately NOT rewritten.
                // A module that is in no loader list keeps itself hidden for a
                // reason, and repointing its imports while it is running has
                // ended the game at that exact instant three times out of three
                // on PB0 (the row is `[SVC]/[SCAN] hidden module` followed by
                // the census, then a UE fatal error). Observation is cheap;
                // tampering with the helper is not. Opt in with
                // HookHiddenModules=true when a session can afford to die.
                StringCchPrintfW(msg, 400,
                    L"(iat) %s is a hidden module (absent from toolhelp32) - reported but NOT instrumented (HookHiddenModules=false)", name);
                HookNote(msg);
                CsvRow(L"hook-hidden", name, msg, L"");
                HookClearPending(name);
                continue;
            }
            {
                int n = HookOneModule(m, name);
                StringCchPrintfW(msg, 400, L"(iat) %s located by address (hidden module, absent from toolhelp32) - instrumented now (%d imports)", name, n);
                HookNote(msg);
                CsvRow(L"hook-late", name, msg, L"");
                HookClearPending(name);
            }
        }
    }
}

// ---- KERNELBASE middleman layer ----
// Every KERNELBASE image in the process (the OS one and any Denuvo shadow
// copy) gets its imported Nt* functions repointed, which catches calls from
// *any* module that goes through KERNELBASE - including a hidden module that
// was never in any loader list.
static void InstallKernelbaseInterposition(void)
{
    int hooked = 0;
    {
        HMODULE kbs[8]; int nkb = 0, i, kb = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, g_pid);
        if (snap != INVALID_HANDLE_VALUE)
        {
            MODULEENTRY32W me; me.dwSize = sizeof(me);
            if (Module32FirstW(snap, &me))
            {
                do
                {
                    if (_wcsicmp(me.szModule, L"KERNELBASE.dll") == 0 && nkb < 8)
                    {
                        int dup = 0;
                        for (i = 0; i < nkb; i++) if (kbs[i] == me.hModule) dup = 1;
                        if (!dup) kbs[nkb++] = me.hModule;
                    }
                } while (Module32NextW(snap, &me));
            }
            CloseHandle(snap);
        }
        if (!nkb) kbs[nkb++] = GetModuleHandleW(L"KERNELBASE.dll");
        for (i = 0; i < nkb; i++)
        {
            if (g_writes) kb += HookIatName(kbs[i], "NtCreateFile", (void*)HkIatKbNtCreate, (void**)&pIatNtCreate_);
            if (g_writes) kb += HookIatName(kbs[i], "NtOpenFile", (void*)HkIatKbNtOpen, (void**)&pIatNtOpen_);
            if (g_reads)  kb += HookIatName(kbs[i], "NtReadFile", (void*)HkIatKbNtRead, (void**)&pIatNtRead_);
            if (g_writes) kb += HookIatName(kbs[i], "NtWriteFile", (void*)HkIatKbNtWrite, (void**)&pIatNtWrite_);
            if (g_prot)   kb += HookIatName(kbs[i], "NtProtectVirtualMemory", (void*)HkIatKbNtProt, (void**)&pIatNtProt_);
            kb += HookIatName(kbs[i], "NtQueryInformationProcess", (void*)HkIatKbNtQip, (void**)&pIatNtQIP_);
            kb += HookIatName(kbs[i], "NtSetInformationThread", (void*)HkIatKbNtSit, (void**)&pIatNtSIT_);
            kb += HookIatName(kbs[i], "NtCreateThreadEx", (void*)HkIatKbNtCtex, (void**)&pIatNtCTEX_);
            kb += HookIatName(kbs[i], "NtAllocateVirtualMemory", (void*)HkIatKbNtAvm, (void**)&pIatNtAVM_);
            kb += HookIatName(kbs[i], "NtSetContextThread", (void*)HkIatKbNtSct, (void**)&pIatNtSCT_);
            kb += HookIatName(kbs[i], "NtGetContextThread", (void*)HkIatKbNtGct, (void**)&pIatNtGCT_);
            kb += HookIatName(kbs[i], "NtQueryInformationThread", (void*)HkIatKbNtQit, (void**)&pIatNtQIT_);
            kb += HookIatName(kbs[i], "NtSetInformationProcess", (void*)HkIatKbNtSip, (void**)&pIatNtSIP_);
            kb += HookIatName(kbs[i], "NtQueryVirtualMemory", (void*)HkIatKbNtQvm, (void**)&pIatNtQVM_);
            kb += HookIatName(kbs[i], "NtReadVirtualMemory", (void*)HkIatKbNtRvm, (void**)&pIatNtRVM_);
            kb += HookIatName(kbs[i], "NtWriteVirtualMemory", (void*)HkIatKbNtWvm, (void**)&pIatNtWVM_);
            kb += HookIatName(kbs[i], "NtOpenThread", (void*)HkIatKbNtOt, (void**)&pIatNtOT_);
            kb += HookIatName(kbs[i], "NtCreateKey", (void*)HkIatKbNtCreateKey, (void**)&pIatNtCKey_);
            kb += HookIatName(kbs[i], "NtSetValueKey", (void*)HkIatKbNtSetValueKey, (void**)&pIatNtSVK_);
            kb += HookIatName(kbs[i], "NtSetSystemInformation", (void*)HkIatKbNtSsi, (void**)&pIatNtSsi_);
        }
        {
            HMODULE nt = FindWorkingNtdll();
            // resolved here, on the worker, so RegPathOfKey never reaches for the
            // loader from inside a hook (hard rule 6)
            if (!pNtQK_) pNtQK_ = (t_NtQueryKey)(void*)GetProcAddress(nt, "NtQueryKey");
            for (i = 0; i < nkb; i++)
            {
                if (g_writes && !pIatNtCreate_) { void* f = (void*)GetProcAddress(nt, "NtCreateFile"); if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtCreate, (void**)&pIatNtCreate_); }
                if (g_writes && !pIatNtOpen_)   { void* f = (void*)GetProcAddress(nt, "NtOpenFile");   if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtOpen,   (void**)&pIatNtOpen_); }
                if (g_reads  && !pIatNtRead_)   { void* f = (void*)GetProcAddress(nt, "NtReadFile");   if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtRead,   (void**)&pIatNtRead_); }
                if (g_writes && !pIatNtWrite_)  { void* f = (void*)GetProcAddress(nt, "NtWriteFile");  if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtWrite,  (void**)&pIatNtWrite_); }
                if (g_prot   && !pIatNtProt_)   { void* f = (void*)GetProcAddress(nt, "NtProtectVirtualMemory"); if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtProt, (void**)&pIatNtProt_); }
                if (!pIatNtQIP_)  { void* f = (void*)GetProcAddress(nt, "NtQueryInformationProcess"); if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtQip,  (void**)&pIatNtQIP_); }
                if (!pIatNtSIT_)  { void* f = (void*)GetProcAddress(nt, "NtSetInformationThread");    if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtSit,  (void**)&pIatNtSIT_); }
                if (!pIatNtCTEX_) { void* f = (void*)GetProcAddress(nt, "NtCreateThreadEx");          if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtCtex, (void**)&pIatNtCTEX_); }
                if (!pIatNtAVM_)  { void* f = (void*)GetProcAddress(nt, "NtAllocateVirtualMemory");   if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtAvm,  (void**)&pIatNtAVM_); }
                if (!pIatNtSCT_)  { void* f = (void*)GetProcAddress(nt, "NtSetContextThread");        if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtSct,  (void**)&pIatNtSCT_); }
                if (!pIatNtGCT_)  { void* f = (void*)GetProcAddress(nt, "NtGetContextThread");        if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtGct,  (void**)&pIatNtGCT_); }
                if (!pIatNtQIT_)  { void* f = (void*)GetProcAddress(nt, "NtQueryInformationThread");   if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtQit,  (void**)&pIatNtQIT_); }
                if (!pIatNtSIP_)  { void* f = (void*)GetProcAddress(nt, "NtSetInformationProcess");    if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtSip,  (void**)&pIatNtSIP_); }
                if (!pIatNtQVM_)  { void* f = (void*)GetProcAddress(nt, "NtQueryVirtualMemory");       if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtQvm,  (void**)&pIatNtQVM_); }
                if (!pIatNtRVM_)  { void* f = (void*)GetProcAddress(nt, "NtReadVirtualMemory");        if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtRvm,  (void**)&pIatNtRVM_); }
                if (!pIatNtWVM_)  { void* f = (void*)GetProcAddress(nt, "NtWriteVirtualMemory");       if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtWvm,  (void**)&pIatNtWVM_); }
                if (!pIatNtOT_)   { void* f = (void*)GetProcAddress(nt, "NtOpenThread");               if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtOt,   (void**)&pIatNtOT_); }
                if (!pIatNtCKey_) { void* f = (void*)GetProcAddress(nt, "NtCreateKey");                if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtCreateKey, (void**)&pIatNtCKey_); }
                if (!pIatNtSVK_)  { void* f = (void*)GetProcAddress(nt, "NtSetValueKey");              if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtSetValueKey, (void**)&pIatNtSVK_); }
                if (!pIatNtSsi_)  { void* f = (void*)GetProcAddress(nt, "NtSetSystemInformation");     if (f) kb += HookIatByValue(kbs[i], f, (void*)HkIatKbNtSsi, (void**)&pIatNtSsi_); }
            }
        }
        {
            WCHAR msg[400];
            StringCchPrintfW(msg, 400,
                L"(iat) KERNELBASE interposition layer: %d Nt* imports hooked across %d KERNELBASE image(s) - game traffic through artifact's trampolines shows here (reads/writes sub-capped)",
                kb, nkb);
            HookNote(msg);
        }
        hooked += kb;
    }

    {
        WCHAR msg[300];
        StringCchPrintfW(msg, 300, L"(iat) %d imports hooked via import tables (no code bytes modified)", hooked);
        HookNote(msg);
    }
}

// ---------------------------------------------------------------- legacy ntdll patching
static BYTE* AllocTramp(void)
{
    if (!trampNext || trampNext + 64 > trampEnd)
    {
        BYTE* page = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!page) return NULL;
        trampNext = page; trampEnd = page + 4096;
    }
    {
        BYTE* p = trampNext; trampNext += 64; return p;
    }
}

static void HookSkip(const char* name, const char* why)
{
    WCHAR wname[64], wwhy[200], msg[300];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64);
    MultiByteToWideChar(CP_UTF8, 0, why, -1, wwhy, 200);
    StringCchPrintfW(msg, 300, L"(skip) %s: %s", wname, wwhy);
    HookNote(msg);
}

static int PatchEx(const char* name, BYTE* target, void* hook, void** slot)
{
    DWORD oldp = 0, oldp2 = 0;
    if (!target) { HookSkip(name, "export not found"); return 0; }
    if (!*slot)
    {
        BYTE b8;
        BYTE* t;
        if (!(target[0] == 0x4C && target[1] == 0x8B && target[2] == 0xD1 && target[3] == 0xB8))
        { HookSkip(name, "unexpected prologue (shimmed or already hooked)"); return 0; }
        b8 = target[8];
        if (!(b8 == 0x0F || b8 == 0xF6 || b8 == 0xC3 || b8 == 0xE9 || b8 == 0xEB))
        { HookSkip(name, "unexpected stub body"); return 0; }
        t = AllocTramp();
        if (!t) { HookSkip(name, "no trampoline memory"); return 0; }
        memcpy(t, target, 16);
        t[16] = 0xFF; t[17] = 0x25; *(DWORD*)(t + 18) = 0;
        *(ULONG_PTR*)(t + 22) = (ULONG_PTR)(target + 16);
        *slot = t;
    }
    if (!VirtualProtect(target, 16, PAGE_EXECUTE_READWRITE, &oldp))
    { HookSkip(name, "VirtualProtect failed"); return 0; }
    target[14] = 0x90; target[15] = 0x90;
    target[0] = 0xFF; target[1] = 0x25; *(DWORD*)(target + 2) = 0;
    *(ULONG_PTR*)(target + 6) = (ULONG_PTR)hook;
    FlushInstructionCache(GetCurrentProcess(), target, 16);
    VirtualProtect(target, 16, oldp, &oldp2);
    return 1;
}

static void SuspendAll(void)
{
    int pass;
    for (pass = 0; pass < 3; pass++)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te; DWORD self = GetCurrentThreadId();
        if (snap == INVALID_HANDLE_VALUE) break;
        te.dwSize = sizeof(te);
        if (Thread32First(snap, &te))
        {
            do
            {
                int known = 0, i;
                HANDLE h;
                if (te.th32OwnerProcessID != g_pid || te.th32ThreadID == self) continue;
                for (i = 0; i < g_nsusp; i++) if (g_suspTid[i] == te.th32ThreadID) { known = 1; break; }
                if (known) continue;
                h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (h && SuspendThread(h) != (DWORD)-1 && g_nsusp < 1024)
                { g_susp[g_nsusp] = h; g_suspTid[g_nsusp] = te.th32ThreadID; g_nsusp++; }
                else if (h) CloseHandle(h);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }
}

static void ResumeAll(void)
{
    int i;
    for (i = 0; i < g_nsusp; i++) { ResumeThread(g_susp[i]); CloseHandle(g_susp[i]); }
    g_nsusp = 0;
}

static void InstallLegacyHooks(int reads, int writes, int prot, int onlyGame, int cap, const WCHAR* filter)
{
    HMODULE nts[8]; int nnt = 0, i, ok = 0;
    HANDLE snap;
    if (g_hooksActive) return;
    g_reads = reads; g_writes = writes; g_prot = prot; g_onlyGame = onlyGame;
    g_cap = cap > 0 ? cap : 2000;
    g_pid = GetCurrentProcessId();
    GetWindowsDirectoryW(g_win, 300);
    if (filter && *filter) StringCchCopyW(g_filter, 600, filter);
    g_hooksActive = 1;
    if (!pStack) pStack = (t_CaptureStack)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "RtlCaptureStackBackTrace");
    if (!pQIF) pQIF = (t_NtQueryInfoFile)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile");

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, g_pid);
    if (snap != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W me; me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me))
        {
            do
            {
                if (_wcsicmp(me.szModule, L"ntdll.dll") == 0 && nnt < 8)
                {
                    int dup = 0;
                    for (i = 0; i < nnt; i++) if (nts[i] == me.hModule) dup = 1;
                    if (!dup) nts[nnt++] = me.hModule;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    if (!nnt) nts[nnt++] = GetModuleHandleW(L"ntdll.dll");

    SuspendAll();
    for (i = 0; i < nnt; i++)
    {
        HMODULE nt = nts[i];
        if (g_writes) ok += PatchEx("NtOpenFile", (BYTE*)GetProcAddress(nt, "NtOpenFile"), HkOpenFile, &tOpen);
        if (g_writes) ok += PatchEx("NtCreateFile", (BYTE*)GetProcAddress(nt, "NtCreateFile"), HkCreateFile, &tCreate);
        if (g_reads)  ok += PatchEx("NtReadFile", (BYTE*)GetProcAddress(nt, "NtReadFile"), HkReadFile, &tRead);
        if (g_writes) ok += PatchEx("NtWriteFile", (BYTE*)GetProcAddress(nt, "NtWriteFile"), HkWriteFile, &tWrite);
        if (g_prot)   ok += PatchEx("NtProtectVirtualMemory", (BYTE*)GetProcAddress(nt, "NtProtectVirtualMemory"), HkProtect, &tProtect);
    }
    ResumeAll();

    {
        WCHAR msg[300];
        StringCchPrintfW(msg, 300, L"hooks installed: %d exports patched across %d ntdll image(s)", ok, nnt);
        HookNote(msg);
    }
}

// ---------------------------------------------------------------- console + scrape
static int Coord(int x, int y) { return (y << 16) | (x & 0xFFFF); }

static void SetupConsole(void)
{
    HANDLE hOut, hIn;
    DWORD mode;
    if (!AllocConsoleFlag) return;
    AllocConsole();
    SetConsoleOutputCP(65001);
    SetConsoleTitleW(g_consoleTitle);
    if (!ConsoleVisible) { HWND w = GetConsoleWindow(); if (w) ShowWindow(w, 0); }

    hOut = CreateFileW(L"CONOUT$", 0xC0000000, 3, NULL, OPEN_EXISTING, 0, NULL);
    hIn  = CreateFileW(L"CONIN$",  0xC0000000, 3, NULL, OPEN_EXISTING, 0, NULL);
    if (RedirectStdHandles)
    {
        SetStdHandle((DWORD)-11, hOut);
        SetStdHandle((DWORD)-12, hOut);
        SetStdHandle((DWORD)-10, hIn);
    }
    g_hOut = hOut;
    g_hIn = hIn;
    SetConsoleScreenBufferSize(hOut, (COORD) { (SHORT)ConsoleCols, (SHORT)ConsoleRows });
    // Read back the ACTUAL width: the resize can fail (some hosts), and Write
    // needs the real width to keep tagged lines from wrapping.
    {
        CONSOLE_SCREEN_BUFFER_INFO ci;
        if (GetConsoleScreenBufferInfo(hOut, &ci) && ci.dwSize.X > 8 && ci.dwSize.X <= 4096)
            g_conWidth = ci.dwSize.X;
    }
    if (GetConsoleMode(hOut, &mode)) SetConsoleMode(hOut, mode | 0x0004);
}

static int g_scrapedRows = 0;
static DWORD g_rowHash[4096];   // content hash per row, dedupes re-scrapes
static FILETIME g_startTime;

static void OpenLogFile(void)
{
    WCHAR dir[520], hdr[1200]; char utf[2400]; int n; DWORD w; SYSTEMTIME t;
    HANDLE h;
    if (!SaveLogFile) return;
    if (g_logDirectory[0]) StringCchCopyW(dir, 520, g_logDirectory);
    else { StringCchCopyW(dir, 520, g_dllDir); StringCchCatW(dir, 520, L"\\logs"); }
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        GetTempPathW(520, dir);

    GetLocalTime(&t);
    StringCchPrintfW(g_logPath, 520, L"%s\\GameConsole_%04u%02u%02u_%02u%02u%02u_pid%u.log",
                     dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, g_pid);
    h = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { Trace(L"log file failed"); return; }
    g_logHandle = h;

    {
        WCHAR exe[520];
        GetModuleFileNameW(NULL, exe, 520);
        StringCchPrintfW(hdr, 1200, L"# GameConsole session %04u-%02u-%02uT%02u:%02u:%02u.%03u  exe=%s\r\n",
                         t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, exe);
    }
    n = WideCharToMultiByte(CP_UTF8, 0, hdr, -1, utf, sizeof(utf) - 1, NULL, NULL);
    if (n > 1) WriteFile(g_logHandle, utf, (DWORD)(n - 1), &w, NULL);
}

// ---------------------------------------------------------------- csv export
// Machine-readable companion to the log: one row per census/survey/hook/mailbox
// finding, so the artifact<->denuvo map can be built offline. Writes happen on
// the worker thread only.
static HANDLE g_csvHandle = INVALID_HANDLE_VALUE;
static WCHAR g_csvPath[520] = L"";

static void CsvOpen(void)
{
    WCHAR dir[520];
    char hdr[] = "time,category,field1,field2,field3\r\n";
    DWORD w;
    if (!CsvExport || !SaveLogFile || g_logPath[0] == 0) return;
    StringCchCopyW(dir, 520, g_logPath);
    { WCHAR* s = wcsrchr(dir, L'\\'); if (s) *s = 0; }
    StringCchPrintfW(g_csvPath, 520, L"%s\\GameConsole_pid%lu.csv", dir, g_pid);
    g_csvHandle = CreateFileW(g_csvPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_csvHandle != INVALID_HANDLE_VALUE) WriteFile(g_csvHandle, hdr, (DWORD)strlen(hdr), &w, NULL);
}

// Append one field, always quoted, doubling any embedded quote. Keeps the row
// structure intact whatever a module name or message contains.
static void CsvField(WCHAR* out, int cch, int* off, const WCHAR* s)
{
    int i;
    if (*off < cch - 3) out[(*off)++] = L'"';
    for (i = 0; s && s[i] && *off < cch - 3; i++)
    {
        out[(*off)++] = s[i];
        if (s[i] == L'"' && *off < cch - 3) out[(*off)++] = L'"';
    }
    if (*off < cch - 3) out[(*off)++] = L'"';
    out[*off] = 0;
}

static void CsvRow(const WCHAR* cat, const WCHAR* a, const WCHAR* b, const WCHAR* c)
{
    SYSTEMTIME t; WCHAR line[1400]; char utf[4200]; int n, off = 0; DWORD w;
    if (!CsvExport || g_csvHandle == INVALID_HANDLE_VALUE) return;
    GetLocalTime(&t);
    off = _snwprintf(line, 1400, L"%02u:%02u:%02u.%03u,", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    if (off < 0) return;
    CsvField(line, 1400, &off, cat);
    StringCchCatW(line, 1400, L",");  off = (int)wcslen(line);
    CsvField(line, 1400, &off, a);
    StringCchCatW(line, 1400, L",");  off = (int)wcslen(line);
    CsvField(line, 1400, &off, b);
    StringCchCatW(line, 1400, L",");  off = (int)wcslen(line);
    CsvField(line, 1400, &off, c);
    StringCchCatW(line, 1400, L"\r\n");
    n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf, sizeof(utf) - 1, NULL, NULL);
    if (n > 1) WriteFile(g_csvHandle, utf, (DWORD)(n - 1), &w, NULL);
}

static void ScrapeConsole(void)
{
    CONSOLE_SCREEN_BUFFER_INFO ci;
    WCHAR row[1024];
    char utf[4096];
    int width, y;
    if (g_logHandle == INVALID_HANDLE_VALUE || !g_hOut) return;
    if (!GetConsoleScreenBufferInfo(g_hOut, &ci)) return;
    width = ci.srWindow.Right - ci.srWindow.Left + 1;
    (void)width;
    width = ci.dwSize.X;
    if (width <= 0 || width > 1000) return;
    if (ci.dwCursorPosition.Y < g_scrapedRows) g_scrapedRows = 0;

    if (g_wrLockInit) EnterCriticalSection(&g_wrLock);
    for (y = g_scrapedRows; y < ci.dwCursorPosition.Y; y++)
    {
        DWORD got = 0;
        int len, i;
        int ours, tagged, skip = 0;
        WCHAR out[1400];
        if (!ReadConsoleOutputCharacterW(g_hOut, row, (DWORD)width, (COORD) { 0, (SHORT)y }, &got)) break;
        len = (int)got;
        while (len > 0 && (row[len - 1] == L' ' || row[len - 1] == L'\t')) len--;
        if (len == 0) continue;
        // Content-hash dedupe. External console output (the game/mods writing
        // CR / ANSI cursor sequences through their redirected CRT) can move
        // the cursor BACKWARD, which resets g_scrapedRows and would re-save
        // everything still in the buffer (duplicate blocks with stale
        // timestamps). Skip a re-scraped row whose content is identical to
        // what was already saved from this row position; log it if changed.
        {
            DWORD h = 2166136261u; int yi = y & 4095;
            for (i = 0; i < len; i++) { h ^= (DWORD)row[i]; h *= 16777619u; }
            if (h == g_rowHash[yi]) continue;
            g_rowHash[yi] = h;
        }
        ours = ShowTimestamps
            ? (len > 12 && row[2] == L':' && row[5] == L':' && row[8] == L'.')
            : (row[0] == L'[');
        tagged = ours || (len >= 4 && row[0] == L' ' && row[1] == L' ' && row[2] == L' ' && row[3] == L' ');
        // Tagged lines are already in the log - Write() appends them directly.
        // The scrape relays RAW game output only, so tagged rows are always
        // skipped here (no more duplicate/stale-timestamp blocks from
        // re-scrapes, and no wrap fragments of our own lines).
        if (tagged) continue;
        if (!SaveRawLines) skip = 1;
        if (skip) continue;
        if (!tagged)
        {
            SYSTEMTIME t; int plen; GetLocalTime(&t);
            StringCchPrintfW(out, 1400, L"%02u:%02u:%02u.%03u [raw] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
            plen = (int)wcslen(out);
            if (len > 1398 - plen) len = 1398 - plen;
            for (i = 0; i < len; i++) out[plen + i] = row[i];
            out[plen + len] = 0;
        }
        else
        {
            if (len > 1398) len = 1398;
            for (i = 0; i < len; i++) out[i] = row[i];
            out[len] = 0;
        }
        {
            WCHAR line[1450]; int n; DWORD w;
            StringCchPrintfW(line, 1450, L"%s\r\n", out);
            n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf, sizeof(utf) - 1, NULL, NULL);
            if (n > 1) WriteFile(g_logHandle, utf, (DWORD)(n - 1), &w, NULL);
        }
    }
    g_scrapedRows = ci.dwCursorPosition.Y;
    if (ci.dwCursorPosition.Y > ci.dwSize.Y - 200)
    {
        DWORD written = 0;
        FillConsoleOutputCharacterW(g_hOut, L' ', (DWORD)(ci.dwSize.X * ci.dwSize.Y), (COORD) { 0, 0 }, &written);
        SetConsoleCursorPosition(g_hOut, (COORD) { 0, 0 });
        g_scrapedRows = 0;
    }
    if (g_wrLockInit) LeaveCriticalSection(&g_wrLock);
}

// ---------------------------------------------------------------- CRT redirect
static void RedirectGameCrt(void)
{
    HMODULE ucrt;
    typedef FILE* (__cdecl* t_iob)(int);
    typedef int   (__cdecl* t_freopen_s)(FILE**, const char*, const char*, FILE*);
    t_iob iob;
    t_freopen_s fr;
    FILE* f;
    ucrt = GetModuleHandleW(L"ucrtbase.dll");
    if (!ucrt) { Write(L"[console]", L"ucrtbase not loaded (static or old CRT?)"); return; }
    iob = (t_iob)(void*)GetProcAddress(ucrt, "__acrt_iob_func");
    fr = (t_freopen_s)(void*)GetProcAddress(ucrt, "freopen_s");
    if (!iob || !fr) return;
    fr(&f, "CONIN$", "r", iob(0));
    fr(&f, "CONOUT$", "w", iob(1));
    fr(&f, "CONOUT$", "w", iob(2));
}

// ---------------------------------------------------------------- module notify install
static void InstallModNotify(void)
{
    typedef LONG (NTAPI* t_LdrReg)(ULONG, void*, void*, void**);
    t_LdrReg reg = (t_LdrReg)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    void* cookie = NULL;
    if (!reg) return;
    reg(0, (void*)ModNotify, NULL, &cookie);
}

static void ListPresentModules(void)
{
    HANDLE snap = INVALID_HANDLE_VALUE;
    int i, count = 0;
    for (i = 0; i < 5; i++)
    {
        snap = CreateToolhelp32Snapshot(0x00000008 | 0x00000010 /*MODULE|MODULE32*/, g_pid);
        if (snap != INVALID_HANDLE_VALUE) break;
        Sleep(20);
    }
    if (snap == INVALID_HANDLE_VALUE) { Trace(L"module snapshot failed"); return; }
    {
        MODULEENTRY32W e; e.dwSize = sizeof(e);
        if (Module32FirstW(snap, &e))
        {
            do
            {
                WCHAR path[520], msg[900];
                if (ModulesHideSystem && IsSystemPath(e.szExePath)) continue;
                if (ModulesShowPath)
                    StringCchPrintfW(msg, 900, L"present  %s @0x%llX  %s", e.szModule, (unsigned long long)(ULONG_PTR)e.modBaseAddr, e.szExePath);
                else
                    StringCchPrintfW(msg, 900, L"present  %s @0x%llX", e.szModule, (unsigned long long)(ULONG_PTR)e.modBaseAddr);
                (void)path;
                Write(L"[MOD]", msg);
                count++;
            } while (Module32NextW(snap, &e));
        }
    }
    CloseHandle(snap);
    { WCHAR m[80]; StringCchPrintfW(m, 80, L"listed %d present modules", count); Trace(m); }
}

static void InstallModuleLog(void)
{
    if (ModulesLoaded || ModulesUnloaded) InstallModNotify();
    if (!ModulesPresent) return;
    ListPresentModules();
}

// ---------------------------------------------------------------- ring drain
static GCEv* g_evBuf = NULL;
static const int g_evBufN = 64;
static int _otherCount = 0;

static void DrainVehEvents(void)
{
    int n, i;
    if (!g_ringInit || !g_evBuf) return;
    n = 0;
    while (n < g_evBufN && EvPop(&g_evBuf[n])) n++;
    for (i = 0; i < n; i++)
    {
        GCEv* e = &g_evBuf[i];
        if (e->kind <= 1)
        {
            WCHAR s[1100], tag[600];
            if (!CaptureOds || !ShowOds) continue;
            if (e->kind == 1) { StringCchCopyW(s, 1100, e->text); }
            else { if (MultiByteToWideChar(CP_ACP, 0, (const char*)e->text, -1, s, 1100) <= 0) continue; }
            if (!s[0]) continue;
            if (OdsShowCaller) { WCHAR c[560]; FirstCaller(e->frames, c, 560); StringCchPrintfW(tag, 600, L"[ODS %s]", c); }
            else StringCchCopyW(tag, 600, L"[ODS]");
            Write(tag, s);
        }
        else if (e->kind == 2)
        {
            int wanted;
            const WCHAR* kindName;
            WCHAR tag[600], text[600], c[560], kindBuf[80];
            if (IsCrashCode((unsigned)e->code)) { if (!ExcCrashCodes) continue; }
            else
            {
                unsigned code = (unsigned)e->code;
                wanted = code == 0xE06D7363u ? ExcCpp :
                         code == 0xE0434352u ? ExcClr :
                         (code == 0x80000003u || code == 0x80000004u) ? ExcBreakpoints : ExcOther;
                if (!wanted || !ShowExceptions) continue;
            }
            if (InterlockedIncrement((volatile LONG*)&_otherCount) > MaxOtherExceptions) continue;
            switch ((unsigned)e->code)
            {
            case 0xE06D7363u: kindName = L"C++ throw"; break;
            case 0xE0434352u: kindName = L"CLR exception"; break;
            case 0x80000003u: kindName = L"breakpoint"; break;
            case 0x80000004u: kindName = L"single step"; break;
            default:
                /* C# uses ExcName(...).ToLowerInvariant() for the [EXC] kind */
                StringCchCopyW(kindBuf, 80, ExcName((unsigned)e->code));
                StringCchLowerW(kindBuf);
                kindName = kindBuf;
                break;
            }
            FirstCaller(e->frames, c, 560);
            StringCchPrintfW(tag, 600, L"[EXC %s]", c);
            if ((unsigned)e->code == 0x80000004u || (unsigned)e->code == 0x80000003u)
            {
                // which breakpoint fired: Dr6 bit 14 = single-step, bits 0-3 name
                // a Dr0-3 match; report the trap Rip and the armed registers
                unsigned which = (unsigned)(e->dr[4] & 0xF);
                StringCchPrintfW(text, 600, L"0x%08X %s rip=0x%llX which=%s Dr6=%llX Dr0=%llX Dr1=%llX Dr2=%llX Dr3=%llX Dr7=%llX",
                                 (unsigned)e->code, kindName, (unsigned long long)e->addr,
                                 which ? (which == 1 ? L"Dr0" : which == 2 ? L"Dr1" : which == 4 ? L"Dr2" : which == 8 ? L"Dr3" : L"?") : L"none(step)",
                                 (unsigned long long)e->dr[4], (unsigned long long)e->dr[0],
                                 (unsigned long long)e->dr[1], (unsigned long long)e->dr[2],
                                 (unsigned long long)e->dr[3], (unsigned long long)e->dr[5]);
            }
            else
                StringCchPrintfW(text, 600, L"0x%08X %s", (unsigned)e->code, kindName);
            Write(tag, text);
        }
        else if (e->kind == 4 || e->kind == 5)
        {
            int load = (e->kind == 4);
            WCHAR msg[900], name[300]; SYSTEMTIME t;
            if (load ? !ModulesLoaded : !ModulesUnloaded) continue;
            if (ModulesHideSystem && IsSystemPath(e->text)) continue;
            {
                const WCHAR* slash = wcsrchr(e->text, L'\\');
                const WCHAR* slash2 = wcsrchr(e->text, L'/');
                const WCHAR* base = e->text;
                if (slash && slash + 1 > base) base = slash + 1;
                if (slash2 && slash2 + 1 > base) base = slash2 + 1;
                StringCchCopyW(name, 300, base);
            }
            if (!name[0]) StringCchCopyW(name, 300, L"?");
            GetLocalTime(&t);
            StringCchPrintfW(msg, 900, L"%s %s @0x%llX (%02u:%02u:%02u.%03u)",
                             load ? L"loaded  " : L"unloaded", name, (unsigned long long)e->addr,
                             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
            Write(L"[MOD]", msg);
        }
        else if (e->kind == 6)
        {
            if (!ShowHooks) continue;
            Write(L"[HOOK]", e->text);
            CsvRow(L"hook", L"", L"", e->text);
        }
        else
        {
            /* The native UEH already wrote this report synchronously (and to
               the crash file); only print it here if it could not. */
            if (!g_crashSynced)
            {
                WCHAR report[2048];
                FormatCrash(e, report, 2048);
                Write(L"[CRASH]", report);
                CrashFileAppend(report);
                Write(L"[CRASH]", HoldOnCrash ? L"process is about to terminate (held open, close this window or kill the game)"
                                              : L"process is about to terminate");
            }
        }
    }
}

// ---------------------------------------------------------------- monitors
static int g_monInit = 0;
static int g_hooksInstalled = 0;            // set when the (possibly deferred) IAT writes went in
static ULONGLONG g_hooksAt = 0;             // GetTickCount64 deadline for HookDelayMs
static void InstallHooks(void);

static void LogStartupInfo(void)
{
    WCHAR exe[520], cwd[520], msg[1300];
    typedef struct { ULONG sz; ULONG major, minor, build, platform; WCHAR csd[128]; } RTL_OSVI;
    typedef LONG (WINAPI* t_RtlGetVersion)(RTL_OSVI*);
    RTL_OSVI vi;
    t_RtlGetVersion rgv = (t_RtlGetVersion)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    GetModuleFileNameW(NULL, exe, 520);
    GetCurrentDirectoryW(520, cwd);
    Write(L"[INFO]", (StringCchPrintfW(msg, 1300, L"exe: %s", exe), msg));
    Write(L"[INFO]", (StringCchPrintfW(msg, 1300, L"cmdline: %s", GetCommandLineW()), msg));
    Write(L"[INFO]", (StringCchPrintfW(msg, 1300, L"cwd: %s", cwd), msg));
    vi.sz = sizeof(vi); vi.major = vi.minor = vi.build = vi.platform = 0; vi.csd[0] = 0;
    if (rgv) rgv(&vi);
    StringCchPrintfW(msg, 1300, L"os: Microsoft Windows NT %lu.%lu.%lu.0, 64-bit process: %s",
                     vi.major, vi.minor, vi.build, sizeof(void*) == 8 ? L"true" : L"false");
    Write(L"[INFO]", msg);
}

// ---- threads ----
#define MAX_THREADS 4096
static DWORD g_threads[MAX_THREADS]; static int g_nThreads = 0;
static int g_threadsBaseline = 0;

static void StartOf(DWORD tid, WCHAR* out, int cch)
{
    typedef LONG (NTAPI* t_NtQIT)(HANDLE, int, void*, ULONG, ULONG*);
    static t_NtQIT qit = NULL;
    HANDLE h;
    if (!qit) qit = (t_NtQIT)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    h = OpenThread(0x0040, FALSE, tid);
    if (!h) h = OpenThread(0x0800, FALSE, tid);
    if (!h) { StringCchCopyW(out, cch, L"?"); return; }
    {
        ULONG_PTR addr = 0;
        LONG st = qit ? qit(h, 9, &addr, (ULONG)sizeof(addr), NULL) : -1;
        if (st == 0) Describe((void*)addr, out, cch); else StringCchCopyW(out, cch, L"?");
    }
    CloseHandle(h);
}

static void PollThreads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    DWORD now[MAX_THREADS]; int nNow = 0, i;
    if (snap == INVALID_HANDLE_VALUE) return;
    {
        THREADENTRY32 e; e.dwSize = sizeof(e);
        if (Thread32First(snap, &e))
        {
            do
            {
                if (e.th32OwnerProcessID == g_pid && nNow < MAX_THREADS) now[nNow++] = e.th32ThreadID;
                e.dwSize = sizeof(e);
            } while (Thread32Next(snap, &e));
        }
    }
    CloseHandle(snap);

    for (i = 0; i < nNow; i++)
    {
        int j, found = 0;
        for (j = 0; j < g_nThreads; j++) if (g_threads[j] == now[i]) { found = 1; break; }
        if (!found)
        {
            WCHAR st[560], msg[700];
            if (g_nThreads < MAX_THREADS) g_threads[g_nThreads++] = now[i];
            if (g_threadsBaseline ? ThreadsStarted : ThreadsPresent)
            {
                StartOf(now[i], st, 560);
                // StartOf prints "module+off" for loader images and a bare
                // "0x…" when the start address belongs to no image - i.e. code
                // in a manually-mapped region (Denuvo's own worker threads).
                StringCchPrintfW(msg, 700, L"%s tid %lu start=%s%s", g_threadsBaseline ? L"+" : L"present", now[i], st,
                                 (st[0] == L'0' && st[1] == L'x') ? L"  [non-image]" : L"");
                Write(L"[THR]", msg);
            }
        }
    }
    for (i = 0; i < g_nThreads; )
    {
        int j, found = 0;
        for (j = 0; j < nNow; j++) if (now[j] == g_threads[i]) { found = 1; break; }
        if (!found)
        {
            DWORD tid = g_threads[i];
            if (ThreadsExited) { WCHAR msg[200]; StringCchPrintfW(msg, 200, L"- tid %lu exited", tid); Write(L"[THR]", msg); }
            g_threads[i] = g_threads[--g_nThreads];
        }
        else i++;
    }
    g_threadsBaseline = 1;
}

// ---- executable memory ----
#define MAX_EXEC 8192
static ULONG_PTR g_exec[MAX_EXEC]; static int g_nExec = 0;
static int g_memBaseline = 0;

static int ExecContains(const ULONG_PTR* arr, int n, ULONG_PTR v)
{
    int i;
    for (i = 0; i < n; i++) if (arr[i] == v) return 1;
    return 0;
}

static void PollMemory(void)
{
    ULONG_PTR newlist[MAX_EXEC]; int nnew = 0;
    ULONG_PTR addr = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < 0x7FFFFFFF0000ULL &&
           VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) != 0)
    {
        ULONG_PTR b = (ULONG_PTR)mbi.BaseAddress;
        if (mbi.State == MEM_COMMIT && mbi.Type != MEM_IMAGE && (mbi.Protect & 0xF0) != 0)
        {
            int wasNew = !ExecContains(g_exec, g_nExec, b);
            if (nnew < MAX_EXEC) newlist[nnew++] = b;
            if (wasNew && g_memBaseline)
            {
                WCHAR msg[300];
                StringCchPrintfW(msg, 300, L"new executable non-image memory @0x%llX size 0x%llX protect 0x%X",
                                 (unsigned long long)b, (unsigned long long)mbi.RegionSize, (unsigned)mbi.Protect);
                Write(L"[MEM]", msg);
                if (ScanShadow) DecodeExecPrologue(b);
            }
        }
        addr = b + (ULONG_PTR)mbi.RegionSize;
        if (mbi.RegionSize == 0) break;
    }
    memcpy(g_exec, newlist, sizeof(ULONG_PTR) * (size_t)nnew);
    g_nExec = nnew;
    if (!g_memBaseline)
    {
        g_memBaseline = 1;
        if (MemBaseline)
        {
            int i2;
            WCHAR msg[300];
            // Baseline census: list every region, not just the count. The
            // Denuvo private code lives here - committed, executable, and
            // belonging to no image in the module list.
            for (i2 = 0; i2 < g_nExec; i2++)
            {
                MEMORY_BASIC_INFORMATION m2;
                if (VirtualQuery((LPCVOID)g_exec[i2], &m2, sizeof(m2)) != 0)
                {
                    StringCchPrintfW(msg, 300, L"exec non-image region @0x%llX size 0x%llX protect 0x%X type 0x%X",
                                     (unsigned long long)g_exec[i2],
                                     (unsigned long long)m2.RegionSize,
                                     (unsigned)m2.Protect, (unsigned)m2.Type);
                    Write(L"[MEM]", msg);
                }
            }
            StringCchPrintfW(msg, 120, L"%d executable non-image regions at start", g_nExec);
            Write(L"[MEM]", msg);
        }
    }
}

// ---- network ----
#define MAX_CONN 2048
typedef struct { char key[80]; DWORD state; } Conn;
static Conn g_conns[MAX_CONN]; static int g_nConns = 0;

typedef struct { DWORD dwState, dwLocalAddr, dwLocalPort, dwRemoteAddr, dwRemotePort, dwOwningPid; } TCPROW;
typedef DWORD (WINAPI* t_GetExtTcp)(PVOID, PDWORD, BOOL, ULONG, int, ULONG);

static void IpTo(DWORD a, char* out)
{
    sprintf_s(out, 20, "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, (a >> 24) & 0xFF);
}
static DWORD PortN(DWORD p) { return ((p & 0xFF) << 8) | ((p >> 8) & 0xFF); }
static const char* TcpState(DWORD s)
{
    switch (s)
    {
    case 1: return "CLOSED"; case 2: return "LISTEN"; case 3: return "SYN_SENT"; case 4: return "SYN_RCVD";
    case 5: return "ESTABLISHED"; case 6: return "FIN_WAIT1"; case 7: return "FIN_WAIT2"; case 8: return "CLOSE_WAIT";
    case 9: return "CLOSING"; case 10: return "LAST_ACK"; case 11: return "TIME_WAIT"; case 12: return "DELETE_TCB";
    default: return "?";
    }
}

static void PollNetwork(void)
{
    static t_GetExtTcp fn = NULL;
    DWORD size = 0, i, count;
    BYTE* buf;
    Conn now[MAX_CONN]; int nNow = 0;
    if (!fn)
    {
        HMODULE ip = LoadLibraryW(L"iphlpapi.dll");
        if (!ip) return;
        fn = (t_GetExtTcp)(void*)GetProcAddress(ip, "GetExtendedTcpTable");
        if (!fn) return;
    }
    fn(NULL, &size, FALSE, 2 /*AF_INET*/, 5 /*OWNER_PID_ALL*/, 0);
    if (size == 0) return;
    buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, size);
    if (!buf) return;
    if (fn(buf, &size, FALSE, 2, 5, 0) != 0) { HeapFree(GetProcessHeap(), 0, buf); return; }
    count = *(DWORD*)buf;
    {
        TCPROW* row = (TCPROW*)(buf + 4);
        for (i = 0; i < count && nNow < MAX_CONN; i++, row++)
        {
            char ip1[20], ip2[20];
            if (row->dwOwningPid != g_pid) continue;
            IpTo(row->dwLocalAddr, ip1);
            IpTo(row->dwRemoteAddr, ip2);
            sprintf_s(now[nNow].key, 80, "%s:%u -> %s:%u", ip1, (unsigned)PortN(row->dwLocalPort), ip2, (unsigned)PortN(row->dwRemotePort));
            now[nNow].state = row->dwState;
            nNow++;
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);

    for (i = 0; i < (DWORD)nNow; i++)
    {
        int j, found = 0;
        for (j = 0; j < g_nConns; j++) if (strcmp(g_conns[j].key, now[i].key) == 0) { found = 1; break; }
        if (!found)
        {
            if (NetNew) { WCHAR msg[300]; StringCchPrintfW(msg, 300, L"+ %S %S", now[i].key, TcpState(now[i].state)); Write(L"[NET]", msg); }
        }
        else if (g_conns[j].state != now[i].state)
        {
            if (NetChanges) { WCHAR msg[400]; StringCchPrintfW(msg, 400, L"~ %S %S -> %S", now[i].key, TcpState(g_conns[j].state), TcpState(now[i].state)); Write(L"[NET]", msg); }
        }
    }
    for (i = 0; i < (DWORD)g_nConns; i++)
    {
        int j, found = 0;
        for (j = 0; j < nNow; j++) if (strcmp(g_conns[i].key, now[j].key) == 0) { found = 1; break; }
        if (!found && NetClosed) { WCHAR msg[300]; StringCchPrintfW(msg, 300, L"- %S gone", g_conns[i].key); Write(L"[NET]", msg); }
    }
    memcpy(g_conns, now, sizeof(Conn) * (size_t)nNow);
    g_nConns = nNow;
}

// ---- filesystem watchers ----
#define MAX_WATCH 4
typedef struct { WCHAR dir[520]; int recurse; HANDLE h; } WatchCtx;
static WatchCtx g_watch[MAX_WATCH]; static int g_nWatch = 0;

static int Mine(const WCHAR* name)
{
    const WCHAR* b = name;
    const WCHAR* s = wcsrchr(name, L'\\');
    if (s) b = s + 1;
    return _wcsnicmp(b, L"GameConsole", 11) == 0;
}

static DWORD WINAPI WatchThread(LPVOID param)
{
    WatchCtx* w = (WatchCtx*)param;
    for (;;)
    {
        char buf[65536];
        DWORD ret = 0;
        FILE_NOTIFY_INFORMATION* fni;
        BYTE* p;
        WCHAR pendingOld[520]; pendingOld[0] = 0;
        if (!ReadDirectoryChangesW(w->h, buf, sizeof(buf), w->recurse,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME, &ret, NULL, NULL)) break;
        if (ret == 0) continue;
        p = (BYTE*)buf;
        for (;;)
        {
            WCHAR name[520], full[1200];
            int chars;
            fni = (FILE_NOTIFY_INFORMATION*)p;
            chars = (int)(fni->FileNameLength / sizeof(WCHAR));
            if (chars > 519) chars = 519;
            memcpy(name, fni->FileName, chars * sizeof(WCHAR));
            name[chars] = 0;
            StringCchPrintfW(full, 1200, L"%s\\%s", w->dir, name);
            switch (fni->Action)
            {
            case FILE_ACTION_ADDED:
                if (FsCreated && !Mine(name))
                {
                    WCHAR msg[1300];
                    int sl = (int)wcslen(name);
                    StringCchPrintfW(msg, 1300, L"created %s", full);
                    Write(L"[FS]", msg);
                    // a .sys written while the game runs is a kernel driver image
                    // being staged for load - called out separately because it is
                    // the step just before a driver service is registered
                    if (sl > 4 && _wcsicmp(name + sl - 4, L".sys") == 0)
                    {
                        StringCchPrintfW(msg, 1300, L"driver image written to disk: %s - a .sys file created at runtime, the step before a driver service is registered", full);
                        Write(L"[KRNL]", msg);
                        CsvRow(L"krnl-image", name, msg, L"");
                    }
                }
                break;
            case FILE_ACTION_REMOVED:
                if (FsDeleted && !Mine(name)) { WCHAR msg[1300]; StringCchPrintfW(msg, 1300, L"deleted %s", full); Write(L"[FS]", msg); }
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                StringCchCopyW(pendingOld, 520, full);
                break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                if (FsRenamed && !Mine(name))
                {
                    WCHAR msg[1800];
                    StringCchPrintfW(msg, 1800, L"renamed %s -> %s", pendingOld[0] ? pendingOld : L"?", full);
                    Write(L"[FS]", msg);
                }
                pendingOld[0] = 0;
                break;
            default:
                break;
            }
            if (fni->NextEntryOffset == 0) break;
            p += fni->NextEntryOffset;
        }
    }
    return 0;
}

static void Watch(const WCHAR* dir, int recurse)
{
    WatchCtx* w;
    if (g_nWatch >= MAX_WATCH) return;
    w = &g_watch[g_nWatch];
    StringCchCopyW(w->dir, 520, dir);
    w->recurse = recurse;
    w->h = CreateFileW(dir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (w->h == INVALID_HANDLE_VALUE) { Trace(L"watch failed"); return; }
    g_nWatch++;
    {
        HANDLE t = CreateThread(NULL, 0, WatchThread, w, 0, NULL);
        if (t) CloseHandle(t);
    }
    {
        WCHAR msg[600];
        StringCchPrintfW(msg, 600, L"watching %s%s", dir, recurse ? L" (recursive)" : L"");
        Write(L"[INFO]", msg);
    }
}

static void InstallFileWatch(void)
{
    WCHAR exe[520], dir[520];
    GetModuleFileNameW(NULL, exe, 520);
    StringCchCopyW(dir, 520, exe);
    { WCHAR* s = wcsrchr(dir, L'\\'); if (s) *s = 0; }
    if (MonFiles) Watch(dir, 1);
    if (MonTempDir) { WCHAR tmp[520]; GetTempPathW(520, tmp); Watch(tmp, 0); }
}

// ---- heartbeat ----
static DWORD CountOwnThreads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    DWORD n = 0;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    {
        THREADENTRY32 e; e.dwSize = sizeof(e);
        if (Thread32First(snap, &e))
        {
            do { if (e.th32OwnerProcessID == g_pid) n++; e.dwSize = sizeof(e); } while (Thread32Next(snap, &e));
        }
    }
    CloseHandle(snap);
    return n;
}

static void Heartbeat(void)
{
    DWORD handles = 0;
    PROCESS_MEMORY_COUNTERS pmc;
    WCHAR msg[300];
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    StringCchPrintfW(msg, 300, L"threads=%lu handles=%lu workingset=%lluMB",
                     CountOwnThreads(), handles, (unsigned long long)(pmc.WorkingSetSize / 1048576));
    Write(L"[HB]", msg);
}

static void SafeCall(void (*fn)(void))
{
    __try { fn(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Trace(L"monitor error"); }
}

static void ScanRescan(void);           // defined with the hook-scan section below
static void WatchSharedDataTick(void);  // defined with the mailbox/shadow section
static void ScanGuardPages(void);
static void ShadowSurvey(int first);
static void LdrSurvey(int first);
static void VehChainSurvey(int first);
static void CpuTopologyInfo(void);
static void CodeIntegrityInfo(void);
static void DriversScan(int first);
static void ServicesScan(int first);

static void RunMonitors(long long n)
{
    if (HookApis && !g_hooksInstalled && g_hooksAt && GetTickCount64() >= g_hooksAt)
    {
        g_hooksInstalled = 1;
        g_hooksAt = 0;
        InstallHooks();                       // worker-side, nothing else running
        Trace(L"hooks installed (deferred)");
        Write(L"[INFO]", L"hooks: IAT writes are in now - the protection's own startup is behind us");
    }
    if (!g_monInit)
    {
        g_monInit = 1;
        if (ShowStartupInfo) SafeCall(LogStartupInfo);
        if (MonFiles || MonTempDir) SafeCall(InstallFileWatch);
        if (MemGuardScan) SafeCall(ScanGuardPages);
        if (WatchSharedData) SafeCall(WatchSharedDataTick);
        if (ScanDrivers) DriversScan(1);          // first: the hypervisor line
        if (ScanDrivers) CodeIntegrityInfo();     // ...and the DSE state beside it
        if (ScanEh) { CpuTopologyInfo(); VehChainSurvey(1); }   // cross-references it
        if (ScanServices) ServicesScan(1);
    }
    if (MonThreads && n % 2 == 0) SafeCall(PollThreads);
    // dse.dll switches driver signature enforcement off only for as long as it
    // takes to start the hypervisor (seconds at most, and it happens right at
    // startup when the driver is not yet loaded). On the slow cadence a short
    // off/on pair can fall entirely between two samples, so for the first minute
    // the DSE state is sampled every other tick. Still one read-only query, and
    // CodeIntegrityInfo() only writes a line when the value actually changes.
    if (ScanDrivers && n <= 240 && n % 2 == 0) SafeCall(CodeIntegrityInfo);
    if (MonNetwork && n % 8 == 0) SafeCall(PollNetwork);
    if (MonMemory && n % 20 == 0) SafeCall(PollMemory);
    if (CensusAll && g_pid && n % 20 == 0) SafeCall(CensusAllModules);
    if (GpaThunks && n % 20 == 4) SafeCall(GpaThunkTick);
    if (MonHeartbeat && HeartbeatSeconds > 0 && n % (HeartbeatSeconds * 4) == 0) SafeCall(Heartbeat);
    if (ScanOtherHooks && HeartbeatSeconds > 0 && n % (HeartbeatSeconds * 4) == 0) SafeCall(ScanRescan);
    if (HeartbeatSeconds > 0 && n % (HeartbeatSeconds * 4) == 2)
    {
        if (ScanDrivers) { DriversScan(0); CodeIntegrityInfo(); }
        if (ScanEh) { CpuTopologyInfo(); VehChainSurvey(0); }  // re-probe + chain
        if (ScanServices) ServicesScan(0);
    }
    if (MemGuardScan && n % 20 == 0) SafeCall(ScanGuardPages);
    if (WatchSharedData && n % 12 == 0) SafeCall(WatchSharedDataTick);
    if (n % 4 == 1)
    {
        // ~1 s: pick up mod DLLs that load after us, and resolve any caller the
        // hook path could not name (a hidden module like dse.dll)
        HookLateModules();
        ResolveCallerAddrs();
    }
    if (n % 40 == 7) RefreshCallerRanges();  // keep caller naming current
}

// ---------------------------------------------------------------- log tailing
#define MAX_CAND 512
static WCHAR g_cand[MAX_CAND][520]; static int g_nCand = 0;

static int EndsWithI(const WCHAR* s, const WCHAR* suf)
{
    size_t ls = wcslen(s), lf = wcslen(suf);
    if (ls < lf) return 0;
    return _wcsicmp(s + ls - lf, suf) == 0;
}
static int ContainsI(const WCHAR* s, const WCHAR* sub)
{
    for (; *s; s++)
    {
        const WCHAR* a = s; const WCHAR* b = sub;
        while (*a && *b && towlower(*a) == towlower(*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}
static void AddCand(const WCHAR* path)
{
    if (g_nCand < MAX_CAND) StringCchCopyW(g_cand[g_nCand++], 520, path);
}

static void ScanDir(const WCHAR* dir, int depth)
{
    WCHAR pat[600];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    StringCchPrintfW(pat, 600, L"%s\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        WCHAR full[1200];
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        StringCchPrintfW(full, 1200, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (depth > 0) ScanDir(full, depth - 1);
        }
        else
        {
            int islog = EndsWithI(fd.cFileName, L".log");
            int istxt = TailTxtLogs && EndsWithI(fd.cFileName, L".txt") && ContainsI(fd.cFileName, L"log");
            if (islog || istxt) AddCand(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void FindLogs(const WCHAR* gameDir)
{
    g_nCand = 0;
    ScanDir(gameDir, LogScanDepth);
    if (IncludeUnityPlayerLog)
    {
        WCHAR low[600], pat[620];
        if (GetEnvironmentVariableW(L"USERPROFILE", low, 600))
        {
            StringCchCatW(low, 600, L"\\AppData\\LocalLow");
            StringCchPrintfW(pat, 620, L"%s\\*", low);
            {
                WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd);
                if (h != INVALID_HANDLE_VALUE)
                {
                    do
                    {
                        WCHAR company[900];
                        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                        StringCchPrintfW(company, 900, L"%s\\%s\\*", low, fd.cFileName);
                        {
                            WIN32_FIND_DATAW fd2; HANDLE h2 = FindFirstFileW(company, &fd2);
                            if (h2 != INVALID_HANDLE_VALUE)
                            {
                                do
                                {
                                    WCHAR plog[1200];
                                    if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                                    if (wcscmp(fd2.cFileName, L".") == 0 || wcscmp(fd2.cFileName, L"..") == 0) continue;
                                    StringCchPrintfW(plog, 1200, L"%s\\%s\\%s\\Player.log", low, fd.cFileName, fd2.cFileName);
                                    if (GetFileAttributesW(plog) != INVALID_FILE_ATTRIBUTES) AddCand(plog);
                                } while (FindNextFileW(h2, &fd2));
                                FindClose(h2);
                            }
                        }
                    } while (FindNextFileW(h, &fd));
                    FindClose(h);
                }
            }
        }
    }
}

#define MAX_TAILS 256
typedef struct { WCHAR path[520]; WCHAR name[260]; long long pos; } Tail;
static Tail g_tails[MAX_TAILS]; static int g_nTails = 0;

static Tail* TailFind(const WCHAR* path)
{
    int i;
    for (i = 0; i < g_nTails; i++) if (_wcsicmp(g_tails[i].path, path) == 0) return &g_tails[i];
    return NULL;
}

static void TailFile(const WCHAR* path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    long long size;
    Tail* t;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return;
    size = ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;

    t = TailFind(path);
    if (!t)
    {
        WCHAR msg[900];
        /* Only follow files touched since we started (-30s). */
        LARGE_INTEGER lt;
        lt.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        lt.LowPart = fad.ftLastWriteTime.dwLowDateTime;
        if (lt.QuadPart < ((LARGE_INTEGER*)&g_startTime)->QuadPart - 300000000LL) return;
        if (g_nTails >= MAX_TAILS) return;
        t = &g_tails[g_nTails++];
        StringCchCopyW(t->path, 520, path);
        {
            const WCHAR* b = wcsrchr(path, L'\\');
            StringCchCopyW(t->name, 260, b ? b + 1 : path);
        }
        t->pos = TailSkipExisting ? size : 0;
        StringCchPrintfW(msg, 900, L"tailing %s", path);
        Write(L"[console]", msg);
    }
    if (size < t->pos) t->pos = 0;
    if (size == t->pos) return;

    {
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        char buf[65536];
        if (h == INVALID_HANDLE_VALUE) return;
        {
            LARGE_INTEGER li; DWORD got = 0; char* p; char* end;
            li.QuadPart = t->pos;
            SetFilePointerEx(h, li, NULL, FILE_BEGIN);
            if (!ReadFile(h, buf, sizeof(buf) - 1, &got, NULL)) got = 0;
            t->pos += got;
            buf[got] = 0;
            p = buf; end = buf + got;
            while (p < end)
            {
                char* nl = memchr(p, '\n', (size_t)(end - p));
                size_t seg = nl ? (size_t)(nl - p) : (size_t)(end - p);
                if (seg > 0 && p[seg - 1] == '\r') seg--;
                if (seg > 0)
                {
                    WCHAR wline[1400], tag[300]; int n; DWORD w;
                    char tmp[1400];
                    if (seg > 1399) seg = 1399;
                    memcpy(tmp, p, seg); tmp[seg] = 0;
                    if (MultiByteToWideChar(CP_UTF8, 0, tmp, -1, wline, 1400) > 0)
                    {
                        StringCchPrintfW(tag, 300, L"[%s]", t->name);
                        Write(tag, wline);
                    }
                    (void)n; (void)w;
                }
                if (!nl) break;
                p = nl + 1;
            }
        }
        CloseHandle(h);
    }
}

static void TailLoop(void)
{
    WCHAR exe[520], gameDir[520];
    long long loopN = 0;
    int tick = 0;
    GetModuleFileNameW(NULL, exe, 520);
    StringCchCopyW(gameDir, 520, exe);
    { WCHAR* s = wcsrchr(gameDir, L'\\'); if (s) *s = 0; }

    for (;;)
    {
        int i;
        DrainVehEvents();
        RunMonitors(++loopN);
        SafeCall(ScrapeConsole);
        if (TailLogFiles && ShowLogFiles && tick++ % 10 == 0) FindLogs(gameDir);
        for (i = 0; i < g_nCand; i++)
        {
            const WCHAR* b = wcsrchr(g_cand[i], L'\\');
            b = b ? b + 1 : g_cand[i];
            if (_wcsnicmp(b, L"GameConsole", 11) == 0) continue;
            TailFile(g_cand[i]);
        }
        Sleep(250);
    }
}

// ---------------------------------------------------------------- hooks install
static void InstallVehCapture(void)
{
    // ScanEh anchors the chain walk on GameConsole's own registration, so with
    // it on the handler is installed even when every logging flag is off (it
    // then just returns EXCEPTION_CONTINUE_SEARCH, like not being there).
    int veh = CaptureOds || LogAllExceptions || ExcCrashCodes || ScanEh;
    if (!veh && !EnableCrashHandler) return;
    if (!pStack) pStack = (t_CaptureStack)(void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "RtlCaptureStackBackTrace");
    g_ods = (veh && CaptureOds) ? 1 : 0;
    g_hold = HoldOnCrash ? 1 : 0;
    if (veh) { g_vehCookie = AddVectoredExceptionHandler(1, VehNative); }
    if (EnableCrashHandler) SetUnhandledExceptionFilter(UehNative);
    g_vehOn = 1;
}

// Exact token test against a comma/space separated module list.
static int ModListHas(const WCHAR* list, const WCHAR* name)
{
    size_t nl = wcslen(name);
    const WCHAR* p = list;
    while (p && *p)
    {
        const WCHAR* e;
        int cl;
        while (*p == L' ' || *p == L',') p++;
        if (!*p) break;
        e = wcschr(p, L',');
        cl = e ? (int)(e - p) : (int)wcslen(p);
        while (cl > 0 && p[cl - 1] == L' ') cl--;
        if ((size_t)cl == nl && _wcsnicmp(p, name, nl) == 0) return 1;
        p = e ? e + 1 : p + wcslen(p);
    }
    return 0;
}

static void InstallHooks(void)
{
    WCHAR mods[700];
    WCHAR exeName[300] = L"";
    int addedExe = 0;
    StringCchCopyW(mods, 700, g_hookModules);
    if (HookExe)
    {
        WCHAR exe[520];
        const WCHAR* b;
        DWORD len = GetModuleFileNameW(NULL, exe, 520);
        b = len ? wcsrchr(exe, L'\\') : NULL;
        b = (b && b[1]) ? b + 1 : exe;
        if (len && b[0] && !ModListHas(mods, b))
        {
            StringCchCopyW(exeName, 300, b);
            if (mods[0]) StringCchCatW(mods, 700, L",");
            StringCchCatW(mods, 700, b);
            addedExe = 1;
        }
    }
    if (mods[0])
    {
        WCHAR msg[1100];
        InstallIatHooks(HookFileReads ? 1 : 0, HookFileWrites ? 1 : 0, HookMemPatches ? 1 : 0,
                        MaxHookReports, mods);
        StringCchPrintfW(msg, 1100,
            L"hooks: IAT capture on [%s]%s + KERNELBASE interposition layer (reads=%d writes=%d prot=%d) - activity appears as [HOOK] lines",
            mods, addedExe ? L" (this executable added by HookExe)" : L"",
            HookFileReads, HookFileWrites, HookMemPatches);
        Write(L"[INFO]", msg);
    }
    else if (!HookLegacyNtdll)
    {
        InstallIatHooks(HookFileReads ? 1 : 0, HookFileWrites ? 1 : 0, HookMemPatches ? 1 : 0,
                        MaxHookReports, mods);
        Write(L"[INFO]", L"hooks: no module list to instrument - KERNELBASE interposition layer installed for ALL callers (reads/writes/prot apply); set HookModules=<dll> or HookExe=true to also instrument an import table");
    }
    else
    {
        WCHAR msg[1100];
        InstallIatHooks(HookFileReads ? 1 : 0, HookFileWrites ? 1 : 0, HookMemPatches ? 1 : 0,
                        MaxHookReports, mods);
        InstallLegacyHooks(HookFileReads ? 1 : 0, HookFileWrites ? 1 : 0, HookMemPatches ? 1 : 0,
                           HookOnlyGameModules ? 1 : 0, MaxHookReports, mods);
        StringCchPrintfW(msg, 1100,
            L"hooks: KERNELBASE interposition + legacy ntdll stub patching installed%s (reads=%d writes=%d prot=%d%s) - activity appears as [HOOK] lines",
            mods[0] ? L" on the module list" : L"",
            HookFileReads, HookFileWrites, HookMemPatches,
            HookOnlyGameModules ? L", game modules only" : L"");
        Write(L"[INFO]", msg);
    }
}

// ---------------------------------------------------------------- hook scan
// Read-only survey of what code OTHER than ntdll itself has done to ntdll.
// Reports EVERY export whose entry has been REPLACED by a jump (instead of a
// clean syscall stub), whatever it points at, classified:
//     ntdll!NtCreateFile -> artifact.dll+0xF789     (hooked: FF25 abs jmp)
//     ntdll!NtQueryTimerResolution -> ntdll!ZwQueryTimerResolution (alias)
//     ntdll!NtXxx -> ntdll+0x1468F0  (NON-EXPORT target - cave/patched body)
// A same-image non-export target is exactly what a code-cave hook inside
// ntdll looks like, so it is reported, not filtered. Nothing is written
// anywhere: only reads of already-mapped executable memory, safe from any
// thread, cannot race another patcher. Runs at startup and is re-run on each
// heartbeat; rescans report only sites not seen before.
// Lock-free by design: this is called from the VEH/IAT hooks as well as from
// the worker, so it may not touch the loader lock. Hidden modules are added to
// the caller-range table by the worker once they are resolved by address.
static void DescribeModuleOf(void* addr, WCHAR* out, int cch)
{
    ULONG_PTR base = 0;
    const WCHAR* name = NULL;
    out[0] = 0;
    if (CallerRangeFor((ULONG_PTR)addr, &base, NULL, &name, NULL) && name && name[0])
        StringCchPrintfW(out, (size_t)cch, L"%s+0x%llX", name,
                         (unsigned long long)((ULONG_PTR)addr - base));
    else
        StringCchPrintfW(out, (size_t)cch, L"0x%llX", (unsigned long long)(ULONG_PTR)addr);
}

// Per-image dedup: one bit per export, so a rescan reports only sites that
// appeared since the last scan. (An earlier pointer-list design overflowed
// and re-reported the same batch every heartbeat.)
static BYTE g_scanSeen[8][8192];       // 65536 exports per image slot
static int  g_scanProxyDone[8];        // per-image summary already printed
static int  g_scanLastRedir[8];        // per-image redirect count last scan
static int  g_scanLastOut[8];          // per-image leaves-the-pair count last scan
static int  g_scanTotal;               // grand total from the previous scan
static int  g_scanProxies;             // proxy image count last scan

// Export-name lookup done RIGHT: AddressOfNameOrdinals[j] is the FUNCTION
// ordinal of name j, so naming function k means scanning for value == k.
static const char* ScanNameOfFn(IMAGE_EXPORT_DIRECTORY* exp, BYTE* base, DWORD k)
{
    DWORD j;
    if (!exp || !exp->NumberOfNames) return NULL;
    for (j = 0; j < exp->NumberOfNames; j++)
        if (((WORD*)(base + exp->AddressOfNameOrdinals))[j] == k)
            return (const char*)(base + ((DWORD*)(base + exp->AddressOfNames))[j]);
    return NULL;
}

static int ScanInRange(void* a, ScanImg* im)
{
    return a && (BYTE*)a >= im->base && (BYTE*)a < im->base + im->size;
}

// Detection shared by both passes: is export i's entry a redirect?
// kind: 1 = FF25 abs jmp (rip-relative slot followed), 2 = rel jmp.
static int ScanDetect(BYTE* base, IMAGE_EXPORT_DIRECTORY* exp, DWORD i, int* kind, void** destOut)
{
    DWORD fRva = exp->AddressOfFunctions ? ((DWORD*)(base + exp->AddressOfFunctions))[i] : 0;
    BYTE* fn;
    MEMORY_BASIC_INFORMATION mbi;
    *destOut = NULL;
    if (!fRva) return 0;
    fn = base + fRva;
    // Clean syscall stubs start 4C 8B D1 B8 (mov r10,rcx; mov eax,<ssn>).
    if (fn[0] == 0x4C && fn[1] == 0x8B && fn[2] == 0xD1 && fn[3] == 0xB8) return 0;
    if (VirtualQuery(fn, &mbi, sizeof(mbi)) == 0) return 0;
    if (!(mbi.Protect & 0xF0)) return 0;                 // not executable
    if (fn[0] == 0xFF && fn[1] == 0x25)                  // FF 25 disp32 -> abs64
    {
        INT32 d32;
        void* d = NULL;
        memcpy(&d32, fn + 2, sizeof(d32));
        __try { d = *(void**)(fn + 6 + d32); }           // pointer lives at rip+disp32
        __except (EXCEPTION_EXECUTE_HANDLER) { d = NULL; }
        if (!d) return 0;
        *kind = 1; *destOut = d; return 1;
    }
    if (fn[0] == 0xE9)                                    // E9 rel32
    {
        LONG rel;
        memcpy(&rel, fn + 1, sizeof(rel));
        *kind = 2; *destOut = fn + 5 + rel; return 1;
    }
    if (fn[0] == 0xEB)                                    // EB rel8
    {
        *kind = 2; *destOut = fn + 2 + (char)fn[1]; return 1;
    }
    return 0;
}

// Scan one ntdll image. Reports EVERY export whose entry is a redirect,
// classified. If >80% of a non-trivial image's exports are forwarders into
// ANOTHER ntdll image, that image is a private/proxy copy: it gets ONE
// summary line instead of thousands of per-export lines, and only its
// forwarders that leave the ntdll pair (apphelp, unlinked regions, ...) are
// listed - those are the anomalies. imgIdx selects the dedup bitset.
static void ScanNtdllImage(int imgIdx, BYTE* base, ScanImg* nts, int nnt, int reportMax,
                           int* pOut, int* pOther, int* pSelf, int* pProxy)
{
    IMAGE_DOS_HEADER* dos;
    IMAGE_NT_HEADERS* nt_h;
    IMAGE_EXPORT_DIRECTORY* exp;
    DWORD rva, size, i, n, imgSize = 0;
    int nRedir = 0, nSelf = 0, nOther = 0, nOut = 0, proxy = 0, reported = 0, newSite = 0;
    int first = !g_scanProxyDone[imgIdx];
    int fwdIdx = -1;
    WCHAR line[1200], who[130];

    if (!base || imgIdx < 0 || imgIdx >= 8) return;
    if (reportMax <= 0) reportMax = 64;

    __try
    {
        dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        nt_h = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt_h->Signature != IMAGE_NT_SIGNATURE) return;
        imgSize = nt_h->OptionalHeader.SizeOfImage;
        rva  = nt_h->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        size = nt_h->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
        if (!rva || !size) return;
        exp = (IMAGE_EXPORT_DIRECTORY*)(base + rva);
        n = exp->NumberOfFunctions;
        if (!n || n > 65536) return;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    // pass 1: full (dedup-free) counts so summaries stay stable across rescans
    for (i = 0; i < n; i++)
    {
        int kind; void* dest; int j;
        __try
        {
            if (!ScanDetect(base, exp, i, &kind, &dest)) continue;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (dest && (BYTE*)dest >= base && (BYTE*)dest < base + imgSize) nSelf++;
        else
        {
            int hit = 0;
            for (j = 0; j < nnt; j++)
                if (nts[j].base != base && ScanInRange(dest, &nts[j]))
                { nOther++; hit = 1; fwdIdx = j; break; }
            if (!hit) nOut++;
        }
    }
    nRedir = nSelf + nOther + nOut;
    proxy = (n > 50u && nRedir * 5 > (int)n * 4);        // >80% redirected

    // pass 2: classify + report
    for (i = 0; i < n; i++)
    {
        int kind, inside, inOther, seenBit, sByte, sMask;
        void* dest;
        const char* fname;
        const char* alias = NULL;
        DWORD dRva, k;

        __try
        {
            if (!ScanDetect(base, exp, i, &kind, &dest)) continue;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }

        sByte = (int)(i >> 3); sMask = 1 << (i & 7);
        seenBit = (g_scanSeen[imgIdx][sByte] & sMask) ? 1 : 0;
        g_scanSeen[imgIdx][sByte] |= (BYTE)sMask;
        if (seenBit) continue;                           // rescan: already reported
        newSite = 1;

        inside = dest && (BYTE*)dest >= base && (BYTE*)dest < base + imgSize;
        inOther = 0;
        if (!inside)
        {
            int j;
            for (j = 0; j < nnt; j++)
                if (nts[j].base != base && ScanInRange(dest, &nts[j])) { inOther = 1; break; }
        }

        if (proxy)
        {
            if (inside || inOther) continue;             // routine forwarder, summarized once
            if (reported >= reportMax) continue;
            reported++;
            fname = ScanNameOfFn(exp, base, i);
            if (!fname) { static char ob[32]; StringCchPrintfA(ob, 32, "#%u", (unsigned)i); fname = ob; }
            DescribeModuleOf(dest, who, 130);
            StringCchPrintfW(line, 1200, L"ntdll#%d!%hs -> %s  (proxy ntdll forwards OUTSIDE the ntdll pair: %s)",
                             imgIdx + 1, fname, who, kind == 1 ? L"FF25 abs jmp" : L"rel jmp");
            Write(L"[SCAN]", line);
            (*pOut)++;
        }
        else
        {
            if (reported >= reportMax) continue;
            reported++;
            fname = ScanNameOfFn(exp, base, i);
            if (!fname) { static char ob[32]; StringCchPrintfA(ob, 32, "#%u", (unsigned)i); fname = ob; }

            if (inside)
            {
                // Exact match against every export entry in this image: if the
                // jump lands on another export's entry it is an alias thunk;
                // if it lands on non-export code it is a cave or a patched
                // body - report it either way, never filter it.
                dRva = (DWORD)((BYTE*)dest - base);
                for (k = 0; k < exp->NumberOfFunctions; k++)
                {
                    if (dRva && ((DWORD*)(base + exp->AddressOfFunctions))[k] == dRva)
                    { alias = ScanNameOfFn(exp, base, k); break; }
                }
            }
            if (inside && alias)
            {
                StringCchPrintfW(line, 1200, L"ntdll#%d!%hs -> ntdll!%hs  (same-image alias, %s)",
                                 imgIdx + 1, fname, alias, kind == 1 ? L"FF25 jmp" : L"rel jmp");
                (*pSelf)++;
            }
            else if (inside)
            {
                StringCchPrintfW(line, 1200, L"ntdll#%d!%hs -> ntdll+0x%llX  (same-image NON-EXPORT target: cave or patched body)",
                                 imgIdx + 1, fname, (unsigned long long)((BYTE*)dest - base));
                (*pSelf)++;
            }
            else if (inOther)
            {
                DescribeModuleOf(dest, who, 130);
                StringCchPrintfW(line, 1200, L"ntdll#%d!%hs -> %s  (hooked: %s - jump into the OTHER ntdll image)",
                                 imgIdx + 1, fname, who, kind == 1 ? L"FF25 abs jmp" : L"rel jmp");
                (*pOther)++;
            }
            else
            {
                DescribeModuleOf(dest, who, 130);
                StringCchPrintfW(line, 1200, L"ntdll#%d!%hs -> %s  (hooked: %s)",
                                 imgIdx + 1, fname, who, kind == 1 ? L"FF25 abs jmp" : L"rel jmp");
                (*pOut)++;
            }
            Write(L"[SCAN]", line);
        }
    }

    if (proxy && (first || nRedir != g_scanLastRedir[imgIdx] || nOut != g_scanLastOut[imgIdx]))
    {
        StringCchPrintfW(line, 1200,
            L"ntdll#%d @0x%llX: %d/%d exports forward into ntdll#%d - private/proxy ntdll copy (not OS behavior); %d forward(s) leave the ntdll pair",
            imgIdx + 1, (unsigned long long)(ULONG_PTR)base, nRedir, (unsigned)n,
            fwdIdx >= 0 ? fwdIdx + 1 : 0, nOut);
        Write(L"[SCAN]", line);
        g_scanProxyDone[imgIdx] = 1;
        g_scanLastRedir[imgIdx] = nRedir;
        g_scanLastOut[imgIdx] = nOut;
    }
    else if (!proxy) g_scanProxyDone[imgIdx] = 0;
    *pProxy = proxy;
}

// Walks EVERY ntdll image in the process (this game has two: the real one
// and a forwarder/proxy copy). first != 0 is the startup survey; rescans
// report only sites not seen before. Read-only: never writes a byte.
static int ScanNtdllHooks(int first, int reportMax)
{
    ScanImg nts[8];
    int nnt = 0, i, nOut = 0, nOther = 0, nSelf = 0, nProxy = 0, total;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, g_pid);
    WCHAR line[1200];

    if (snap != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W me; me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me))
        {
            do
            {
                if (_wcsicmp(me.szModule, L"ntdll.dll") == 0 && nnt < 8)
                {
                    int dup = 0;
                    for (i = 0; i < nnt; i++) if (nts[i].base == (BYTE*)me.hModule) dup = 1;
                    if (!dup) { nts[nnt].base = (BYTE*)me.hModule; nts[nnt].size = me.modBaseSize; nnt++; }
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    if (!nnt)
    {
        BYTE* b = (BYTE*)GetModuleHandleW(L"ntdll.dll");
        if (!b) return 0;
        nts[0].base = b;
        nts[0].size = 0x200000;                          // generous fallback
        nnt = 1;
    }

    for (i = 0; i < nnt; i++)
    {
        int a = 0, b = 0, c = 0, px = 0;
        ScanNtdllImage(i, nts[i].base, nts, nnt, reportMax, &a, &b, &c, &px);
        if (px) { nProxy++; continue; }                  // summarized separately
        nOut += a; nOther += b; nSelf += c;
    }
    total = nOut + nOther + nSelf;
    if (first || total != g_scanTotal || nProxy != g_scanProxies)
    {
        StringCchPrintfW(line, 1200,
            L"ntdll survey across %d image(s): %d anomalous entr%s - %d hooked outside ntdll, %d into the other ntdll copy, %d same-image (alias/cave) - %d private/proxy ntdll image(s)",
            nnt, total, total == 1 ? L"y" : L"ies", nOut, nOther, nSelf, nProxy);
        Write(L"[SCAN]", line);
    }
    g_scanTotal = total;
    g_scanProxies = nProxy;
    return g_scanTotal;
}

static void ScanRescan(void) { ScanNtdllHooks(0, ScanOtherHooksMax); ShadowSurvey(0); LdrSurvey(0); }

// ---------------------------------------------------------------- PEB loader list
// Toolhelp32 walks the loader list with its own validation. A manually-mapped
// module can be invisible to it while GetModuleHandle/GetProcAddress still
// resolve it (the protection needs its own DLL findable). This walks the three
// real PEB loader lists read-only (load-order, memory-order, init-order) with
// per-list cycle detection, so a relinked/tampered list is reported instead of
// silently truncating the walk. It reports entries toolhelp32 does not show and
// entries whose memory is not MEM_IMAGE. Never writes anything.

typedef struct { USHORT Length, MaximumLength; WCHAR* Buffer; } GC_USTR;
typedef struct {
    LIST_ENTRY Load, Memory, Init;
    void* DllBase;
    void* EntryPoint;
    ULONG SizeOfImage;
    ULONG Reserved;
    GC_USTR FullName;
    GC_USTR BaseName;
} GC_LDR;

#define MAX_LDR 4096
static ULONG_PTR g_ldrSeen[MAX_LDR]; static int g_nLdrSeen = 0;
static int g_ldrSummaryDone = 0;

static ULONG_PTR g_ldrTb[1024]; static int g_nLdrTb = 0;

// Returns the number of distinct entries walked. ghost/nonimg accumulate.
// linkOff is the offset of the list links inside LDR_DATA_TABLE_ENTRY: 0x00
// (load order), 0x10 (memory order), 0x20 (initialisation order).
static int LdrWalkList(LIST_ENTRY* head, int linkOff, const WCHAR* which, int* ghost, int* nonimg)
{
    static LIST_ENTRY* vis[MAX_LDR];
    int nv = 0, total = 0, guard = 0, cycle = 0, i;
    LIST_ENTRY* cur;
    __try { cur = head->Flink; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    while (cur && cur != head && guard++ < 20000)
    {
        GC_LDR* e = (GC_LDR*)((BYTE*)cur - linkOff);
        void* base = NULL;
        ULONG size = 0;
        WCHAR name[260];
        int nl = 0, known = 0, dup = 0;
        LIST_ENTRY* next = NULL;
        MEMORY_BASIC_INFORMATION mb;
        DWORD prot = 0, type = 0;
        int isImg = 0;
        for (i = 0; i < nv; i++) if (vis[i] == cur) { cycle = 1; break; }
        if (cycle) break;
        if (nv >= MAX_LDR) break;
        vis[nv++] = cur;
        name[0] = 0;
        __try
        {
            next = cur->Flink;
            base = e->DllBase;
            size = e->SizeOfImage;
            if (e->BaseName.Buffer && e->BaseName.Length)
            {
                nl = e->BaseName.Length / 2;
                if (nl > 255) nl = 255;
                memcpy(name, e->BaseName.Buffer, (size_t)nl * sizeof(WCHAR));
                name[nl] = 0;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (!next) break;
        total++;
        if (base)
        {
            for (i = 0; i < g_nLdrTb; i++) if (g_ldrTb[i] == (ULONG_PTR)base) { known = 1; break; }
            for (i = 0; i < g_nLdrSeen; i++) if (g_ldrSeen[i] == (ULONG_PTR)base) { dup = 1; break; }
            if (VirtualQuery(base, &mb, sizeof(mb)))
            { isImg = (mb.Type == MEM_IMAGE); prot = mb.Protect; type = mb.Type; }
            if (!known)
            {
                (*ghost)++;
                if (!dup)
                {
                    WCHAR line[600];
                    StringCchPrintfW(line, 600, L"ldr ghost: %s @0x%llX size 0x%X type 0x%X protect 0x%X - in the %s loader list, NOT visible to toolhelp32",
                                     name[0] ? name : L"(unnamed)", (unsigned long long)(ULONG_PTR)base, size, type, prot, which);
                    Write(L"[SCAN]", line);
                    CsvRow(L"ldr-ghost", name, line, L"");
                }
            }
            else if (!isImg)
            {
                (*nonimg)++;
                if (!dup)
                {
                    WCHAR line[600];
                    StringCchPrintfW(line, 600, L"ldr non-image: %s @0x%llX size 0x%X protect 0x%X - loader entry points at memory that is not MEM_IMAGE",
                                     name[0] ? name : L"(unnamed)", (unsigned long long)(ULONG_PTR)base, size, prot);
                    Write(L"[SCAN]", line);
                    CsvRow(L"ldr-nonimage", name, line, L"");
                }
            }
            if (!dup && g_nLdrSeen < MAX_LDR) g_ldrSeen[g_nLdrSeen++] = (ULONG_PTR)base;
        }
        cur = next;
    }
    if (cycle)
    {
        WCHAR line[400];
        StringCchPrintfW(line, 400, L"ldr: %s list cycles after %d distinct entries - loader list relinked (not normal)",
                         which, total);
        Write(L"[SCAN]", line);
        CsvRow(L"ldr-cycle", which, line, L"");
    }
    return total;
}

static void LdrSurvey(int first)
{
    int i, ghost = 0, nonimg = 0;
    int nLoad = 0, nMem = 0, nInit = 0;
    HANDLE snap;
    BYTE* peb;
    BYTE* ldr;
    if (!ScanLdr) return;
    (void)first;
    g_nLdrTb = 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, g_pid ? g_pid : GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W me; me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me))
            do { if (g_nLdrTb < 1024) g_ldrTb[g_nLdrTb++] = (ULONG_PTR)me.hModule; } while (Module32NextW(snap, &me));
        CloseHandle(snap);
    }
    peb = (BYTE*)__readgsqword(0x60);
    if (!peb) return;
    __try { ldr = *(BYTE**)(peb + 0x18); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!ldr) return;
    nLoad = LdrWalkList((LIST_ENTRY*)(ldr + 0x10), 0x00, L"load-order", &ghost, &nonimg);
    nMem  = LdrWalkList((LIST_ENTRY*)(ldr + 0x20), 0x10, L"memory-order", &ghost, &nonimg);
    nInit = LdrWalkList((LIST_ENTRY*)(ldr + 0x30), 0x20, L"init-order", &ghost, &nonimg);
    if (!g_ldrSummaryDone && (nLoad || nMem || nInit || ghost || nonimg))
    {
        WCHAR line[400];
        g_ldrSummaryDone = 1;
        StringCchPrintfW(line, 400, L"ldr survey: load-order %d / memory-order %d / init-order %d distinct entries, %d not visible to toolhelp32, %d pointing at non-image memory",
                         nLoad, nMem, nInit, ghost, nonimg);
        Write(L"[SCAN]", line);
        CsvRow(L"ldr-summary", L"all", line, L"");
    }
    (void)i;
}

// ---------------------------------------------------------------- service watch
// One case-insensitive keyword list, matched against service names, display
// names, service binary paths and kernel driver paths. Generic by design: a
// protection's user-mode service, its kernel driver and its hypervisor all get
// caught by the same list.
static int WStrContainsCI(const WCHAR* hay, const WCHAR* needle)
{
    size_t nl = wcslen(needle);
    if (!nl || !hay) return 0;
    for (; *hay; hay++) if (_wcsnicmp(hay, needle, nl) == 0) return 1;
    return 0;
}

static int WatchMatch(const WCHAR* text)
{
    const WCHAR* p = ServiceWatch;
    if (!text || !text[0]) return 0;
    while (p && *p)
    {
        const WCHAR* e;
        int cl;
        WCHAR word[80];
        int n;
        while (*p == L' ' || *p == L',') p++;
        if (!*p) break;
        e = wcschr(p, L',');
        cl = e ? (int)(e - p) : (int)wcslen(p);
        while (cl > 0 && p[cl - 1] == L' ') cl--;
        if (cl > 0)
        {
            n = cl < 79 ? cl : 79;
            memcpy(word, p, (size_t)n * sizeof(WCHAR));
            word[n] = 0;
            if (WStrContainsCI(text, word)) return 1;
        }
        p = e ? e + 1 : p + wcslen(p);
    }
    return 0;
}

static int IsExecAddr(ULONG_PTR a)
{
    MEMORY_BASIC_INFORMATION mb;
    DWORD p;
    if (!a) return 0;
    if (!VirtualQuery((void*)a, &mb, sizeof(mb))) return 0;
    if (mb.State != MEM_COMMIT) return 0;
    p = mb.Protect & 0xFF;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

// ---------------------------------------------------------------- exception handlers
// The VEH chain is a circular doubly linked list of _VECTORED_HANDLER_ENTRY
// nodes and RtlAddVectoredExceptionHandler returns the NODE itself, so
// GameConsole's own registration cookie is a valid anchor into the chain. The
// node layout differs between Windows builds, so rather than hard-coding
// offsets it is derived once: the handler slot is the word holding our own
// VehNative pointer, and the LIST_ENTRY is the adjacent pair (Flink, Blink)
// whose links point back at the node. The list head is a sentinel inside ntdll
// data, recognised because its Handler slot is not executable code. Read-only.
#define VEH_MAX 64
static ULONG_PTR g_vehPrev[VEH_MAX]; static int g_nVehPrev = 0;
static int g_vehFirstDone = 0;
static int g_vehLayoutReported = 0;
static ULONG_PTR g_vehListOff = 0, g_vehHandlerOff = (ULONG_PTR)-1;
static int g_vehLayoutDone = 0;
static int g_vehHandlerEnc = 0;
static PVOID (WINAPI* pDecodePtr_)(PVOID) = NULL;

static ULONG_PTR VehDecode(ULONG_PTR v)
{
    if (!pDecodePtr_) return v;
    return (ULONG_PTR)pDecodePtr_((PVOID)v);
}

static void VehFindLayout(void)
{
    ULONG_PTR n = (ULONG_PTR)g_vehCookie, self = (ULONG_PTR)VehNative;
    int off;
    if (g_vehLayoutDone || !n) return;
    g_vehLayoutDone = 1;
    if (!pDecodePtr_)
        pDecodePtr_ = (PVOID (WINAPI*)(PVOID))(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlDecodePointer");
    // Recent Windows builds store the handler as RtlEncodePointer(handler), so
    // test both the raw value and its decoded form.
    for (off = 0; off <= 0x100; off += 8)
    {
        ULONG_PTR v = 0;
        __try { v = *(ULONG_PTR*)(n + off); }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (v == self) { g_vehHandlerOff = (ULONG_PTR)off; g_vehHandlerEnc = 0; break; }
        if (pDecodePtr_ && VehDecode(v) == self)
        { g_vehHandlerOff = (ULONG_PTR)off; g_vehHandlerEnc = 1; break; }
    }
    if (g_vehHandlerOff == (ULONG_PTR)-1) return;
    for (off = 0; off <= 0x38; off += 8)
    {
        ULONG_PTR f = 0, b = 0;
        if (off == (int)g_vehHandlerOff) continue;
        __try { f = *(ULONG_PTR*)(n + off); b = *(ULONG_PTR*)(n + off + 8); }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (!f || !b) continue;
        __try
        {
            if (*(ULONG_PTR*)(f + 8) == n + off && *(ULONG_PTR*)b == n + off)
            { g_vehListOff = (ULONG_PTR)off; break; }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
}

static int VehHandlerOf(ULONG_PTR node, ULONG_PTR* handler)
{
    ULONG_PTR h = 0;
    if (!node || g_vehHandlerOff == (ULONG_PTR)-1) return 0;
    __try { h = *(ULONG_PTR*)(node + g_vehHandlerOff); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (g_vehHandlerEnc) h = VehDecode(h);
    *handler = h;
    return 1;
}

static void VehDescribe(ULONG_PTR node, WCHAR* out, int cch)
{
    ULONG_PTR h = 0;
    WCHAR who[200];
    if (!VehHandlerOf(node, &h))
    {
        StringCchPrintfW(out, cch, L"<unreadable node 0x%llX>", (unsigned long long)node);
        return;
    }
    DescribeModuleOf((void*)h, who, 200);
    StringCchPrintfW(out, cch, L"%s%s", who,
                     node == (ULONG_PTR)g_vehCookie ? L" [GameConsole]" : L"");
}

static void VehChainSurvey(int first)
{
    ULONG_PTR me = (ULONG_PTR)g_vehCookie;
    ULONG_PTR fwd[VEH_MAX], bwd[VEH_MAX], chain[VEH_MAX + 1];
    int nf = 0, nb = 0, nc = 0, i, j;
    ULONG_PTR p;
    int changed = 0;
    if (!ScanEh) return;
    if (!me)
    {
        if (first && !g_vehFirstDone)
        {
            g_vehFirstDone = 1;
            Write(L"[EH]", L"veh chain: no anchor - GameConsole's own VEH is not installed (CaptureOds/LogAllExceptions/ExcCrashCodes all false), so the chain cannot be walked");
            CsvRow(L"eh-summary", L"veh", L"chain not walkable (no anchor)", L"");
        }
        return;
    }
    VehFindLayout();
    if (g_vehHandlerOff == (ULONG_PTR)-1)
    {
        if (!g_vehFirstDone)
        {
            WCHAR words[400], line[600];
            int k, o = 0;
            o += swprintf_s(words + o, 400 - o, L"");
            for (k = 0; k < 8; k++)
            {
                ULONG_PTR v = 0;
                __try { v = *(ULONG_PTR*)(me + k * 8); }
                __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
                o += swprintf_s(words + o, 400 - o, L"%s%llX", k ? L" " : L"", (unsigned long long)v);
            }
            g_vehFirstDone = 1;
            StringCchPrintfW(line, 600,
                L"veh chain: VEH node layout not recognised on this Windows build (no handler slot holding VehNative); node 0x%llX words: %s",
                (unsigned long long)me, words);
            Write(L"[EH]", line);
            CsvRow(L"eh-summary", L"veh", L"chain layout unrecognised", L"");
        }
        return;
    }
    __try { p = *(ULONG_PTR*)(me + g_vehListOff); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    while (p && p != me && nf < VEH_MAX - 1)
    {
        int dup = 0;
        ULONG_PTR h = 0;
        for (i = 0; i < nf; i++) if (fwd[i] == p) { dup = 1; break; }
        if (dup) break;
        if (!VehHandlerOf(p, &h) || !IsExecAddr(h)) break;   // list head sentinel
        fwd[nf++] = p;
        __try { p = *(ULONG_PTR*)(p + g_vehListOff); }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
    }
    __try { p = *(ULONG_PTR*)(me + g_vehListOff + 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    while (p && p != me && nb < VEH_MAX - 1)
    {
        int dup = 0;
        ULONG_PTR h = 0;
        for (i = 0; i < nb; i++) if (bwd[i] == p) { dup = 1; break; }
        if (dup) break;
        if (!VehHandlerOf(p, &h) || !IsExecAddr(h)) break;
        bwd[nb++] = p;
        __try { p = *(ULONG_PTR*)(p + g_vehListOff + 8); }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
    }

    // list order = execution order (a "first" handler is linked nearest the head)
    for (i = nb - 1; i >= 0; i--) chain[nc++] = bwd[i];
    chain[nc++] = me;
    for (i = 0; i < nf; i++) chain[nc++] = fwd[i];

    if (first && !g_vehLayoutReported)
    {
        WCHAR line[400];
        g_vehLayoutReported = 1;
        StringCchPrintfW(line, 400,
            L"veh chain: node layout - list links at +0x%llX, handler at +0x%llX%s",
            (unsigned long long)g_vehListOff, (unsigned long long)g_vehHandlerOff,
            g_vehHandlerEnc ? L" (RtlEncodePointer-encoded)" : L"");
        Write(L"[EH]", line);
        CsvRow(L"eh-summary", L"veh-layout", line, L"");
    }

    for (i = 0; i < nc && !changed; i++)
    {
        int found = 0;
        for (j = 0; j < g_nVehPrev; j++) if (g_vehPrev[j] == chain[i]) { found = 1; break; }
        if (!found) changed = 1;
    }
    if (!changed && nc != g_nVehPrev) changed = 1;
    if (!changed && !first) return;

    {
        WCHAR line[900];
        StringCchPrintfW(line, 900, L"veh chain: %d handler(s) - listed in the order Windows calls them", nc);
        Write(L"[EH]", line);
        CsvRow(L"eh-summary", L"veh", line, L"");
    }
    for (i = 0; i < nc; i++)
    {
        WCHAR who[220], line[700];
        VehDescribe(chain[i], who, 220);
        StringCchPrintfW(line, 700, L"veh chain[%d]%s: %s",
                         i, i == 0 ? L" (first)" : (i == nc - 1 ? L" (last)" : L""), who);
        Write(L"[EH]", line);
        CsvRow(L"eh-veh", who, line, L"");
    }
    // removals: previously present nodes that are gone
    for (j = 0; j < g_nVehPrev; j++)
    {
        int found = 0;
        for (i = 0; i < nc; i++) if (chain[i] == g_vehPrev[j]) { found = 1; break; }
        if (!found)
        {
            WCHAR who[220], line[700];
            VehDescribe(g_vehPrev[j], who, 220);
            StringCchPrintfW(line, 700, L"veh removed: %s (cookie no longer in the chain)", who);
            Write(L"[EH]", line);
            CsvRow(L"eh-veh", who, line, L"");
        }
    }
    g_nVehPrev = 0;
    for (i = 0; i < nc && g_nVehPrev < VEH_MAX; i++) g_vehPrev[g_nVehPrev++] = chain[i];

    // Continuation handlers live in their own chain anchored by a different
    // cookie (RtlAddVectoredContinueHandler); only the VEH chain is walkable
    // from here, so registrations of those are logged by the IAT hooks.
}

// CPUID: a hypervisor leaves CPUID.1:ECX[31] set and identifies itself through
// leaf 0x40000000. A protection that ships its own hypervisor (or runs under
// one) shows up here; a plain game process on physical hardware does not.
//
// This is re-probed every heartbeat and only logged when the state CHANGES, so
// a hypervisor that is armed mid-session (or unloaded) is caught - and it is
// cross-referenced against the loaded kernel modules, because a protection
// hypervisor can be present in kernel space while keeping the CPUID bit clear
// (seen with hyperkd.sys on Planet Zoo 2).
static int g_hvState = -1;                  // -1 = unknown, 0 = absent, 1 = present
static WCHAR g_hvLast[128] = L"";
// loaded kernel modules that matched ServiceWatch (basenames, with .sys),
// filled by DriversScan, so the CPUID probe can cross-reference what is
// actually loaded in kernel space
static WCHAR g_drvMatch[16][48]; static int g_nDrvMatch = 0;

static void CpuTopologyInfo(void)
{
    int r[4];
    char vend[16], hvend[16];
    WCHAR wv[64] = L"", wh[64] = L"";
    int i, hv, stdMax = 0, extMax = 0;
    WCHAR line[900], state[128], drv[300];
    for (i = 0; i < 16; i++) { vend[i] = 0; hvend[i] = 0; }
    __cpuid(r, 0);
    memcpy(vend, &r[1], 4); memcpy(vend + 4, &r[3], 4); memcpy(vend + 8, &r[2], 4);
    vend[12] = 0;
    stdMax = r[0];
    __cpuid(r, 0x80000000);
    extMax = r[0];
    __cpuid(r, 1);
    hv = (r[2] >> 31) & 1;                      // CPUID.1:ECX[31] hypervisor-present
    MultiByteToWideChar(CP_ACP, 0, vend, -1, wv, 64);
    drv[0] = 0;
    for (i = 0; i < g_nDrvMatch; i++)
    {
        if (i) StringCchCatW(drv, 300, L", ");
        StringCchCatW(drv, 300, g_drvMatch[i]);
    }
    if (hv)
    {
        __cpuid(r, 0x40000000);
        memcpy(hvend, &r[1], 4); memcpy(hvend + 4, &r[2], 4); memcpy(hvend + 8, &r[3], 4);
        hvend[12] = 0;
        for (i = 0; i < 12; i++)
            if (hvend[i] && (hvend[i] < 0x20 || hvend[i] > 0x7E)) hvend[i] = L'.';
        MultiByteToWideChar(CP_ACP, 0, hvend, -1, wh, 64);
        {
            int feat = 0;
            if (r[0] >= 0x40000001) { int r2[4]; __cpuid(r2, 0x40000001); feat = r2[0]; }
            StringCchPrintfW(line, 900,
                L"hypervisor: PRESENT - cpu vendor \"%s\", hypervisor vendor \"%s\", hypervisor leaves 0x40000000-0x%X, feature word 0x%X, cpu leaves std 0x%X ext 0x%X%s%s%s",
                wv, wh, r[0], feat, stdMax, extMax,
                WatchMatch(wh) ? L" - that vendor matches ServiceWatch" : L"",
                drv[0] ? L"; ServiceWatch kernel module(s) loaded: " : L"", drv);
        }
        StringCchPrintfW(state, 128, L"present:%s|%s", wh, drv);
    }
    else
    {
        // the interesting case: no CPUID leaf, but a protection driver is loaded
        StringCchPrintfW(line, 900,
            L"hypervisor: no CPUID hypervisor leaf (CPUID.1:ECX[31]=0, cpu vendor \"%s\", leaves std 0x%X ext 0x%X)%s%s - a hypervisor can still be present in kernel space without advertising itself",
            wv, stdMax, extMax,
            drv[0] ? L"; ServiceWatch kernel module(s) loaded: " : L"", drv);
        StringCchPrintfW(state, 128, L"absent|%s", drv);
    }
    if (g_hvState != -1 && _wcsicmp(g_hvLast, state) == 0) return;   // unchanged
    if (g_hvState != -1)
    {
        WCHAR note[600];
        StringCchPrintfW(note, 600, L"hypervisor state CHANGED since the last probe: %s", state);
        Write(L"[SYS]", note);
        CsvRow(L"sys-cpu", L"change", note, L"");
    }
    g_hvState = hv;
    StringCchCopyW(g_hvLast, 128, state);
    Write(L"[SYS]", line);
    CsvRow(L"sys-cpu", hv ? (wh[0] ? wh : L"present") : L"none", line, L"");
}

// ---------------------------------------------------------------- code integrity
// A helper that has to start an unsigned kernel driver (the protection
// hypervisor) must first switch DSE - driver signature enforcement - off, and
// switch it back on afterwards. That is not visible as a memory write we can
// hook, but its *effect* is queryable from user mode:
// NtQuerySystemInformation(SystemCodeIntegrityInformation, 103) returns the Code
// Integrity options, bit 0 being CODEINTEGRITY_OPTION_ENABLED (a kernel driver
// must be signed to load). Sampled at startup and on the same cadence as the
// driver scan, so "enabled -> not enabled -> enabled" lands in the log with a
// timestamp beside the SCM calls - the install is then provable even when the
// write itself happens in a process we cannot instrument (services.exe) or in
// kernel memory. Read-only: one query, no handle, no writes.
typedef struct { ULONG Length; ULONG CodeIntegrityOptions; } GC_CIOPTS;

static ULONG g_ciLast = 0xFFFFFFFFu;         // 0xFFFFFFFF = never sampled
static int   g_ciUnsupported = 0;

static void CiOptionsText(ULONG o, WCHAR* out, int cch)
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
        if (tmp[0]) StringCchCatW(tmp, 240, L" ");
        StringCchCatW(tmp, 240, names[i]);
    }
    if (o >> 13)
    {
        WCHAR tail[60];
        StringCchPrintfW(tail, 60, L"%sundecoded 0x%X", tmp[0] ? L" " : L"", o >> 13);
        StringCchCatW(tmp, 240, tail);
    }
    if (!tmp[0]) StringCchCopyW(tmp, 240, L"none");
    StringCchCopyW(out, (size_t)cch, tmp);
}

static void CodeIntegrityInfo(void)
{
    typedef LONG (WINAPI* t_NtQSIci)(ULONG, void*, ULONG, ULONG*);
    t_NtQSIci q;
    GC_CIOPTS ci;
    ULONG ret = 0;
    LONG st;
    WCHAR names[240], line[700];
    if (g_ciUnsupported) return;
    q = (t_NtQSIci)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    if (!q) return;
    memset(&ci, 0, sizeof(ci));
    ci.Length = sizeof(ci);
    st = (LONG)q(103 /* SystemCodeIntegrityInformation */, &ci, sizeof(ci), &ret);
    if (st != 0 || ci.Length < sizeof(ci))
    {
        // say so once rather than silently reporting nothing forever
        WCHAR note[400];
        g_ciUnsupported = 1;
        StringCchPrintfW(note, 400,
            L"code integrity: NtQuerySystemInformation(SystemCodeIntegrityInformation) "
            L"unsupported here (st=0x%08X, returned %lu) - DSE changes cannot be observed",
            (unsigned)st, (unsigned long)ci.Length);
        Write(L"[SYS]", note);
        CsvRow(L"sys-ci", L"unsupported", note, L"");
        return;
    }
    if (g_ciLast == ci.CodeIntegrityOptions) return;
    CiOptionsText(ci.CodeIntegrityOptions, names, 240);
    if (g_ciLast == 0xFFFFFFFFu)
    {
        StringCchPrintfW(line, 700, L"code integrity: options 0x%08X (%s) - %s",
                         (unsigned)ci.CodeIntegrityOptions, names,
                         (ci.CodeIntegrityOptions & 1)
                             ? L"kernel-mode driver signing enforced (DSE on)"
                             : L"kernel-mode driver signing NOT enforced (DSE already off)");
    }
    else
    {
        const WCHAR* what = L"";
        if (!(g_ciLast & 1) && (ci.CodeIntegrityOptions & 1))
            what = L" - DSE was turned back ON";
        else if ((g_ciLast & 1) && !(ci.CodeIntegrityOptions & 1))
            what = L" - DSE was turned OFF (an unsigned kernel driver can load now)";
        StringCchPrintfW(line, 700, L"code integrity CHANGED 0x%08X -> 0x%08X (%s)%s",
                         (unsigned)g_ciLast, (unsigned)ci.CodeIntegrityOptions, names, what);
    }
    g_ciLast = ci.CodeIntegrityOptions;
    Write(L"[SYS]", line);
    CsvRow(L"sys-ci", (ci.CodeIntegrityOptions & 1) ? L"dse-on" : L"dse-off", line, L"");
}

// ---------------------------------------------------------------- kernel modules
// NtQuerySystemInformation(SystemModuleInformation) lists every module loaded
// into kernel space: drivers, the hypervisor itself, and any protection driver.
// Read-only query, no handle to anything, no writes.
typedef NTSTATUS (NTAPI* t_NtQSI)(ULONG, PVOID, ULONG, PULONG);
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
} GC_SYS_MOD;
typedef struct { ULONG NumberOfModules; GC_SYS_MOD Modules[1]; } GC_SYS_MODS;

static void InstalledDriversScan(int first);

static ULONG_PTR g_drvSeen[512]; static int g_nDrvSeen = 0;
static int g_drvLastCount = -1;

static ULONG_PTR ModNameHash(const WCHAR* s, ULONG_PTR base)
{
    ULONG_PTR h = 1469598103934665603ULL;
    if (base) h = (h ^ base) * 1099511628211ULL;
    for (; s && *s; s++) h = (h ^ (ULONG_PTR)(*s & 0xFF)) * 1099511628211ULL;
    return h;
}

// The loaded module is "hvservice.sys" while the driver service key is
// "hvservice", so compare with the .sys suffix stripped.
static ULONG_PTR DrvKeyOf(const WCHAR* name)
{
    WCHAR buf[64];
    size_t n = name ? wcslen(name) : 0;
    if (n > 4 && _wcsicmp(name + n - 4, L".sys") == 0) n -= 4;
    if (n > 63) n = 63;
    if (n) memcpy(buf, name, n * sizeof(WCHAR));
    buf[n] = 0;
    return ModNameHash(buf, 0);
}

static void DriversScan(int first)
{
    t_NtQSI q;
    ULONG need = 0, st;
    GC_SYS_MODS* mods;
    ULONG i;
    int matches = 0, nnew = 0, zeroBase = 0;
    WCHAR line[900];
    if (!ScanDrivers) return;
    q = (t_NtQSI)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    if (!q) return;
    g_nDrvMatch = 0;
    st = (ULONG)q(11 /* SystemModuleInformation */, NULL, 0, &need);
    if (!need || need > (64u << 20)) return;
    mods = (GC_SYS_MODS*)HeapAlloc(GetProcessHeap(), 0, need + 65536);
    if (!mods) return;
    st = (ULONG)q(11, mods, need + 65536, &need);
    if (st != 0)
    {
        HeapFree(GetProcessHeap(), 0, mods);
        return;
    }
    for (i = 0; i < mods->NumberOfModules; i++)
    {
        GC_SYS_MOD* m = &mods->Modules[i];
        WCHAR path[300];
        const WCHAR* base;
        ULONG_PTR img = (ULONG_PTR)m->ImageBase;
        ULONG_PTR key;
        int k, dup = 0;
        MultiByteToWideChar(CP_ACP, 0, (const char*)m->FullPathName, -1, path, 300);
        base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        // ImageBase is redacted (zero) for an ordinary caller, and MappedBase is
        // NOT a usable substitute: it is not an image base, and on this box it
        // carried text that printed as `hyperkd.sys @0x7465536C6F72746E` ("nrolSet"
        // - a fragment of "CurrentControlSet"). Report a base only when it is a
        // canonical kernel address; otherwise say the base is redacted and keep
        // the exact facts (name, size, load order).
        if (!img || img < 0xFFFF800000000000ull) { img = 0; zeroBase++; }
        // record EVERY loaded module by base name (not only matches), so the
        // registry view can ask whether a driver service is currently loaded;
        // nothing is logged for the ones that do not match
        key = DrvKeyOf(base);
        for (k = 0; k < g_nDrvSeen; k++) if (g_drvSeen[k] == key) { dup = 1; break; }
        if (!dup && g_nDrvSeen < 512) g_drvSeen[g_nDrvSeen++] = key;
        if (!(ServiceWatchAll || WatchMatch(path) || WatchMatch(base))) continue;
        matches++;
        if (g_nDrvMatch < 16 && _wcsicmp(base, L"ntoskrnl.exe") != 0 && _wcsicmp(base, L"hal.dll") != 0)
        {
            int k2, have = 0;
            for (k2 = 0; k2 < g_nDrvMatch; k2++) if (_wcsicmp(g_drvMatch[k2], base) == 0) { have = 1; break; }
            if (!have) StringCchCopyW(g_drvMatch[g_nDrvMatch++], 48, base);
        }
        if (dup && !first) continue;
        if (!dup) nnew++;
        if (img)
            StringCchPrintfW(line, 900, L"driver: %s @0x%llX size 0x%X load-order %u - kernel module matching ServiceWatch: %s",
                             base, (unsigned long long)img,
                             (unsigned)m->ImageSize, (unsigned)m->LoadOrderIndex, path);
        else
            StringCchPrintfW(line, 900, L"driver: %s size 0x%X load-order %u (base redacted for this caller) - kernel module matching ServiceWatch: %s",
                             base,
                             (unsigned)m->ImageSize, (unsigned)m->LoadOrderIndex, path);
        Write(L"[KRNL]", line);
        CsvRow(L"krnl-driver", base, line, L"");
    }
    if (first || (int)mods->NumberOfModules != g_drvLastCount || nnew)
    {
        StringCchPrintfW(line, 900,
            L"kernel modules: %lu loaded in kernel space, %d match ServiceWatch (%d new this pass)%s",
            (unsigned long)mods->NumberOfModules, matches, nnew,
            zeroBase ? L" - kernel base address not exposed for the matched modules (Windows redacts it); names/sizes/load order are exact" : L"");
        Write(L"[KRNL]", line);
        CsvRow(L"krnl-summary", L"all", line, L"");
    }
    g_drvLastCount = (int)mods->NumberOfModules;
    HeapFree(GetProcessHeap(), 0, mods);
    InstalledDriversScan(first);
}

// ---------------------------------------------------------------- installed drivers
// The registry view of kernel drivers: HKLM\SYSTEM\CurrentControlSet\Services
// with Type 1 (kernel driver) / 2 (file system driver). This is readable
// without elevation - unlike the object manager's \Driver directory, which
// returns STATUS_ACCESS_DENIED (0xC0000022) to a normal game process - and it
// answers a question the loaded-module list cannot: a driver named in the
// config (e.g. hyperkd.sys / simplesvm.sys) is reported as
// installed-but-not-loaded until the game actually starts it, then as loaded.
typedef LONG (WINAPI* t_RegOpenKeyExW)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
typedef LONG (WINAPI* t_RegEnumKeyExW)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPWSTR, LPDWORD, PFILETIME);
typedef LONG (WINAPI* t_RegQueryValueExW)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI* t_RegCloseKey)(HKEY);

typedef struct {
    WCHAR name[64];
    WCHAR image[260];
    DWORD type, start;
    int loaded, reported;
} InstDrv;
#define MAX_INSTDRV 640
static InstDrv g_instDrv[MAX_INSTDRV]; static int g_nInstDrv = 0;
static int g_instDrvDone = 0;

static const WCHAR* SvcStartName(DWORD s)
{
    switch (s)
    {
    case 0: return L"boot";
    case 1: return L"system";
    case 2: return L"auto";
    case 3: return L"demand";
    case 4: return L"disabled";
    default: return L"?";
    }
}

// is a module with this base name in the loaded-kernel-module table?
static int DrvIsLoaded(const WCHAR* name)
{
    ULONG_PTR key = DrvKeyOf(name);
    int i;
    for (i = 0; i < g_nDrvSeen; i++) if (g_drvSeen[i] == key) return 1;
    return 0;
}

static void InstalledDriversScan(int first)
{
    HMODULE adv = GetModuleHandleW(L"advapi32.dll");
    t_RegOpenKeyExW pOpen;
    t_RegEnumKeyExW pEnum;
    t_RegQueryValueExW pQuery;
    t_RegCloseKey pClose;
    HKEY root;
    DWORD i;
    WCHAR line[900];
    int matches = 0, nloaded = 0;
    if (!ScanDrivers) return;
    if (!adv) adv = LoadLibraryW(L"advapi32.dll");
    if (!adv) return;
    pOpen  = (t_RegOpenKeyExW)(void*)GetProcAddress(adv, "RegOpenKeyExW");
    pEnum  = (t_RegEnumKeyExW)(void*)GetProcAddress(adv, "RegEnumKeyExW");
    pQuery = (t_RegQueryValueExW)(void*)GetProcAddress(adv, "RegQueryValueExW");
    pClose = (t_RegCloseKey)(void*)GetProcAddress(adv, "RegCloseKey");
    if (!pOpen || !pEnum || !pQuery || !pClose) return;

    if (pOpen(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services", 0,
              0x20019 /* KEY_READ */, &root) != 0) return;
    for (i = 0; i < 4096; i++)
    {
        WCHAR key[260], img[260];
        DWORD kl = 260, type = 0, start = 3, sz, t = 0;
        HKEY h;
        int kind, k, found = -1;
        if (pEnum(root, i, key, &kl, NULL, NULL, NULL, NULL) != 0) break;
        if (pOpen(root, key, 0, 0x20019, &h) != 0) continue;
        sz = sizeof(type);
        if (pQuery(h, L"Type", NULL, &t, (LPBYTE)&type, &sz) != 0) type = 0;
        sz = sizeof(start);
        if (pQuery(h, L"Start", NULL, &t, (LPBYTE)&start, &sz) != 0) start = 3;
        img[0] = 0; sz = sizeof(img) - sizeof(WCHAR);
        if (pQuery(h, L"ImagePath", NULL, &t, (LPBYTE)img, &sz) == 0)
            img[sz / sizeof(WCHAR)] = 0;
        pClose(h);
        kind = (int)(type & 0xF);
        if (kind != 1 && kind != 2) continue;              // kernel / fs driver only
        for (k = 0; k < g_nInstDrv; k++)                   // already cached?
            if (_wcsicmp(g_instDrv[k].name, key) == 0) { found = k; break; }
        if (found >= 0)
        {
            // a protection can rewrite a driver key it created: ImagePath and
            // Start changes are reported whatever the name looks like
            if (_wcsicmp(g_instDrv[found].image, img) != 0 || g_instDrv[found].start != start)
            {
                StringCchPrintfW(line, 900,
                    L"driver service key changed: %s start=%s image=%s -> start=%s image=%s - kernel driver registration modified while the game ran",
                    key, SvcStartName(g_instDrv[found].start),
                    g_instDrv[found].image[0] ? g_instDrv[found].image : L"(none)",
                    SvcStartName(start), img[0] ? img : L"(none)");
                Write(L"[SVC]", line);
                CsvRow(L"svc-driver", key, line, L"");
                StringCchCopyW(g_instDrv[found].image, 260, img);
                g_instDrv[found].start = start;
            }
            continue;
        }
        if (g_nInstDrv >= MAX_INSTDRV) continue;
        StringCchCopyW(g_instDrv[g_nInstDrv].name, 64, key);
        StringCchCopyW(g_instDrv[g_nInstDrv].image, 260, img);
        g_instDrv[g_nInstDrv].type = type;
        g_instDrv[g_nInstDrv].start = start;
        // the loaded check must use the DRIVER FILE name, not the service key
        // name: the protection registers hyperkd.sys under a service called
        // "artifact", so matching by key name wrongly reported "not loaded"
        {
            const WCHAR* ib = wcsrchr(img, L'\\');
            ib = ib ? ib + 1 : img;
            g_instDrv[g_nInstDrv].loaded = ib[0] ? DrvIsLoaded(ib) : DrvIsLoaded(key);
        }
        // entries cached at startup are still reported (once) by the loop
        // below; entries cached later were just logged as newly registered
        g_instDrv[g_nInstDrv].reported = first ? 0 : 1;
        g_nInstDrv++;
        if (!first)
        {
            // new kernel driver registration appearing mid-session: this is how
            // artifact/dse set up the hypervisor driver after the DLL loaded, so
            // it is logged whether or not the name matches ServiceWatch
            const WCHAR* b = wcsrchr(img, L'\\');
            b = b ? b + 1 : img;
            StringCchPrintfW(line, 900,
                L"driver service registered: %s type=0x%X start=%s image=%s - kernel driver service added while the game ran%s",
                key, (unsigned)type, SvcStartName(start), img[0] ? img : L"(no ImagePath)",
                (WatchMatch(key) || WatchMatch(b) || WatchMatch(img)) ? L" (matches ServiceWatch)" : L"");
            Write(L"[SVC]", line);
            CsvRow(L"svc-driver", key, line, L"");
        }
    }
    pClose(root);

    for (i = 0; i < (DWORD)g_nInstDrv; i++)
    {
        const WCHAR* b = wcsrchr(g_instDrv[i].image, L'\\');
        int isMatch;
        b = b ? b + 1 : g_instDrv[i].image;
        isMatch = ServiceWatchAll || WatchMatch(g_instDrv[i].name) || WatchMatch(b) || WatchMatch(g_instDrv[i].image);
        if (isMatch)
        {
            int now = b[0] ? DrvIsLoaded(b) : DrvIsLoaded(g_instDrv[i].name);
            matches++;
            if (now) nloaded++;
            if (!g_instDrv[i].reported || now != g_instDrv[i].loaded)
            {
                StringCchPrintfW(line, 900,
                    L"installed driver: %s type=0x%X start=%s image=%s - matches ServiceWatch (%s)",
                    g_instDrv[i].name, (unsigned)g_instDrv[i].type, SvcStartName(g_instDrv[i].start),
                    g_instDrv[i].image[0] ? g_instDrv[i].image : L"(no ImagePath)",
                    now ? (g_instDrv[i].reported ? L"is now LOADED - the game started it"
                                                : L"currently LOADED (already up at first scan - the mod loads it during process startup)")
                       : (g_instDrv[i].reported ? L"is no longer loaded" : L"NOT currently loaded"));
                Write(L"[SVC]", line);
                CsvRow(L"svc-driver", g_instDrv[i].name, line, L"");
                g_instDrv[i].reported = 1;
                g_instDrv[i].loaded = now;
            }
        }
        else g_instDrv[i].reported = 1;
    }
    if (first)
    {
        StringCchPrintfW(line, 900,
            L"installed driver services: %d kernel/fs driver key(s) known, %d match ServiceWatch (%d loaded) - registry view, no elevation needed",
            g_nInstDrv, matches, nloaded);
        Write(L"[SVC]", line);
        CsvRow(L"svc-summary", L"drivers", line, L"");
    }
}

// ---------------------------------------------------------------- services// Enumerates the service control manager read-only and reports services whose
// name, display name or binary path matches ServiceWatch, plus state changes.
// A protection that runs a service (its licence/anti-tamper/hypervisor helper)
// is named here even when the process itself never opens a service handle.
typedef SC_HANDLE (WINAPI* t_OpenSCM)(LPCWSTR, LPCWSTR, DWORD);
typedef BOOL      (WINAPI* t_EnumSvc)(SC_HANDLE, SC_ENUM_TYPE, DWORD, DWORD, LPBYTE, DWORD, LPDWORD, LPDWORD, LPDWORD, LPCWSTR);
typedef BOOL      (WINAPI* t_CloseSvcH)(SC_HANDLE);
typedef SC_HANDLE (WINAPI* t_OpenSvc)(SC_HANDLE, LPCWSTR, DWORD);
typedef BOOL      (WINAPI* t_QuerySvcCfg)(SC_HANDLE, LPQUERY_SERVICE_CONFIGW, DWORD, LPDWORD);

typedef struct { WCHAR name[64]; DWORD state; } SvcPrev;
#define MAX_SVC 1024
static SvcPrev g_svcPrev[MAX_SVC]; static int g_nSvcPrev = 0;
static int g_nSvcLastCount = -1;

static const WCHAR* SvcStateName(DWORD s)
{
    switch (s)
    {
    case SERVICE_STOPPED: return L"stopped";
    case SERVICE_START_PENDING: return L"start-pending";
    case SERVICE_STOP_PENDING: return L"stop-pending";
    case SERVICE_RUNNING: return L"RUNNING";
    case SERVICE_CONTINUE_PENDING: return L"continue-pending";
    case SERVICE_PAUSE_PENDING: return L"pause-pending";
    case SERVICE_PAUSED: return L"paused";
    default: return L"?";
    }
}

static void ServicesScan(int first)
{
    HMODULE adv = GetModuleHandleW(L"advapi32.dll");
    t_OpenSCM pOpen;
    t_EnumSvc pEnum;
    t_CloseSvcH pClose;
    t_OpenSvc pOpenSvc;
    t_QuerySvcCfg pCfg;
    SC_HANDLE scm;
    DWORD need = 0, count = 0, resume = 0, i;
    BYTE* buf;
    int matches = 0, nnew = 0, nchg = 0;
    WCHAR line[1100];
    if (!ScanServices) return;
    if (!adv) adv = LoadLibraryW(L"advapi32.dll");
    if (!adv) return;
    pOpen    = (t_OpenSCM)(void*)GetProcAddress(adv, "OpenSCManagerW");
    pEnum    = (t_EnumSvc)(void*)GetProcAddress(adv, "EnumServicesStatusExW");
    pClose   = (t_CloseSvcH)(void*)GetProcAddress(adv, "CloseServiceHandle");
    pOpenSvc = (t_OpenSvc)(void*)GetProcAddress(adv, "OpenServiceW");
    pCfg     = (t_QuerySvcCfg)(void*)GetProcAddress(adv, "QueryServiceConfigW");
    if (!pOpen || !pEnum || !pClose) return;
    scm = pOpen(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return;
    pEnum(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32 | SERVICE_DRIVER,
          SERVICE_STATE_ALL, NULL, 0, &need, &count, &resume, NULL);
    if (!need || need > (32u << 20)) { pClose(scm); return; }
    {
        int attempt;
        buf = NULL;
        for (attempt = 0; attempt < 3; attempt++)
        {
            DWORD size = need + 65536, n = 0, c = 0, r = 0;
            if (attempt > 0)
            {
                // the service list can change between calls, so re-size
                void* dummy = NULL; (void)dummy;
                pEnum(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32 | SERVICE_DRIVER,
                      SERVICE_STATE_ALL, NULL, 0, &n, &c, &r, NULL);
                size = (n ? n : need + attempt * 65536) + 65536;
            }
            if (size > (32u << 20)) break;
            buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, size);
            if (!buf) break;
            resume = 0;
            count = 0;
            if (pEnum(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32 | SERVICE_DRIVER,
                      SERVICE_STATE_ALL, buf, size, &need, &count, &resume, NULL))
                break;
            HeapFree(GetProcessHeap(), 0, buf);
            buf = NULL;
        }
        if (!buf) { pClose(scm); return; }
    }
    for (i = 0; i < count; i++)
    {
        ENUM_SERVICE_STATUS_PROCESSW* e = &((ENUM_SERVICE_STATUS_PROCESSW*)buf)[i];
        DWORD state = e->ServiceStatusProcess.dwCurrentState;
        int prev = -1, k, nameMatch, isNew, chg, isMatch;
        WCHAR path[520] = L"";
        for (k = 0; k < g_nSvcPrev; k++)
            if (_wcsicmp(g_svcPrev[k].name, e->lpServiceName) == 0) { prev = k; break; }
        isNew = (prev < 0);
        chg = !isNew && g_svcPrev[prev].state != state;
        // Track every service's state, not just the ones that matched: a
        // protection driver is often started by the game, so the pass where it
        // appears is a "new service that is now RUNNING" and only then can its
        // binary path identify it.
        if (isNew)
        {
            if (g_nSvcPrev < MAX_SVC)
            {
                StringCchCopyW(g_svcPrev[g_nSvcPrev].name, 64, e->lpServiceName);
                g_svcPrev[g_nSvcPrev].state = state;
                g_nSvcPrev++;
            }
        }
        else g_svcPrev[prev].state = state;

        nameMatch = WatchMatch(e->lpServiceName) || WatchMatch(e->lpDisplayName);
        if (first || ServiceWatchAll || nameMatch || chg ||
            state == SERVICE_RUNNING || state == SERVICE_START_PENDING)
        {
            if (pOpenSvc && pCfg)
            {
                SC_HANDLE h = pOpenSvc(scm, e->lpServiceName, SERVICE_QUERY_CONFIG);
                if (h)
                {
                    DWORD cfgNeed = 0;
                    BYTE cfgBuf[4096];
                    if (pCfg(h, (LPQUERY_SERVICE_CONFIGW)cfgBuf, sizeof(cfgBuf), &cfgNeed))
                    {
                        QUERY_SERVICE_CONFIGW* c = (QUERY_SERVICE_CONFIGW*)cfgBuf;
                        if (c->lpBinaryPathName)
                            StringCchCopyW(path, 520, c->lpBinaryPathName);
                    }
                    pClose(h);
                }
            }
        }
        isMatch = nameMatch || WatchMatch(path) || ServiceWatchAll;
        if (!isMatch) continue;
        matches++;
        if (!isNew && !chg) continue;      // already reported on an earlier pass
        if (isNew) nnew++; else nchg++;
        StringCchPrintfW(line, 1100,
            L"service: %s \"%s\" state=%s pid=%lu type=0x%X controls=0x%X%s%s",
            e->lpServiceName, e->lpDisplayName ? e->lpDisplayName : L"",
            SvcStateName(state), (unsigned long)e->ServiceStatusProcess.dwProcessId,
            (unsigned)e->ServiceStatusProcess.dwServiceType,
            (unsigned)e->ServiceStatusProcess.dwControlsAccepted,
            path[0] ? L" - " : L"", path);
        Write(L"[SVC]", line);
        CsvRow(L"svc-service", e->lpServiceName, line, L"");
    }
    if (first || (int)count != g_nSvcLastCount || nnew || nchg)
    {
        StringCchPrintfW(line, 1100,
            L"services: %lu enumerated, %d match ServiceWatch (%d new, %d state change)",
            (unsigned long)count, matches, nnew, nchg);
        Write(L"[SVC]", line);
        CsvRow(L"svc-summary", L"all", line, L"");
    }
    g_nSvcLastCount = (int)count;
    HeapFree(GetProcessHeap(), 0, buf);
    pClose(scm);
}

// ---------------------------------------------------------------- mailbox + shadow

// artifact arms Dr3 at KUSER_SHARED_DATA+0xFF0 and never reads it back; a data
// address that never executes cannot be a working breakpoint, so the page may
// be a handshake mailbox. Watch the tail of KUSER_SHARED_DATA for changes.
static BYTE g_sdPrev[32];
static int  g_sdHave = 0;

static void HexBytes(const BYTE* p, int n, WCHAR* out, int cch)
{
    static const WCHAR hx[] = L"0123456789ABCDEF";
    int i, o = 0;
    for (i = 0; i < n && o < cch - 3; i++)
    {
        out[o++] = hx[p[i] >> 4];
        out[o++] = hx[p[i] & 0xF];
    }
    out[o] = 0;
}

static void WatchSharedDataTick(void)
{
    const BYTE* p = (const BYTE*)(ULONG_PTR)0x7FFE0000ULL;
    BYTE cur[32]; int i; WCHAR msg[600], hex[80], hexo[80];
    if (!WatchSharedData) return;
    __try { for (i = 0; i < 32; i++) cur[i] = p[0xFE0 + i]; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    HexBytes(cur, 32, hex, 80);
    if (!g_sdHave)
    {
        StringCchPrintfW(msg, 600, L"KUSER_SHARED_DATA@0x7FFE0FE0 baseline %s", hex);
        Write(L"[SD]", msg);
        CsvRow(L"shareddata", L"baseline", hex, L"");
        memcpy(g_sdPrev, cur, 32);
        g_sdHave = 1;
        return;
    }
    if (memcmp(cur, g_sdPrev, 32) != 0)
    {
        HexBytes(g_sdPrev, 32, hexo, 80);
        StringCchPrintfW(msg, 600, L"KUSER_SHARED_DATA@0x7FFE0FE0 changed %s -> %s", hexo, hex);
        Write(L"[SD]", msg);
        CsvRow(L"shareddata", L"change", hexo, hex);
        memcpy(g_sdPrev, cur, 32);
    }
}

// Committed, non-image pages carrying PAGE_GUARD or PAGE_NOACCESS: classic
// anti-debug tripwires. Deduped by address, capped.
#define MAX_GUARD 256
static ULONG_PTR g_guard[MAX_GUARD]; static int g_nGuard = 0;
static int g_guardSkipped = 0;   // distinct low-address guards counted, not listed
static int g_guardSkipFull = 0;  // dedup table saturated
#define MAX_GUARDSKIP 1024
static ULONG_PTR g_guardSkip[MAX_GUARDSKIP]; static int g_nGuardSkip = 0;

static void ScanGuardPages(void)
{
    ULONG_PTR addr = 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (!MemGuardScan) return;
    while (addr < 0x7FFFFFFF0000ULL && VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) != 0)
    {
        ULONG_PTR b = (ULONG_PTR)mbi.BaseAddress;
        if (mbi.State == MEM_COMMIT && mbi.Type != MEM_IMAGE &&
            ((mbi.Protect & 0x100) || (mbi.Protect & 0xFF) == 0x01))
        {
            if (!ExecContains(g_guard, g_nGuard, b))
            {
                // Stack and heap guards are PAGE_GUARD/PAGE_NOACCESS at LOW
                // addresses; those are noise. The protection's tripwires would
                // be in the high module range (0x7F...). List the high ones (and
                // any executable guard) always; count the rest (MemGuardAll
                // lists them too). Nothing is silently dropped.
                int interesting = mbi.Protect & 0xF0;             // executable
                if (!interesting && b >= 0x7F0000000000ULL) interesting = 1;
                if (interesting || MemGuardAll)
                {
                    if (g_nGuard < MAX_GUARD)
                    {
                        WCHAR msg[400];
                        g_guard[g_nGuard++] = b;
                        StringCchPrintfW(msg, 400, L"%s page @0x%llX size 0x%llX protect 0x%X type 0x%X",
                                         (mbi.Protect & 0x100) ? L"guard" : L"no-access",
                                         (unsigned long long)b, (unsigned long long)mbi.RegionSize,
                                         (unsigned)mbi.Protect, (unsigned)mbi.Type);
                        Write(L"[MEM]", msg);
                        CsvRow(L"guardpage", (mbi.Protect & 0x100) ? L"guard" : L"noaccess", msg, L"");
                    }
                }
                else if (!ExecContains(g_guardSkip, g_nGuardSkip, b))
                {
                    // dedup against rescans so the count stays honest. Once the
                    // table fills we stop counting rather than inflate on rescans;
                    // low guards are noise anyway (MemGuardAll=true lists them).
                    if (g_nGuardSkip >= MAX_GUARDSKIP)
                    {
                        if (!g_guardSkipFull)
                        {
                            g_guardSkipFull = 1;
                            Write(L"[MEM]", L"low-address guard table full (1024 distinct pages); further low guards not tracked - MemGuardAll=true lists them");
                            CsvRow(L"guardpage-summary", L"low", L"table full at 1024 distinct pages", L"");
                        }
                    }
                    else
                    {
                        g_guardSkip[g_nGuardSkip++] = b;
                        g_guardSkipped++;
                        if (g_guardSkipped == 1 || (g_guardSkipped % 512) == 0)
                        {
                            WCHAR msg[300];
                            StringCchPrintfW(msg, 300, L"%d low-address guard/no-access pages tracked (thread-stack/heap guards; MemGuardAll=true lists them)",
                                             g_guardSkipped);
                            Write(L"[MEM]", msg);
                            CsvRow(L"guardpage-summary", L"low", msg, L"");
                        }
                    }
                }
            }
        }
        addr = b + (ULONG_PTR)mbi.RegionSize;
        if (mbi.RegionSize == 0) break;
    }
}

// Read-only survey of the manually-mapped shadow DLLs (are their exports
// trampolines into the protected image?) and a prologue decode of executable
// non-image regions (the Denuvo stub pages). Never writes anything.
static int StubDest(const BYTE* fn, int* kind, void** destOut)
{
    if (fn[0] == 0xFF && fn[1] == 0x25)
    {
        LONG d32; void* d = NULL;
        memcpy(&d32, fn + 2, sizeof(d32));
        __try { memcpy(&d, fn + 6 + d32, sizeof(d)); }   // alignment-safe read
        __except (EXCEPTION_EXECUTE_HANDLER) { d = NULL; }
        if (!d) return 0;
        *kind = 1; *destOut = d; return 1;
    }
    if (fn[0] == 0xE9)
    {
        LONG rel; memcpy(&rel, fn + 1, sizeof(rel));
        *kind = 2; *destOut = (void*)(ULONG_PTR)(fn + 5 + rel); return 1;
    }
    return 0;
}

static void ShadowSurvey(int first)
{
    HANDLE snap;
    WCHAR line[1200];
    if (!ScanShadow) return;
    RefreshModuleRanges();
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, g_pid);
    if (snap != INVALID_HANDLE_VALUE)
    {
        MODULEENTRY32W me; me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me))
        {
            do
            {
                BYTE* base = (BYTE*)me.hModule;
                MEMORY_BASIC_INFORMATION mb;
                IMAGE_DOS_HEADER* dos; IMAGE_NT_HEADERS* nth;
                int shownM = 0, cappedM = 0, seen = 0, s;
                if (!base) continue;
                if (!VirtualQuery(base, &mb, sizeof(mb))) continue;
                if (mb.Type == MEM_IMAGE) continue;  // loader images aren't shadow copies
                // loader images are not shadow copies; ntdll's proxy is covered by
                // the dedicated ntdll survey (skip it here to avoid noise)
                if (_wcsicmp(me.szModule, L"ntdll.dll") == 0) continue;
                for (s = 0; s < g_nShadowSeen; s++)
                    if (g_shadowSeen[s] == (ULONG_PTR)base) { seen = 1; break; }
                if (seen) continue;                 // rescan: already reported
                __try
                {
                    IMAGE_EXPORT_DIRECTORY* exp;
                    DWORD i, nf, nfn, expSize;
                    int out = 0, rel = 0, iat = 0, other = 0, same = 0;
                    DWORD rva;
                    dos = (IMAGE_DOS_HEADER*)base;
                    if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
                    nth = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
                    if (nth->Signature != IMAGE_NT_SIGNATURE) continue;
                    if (g_nShadowSeen < MAX_SHADOWSEEN) g_shadowSeen[g_nShadowSeen++] = (ULONG_PTR)base;
                    rva = (DWORD)nth->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
                    expSize = nth->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
                    if (!rva) 
                    {
                        StringCchPrintfW(line, 1200, L"shadow module %s @0x%llX: no exports (data/region)", me.szModule, (unsigned long long)(ULONG_PTR)base);
                        Write(L"[SCAN]", line); CsvRow(L"shadow", me.szModule, L"no-exports", L"");
                        continue;
                    }
                    exp = (IMAGE_EXPORT_DIRECTORY*)(base + rva);
                    nf = exp->NumberOfFunctions;
                    for (i = 0; i < nf && i < 20000; i++)
                    {
                        DWORD fr = ((DWORD*)(base + exp->AddressOfFunctions))[i];
                        const BYTE* fn; int k; void* d = NULL;
                        WCHAR dest[130];
                        if (!fr) continue;
                        if (fr >= rva && fr < rva + expSize) continue;   // forwarder string
                        fn = base + fr;
                        if (!StubDest(fn, &k, &d)) { other++; continue; }
                        out++;
                        CensusDescribe(d, dest, 130);
                        if (d >= (void*)base && (BYTE*)d < base + nth->OptionalHeader.SizeOfImage) { same++; continue; }
                        if (k == 1) iat++; else rel++;
                        if (shownM < ShadowDetailMax)
                        {
                            WCHAR nm[80];
                            const char* pn = ScanNameOfFn(exp, base, i);
                            shownM++;
                            if (pn) MultiByteToWideChar(CP_ACP, 0, pn, -1, nm, 80); else StringCchPrintfW(nm, 80, L"ord%u", i);
                            StringCchPrintfW(line, 1200, L"shadow survey: %s @0x%llX!%s -> %s (%s)",
                                             me.szModule, (unsigned long long)(ULONG_PTR)base, nm, dest,
                                             k == 1 ? L"FF25 abs jmp" : L"E9 rel jmp");
                            Write(L"[SCAN]", line);
                            CsvRow(L"shadow-export", me.szModule, nm, dest);
                        }
                        else cappedM++;
                    }
                    StringCchPrintfW(line, 1200, L"shadow survey summary: %s @0x%llX %u exports - %d redir%s (%d IAT-jmp, %d rel-jmp, %d same-image), %d code, %d forwarder",
                                     me.szModule, (unsigned long long)(ULONG_PTR)base, nf, out, out == 1 ? L"" : L"s", iat, rel, same, other, nf - out - other);
                    Write(L"[SCAN]", line);
                    CsvRow(L"shadow-summary", me.szModule, line, L"");
                    if (cappedM)
                    {
                        StringCchPrintfW(line, 1200, L"shadow survey: %s: %d further redirect%s not listed (ShadowDetailMax=%d)",
                                         me.szModule, cappedM, cappedM == 1 ? L"" : L"s", ShadowDetailMax);
                        Write(L"[SCAN]", line);
                        CsvRow(L"shadow-detail-capped", me.szModule, line, L"");
                    }
                    nfn = 0; (void)nfn;
                }
                __except (EXCEPTION_EXECUTE_HANDLER) { Trace(L"shadow survey fault"); }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }

    // prologue decode of the executable non-image regions (Denuvo stub pages)
    {
        int i;
        for (i = 0; i < g_nExec && i < 400; i++)
            DecodeExecPrologue(g_exec[i]);
    }
}

// Decode the first bytes of an executable non-image region: a jump stub's
// destination says where that region forwards (shadow DLL, unlinked code).
static void DecodeExecPrologue(ULONG_PTR a)
{
    MEMORY_BASIC_INFORMATION m2;
    if (!ScanShadow) return;
    if (!VirtualQuery((LPCVOID)a, &m2, sizeof(m2))) return;
    if (!(m2.Protect & 0xF0)) return;
    __try
    {
        const BYTE* fn = (const BYTE*)a;
        WCHAR dest[130], msg[600];
        int k; void* d = NULL;
        if (StubDest(fn, &k, &d))
        {
            CensusDescribe(d, dest, 130);
            StringCchPrintfW(msg, 600, L"exec region @0x%llX prologue %02X %02X %02X %02X %02X -> %s (%s)",
                             (unsigned long long)a, fn[0], fn[1], fn[2], fn[3], fn[4], dest,
                             k == 1 ? L"FF25 abs jmp" : L"E9 rel jmp");
        }
        else
            StringCchPrintfW(msg, 600, L"exec region @0x%llX prologue %02X %02X %02X %02X %02X (not a jump stub)",
                             (unsigned long long)a, fn[0], fn[1], fn[2], fn[3], fn[4]);
        Write(L"[MEM]", msg);
        CsvRow(L"exec-prologue", L"", msg, L"");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// ---------------------------------------------------------------- worker
static DWORD WINAPI WorkerThread(LPVOID param)
{
    UNREFERENCED_PARAMETER(param);
    GetSystemTimeAsFileTime(&g_startTime);
    g_pid = GetCurrentProcessId();
    InitializeCriticalSection(&g_wrLock);
    g_wrLockInit = 1;
    g_evBuf = (GCEv*)HeapAlloc(GetProcessHeap(), 0, sizeof(GCEv) * (size_t)g_evBufN);
    RingInit();

    Trace(L"worker started");
    LoadSettings();
    Trace(L"settings loaded");
    // Time-to-hooks is the whole game here. The loader (winmm.dll) reads
    // artifact.json and then loads dse.dll, dse switches driver signature
    // enforcement off, artifact starts the hypervisor, dse switches it back on -
    // all before the game proper runs (user-confirmed). On this box that whole
    // sequence fits in the first few tens of milliseconds of the process, so
    // everything that is not needed *by* the hooks is deferred until they are
    // in: the console allocation, the CSV header, the surveys. What must stay in
    // front of them is the saved log (so an early hook line is not lost), the
    // lock-free caller table (hard rule 6) and the exception capture.
    OpenLogFile();
    if (g_logHandle != INVALID_HANDLE_VALUE)
    {
        WCHAR msg[900];
        StringCchPrintfW(msg, 900, L"saving log to %s", g_logPath);
        Write(L"[INFO]", msg);
    }
    {
        // How long after the process was created did this worker get going? The
        // protection's own startup (load dse.dll, take DSE down, start the
        // hypervisor, put DSE back) happens around this point, so the delta says
        // whether we were in time to see any of it - and it is the number to
        // quote when the loader is rearranged in artifact.json.
        FILETIME ct, et, kt, ut;
        if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut))
        {
            ULARGE_INTEGER a, b;
            SYSTEMTIME st;
            ULONGLONG d;
            WCHAR msg[320];
            a.LowPart = ct.dwLowDateTime; a.HighPart = ct.dwHighDateTime;
            b.LowPart = g_startTime.dwLowDateTime; b.HighPart = g_startTime.dwHighDateTime;
            FILETIME lt;
            d = (b.QuadPart - a.QuadPart) / 10000ull;   // 100ns ticks -> ms
            if (!FileTimeToLocalFileTime(&ct, &lt)) lt = ct;
            FileTimeToSystemTime(&lt, &st);
            StringCchPrintfW(msg, 320,
                L"process created %02u:%02u:%02u.%03u - this worker started +%llu ms later",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, d);
            Write(L"[INFO]", msg);
        }
    }
    if (ScanDrivers) CodeIntegrityInfo();          // DSE baseline, right after the log opens
    // Build the lock-free caller table BEFORE any hook is installed: the hook
    // path names callers from this table and must never call the loader APIs.
    RefreshCallerRanges();
    Trace(L"caller ranges ready");
    InstallVehCapture();
    Trace(L"veh/crash capture installed");
    InstallModuleLog();
    Trace(L"module log installed");
    if (HookApis && HookDelayMs <= 0) { g_hooksInstalled = 1; InstallHooks(); Trace(L"hooks installed"); }
    else if (HookApis)
    {
        // Injected at instruction zero (EarlyLoad): hold every IAT write back
        // until the protection has finished its own startup, or it fails its
        // tamper check and the session dies with Denuvo's "something went wrong"
        // dialog. Read-only work is unaffected - it is already running.
        WCHAR msg[300];
        g_hooksAt = GetTickCount64() + (ULONGLONG)HookDelayMs;
        StringCchPrintfW(msg, 300,
            L"hooks: IAT writes deferred by HookDelayMs=%d ms - a protected module must not see its imports repointed before its own startup; read-only capture is already live",
            HookDelayMs);
        Write(L"[INFO]", msg);
    }
    // Hooks are in - now the deferred, slower setup can happen.
    SetupConsole();
    Trace(L"console ready");
    CsvOpen();
    if (g_csvHandle != INVALID_HANDLE_VALUE)
    {
        WCHAR msg[900];
        StringCchPrintfW(msg, 900, L"csv: %s", g_csvPath);
        Write(L"[INFO]", msg);
    }
    // Startup capture window: dse.dll holds DSE down only for as long as the
    // hypervisor takes to start, so for the next few seconds the state is
    // sampled every 10 ms instead of on the monitor cadence, and modules that
    // load after this DLL (dse.dll always does) are re-tried for hooking on the
    // same tight loop - that is the last chance to catch the SCM calls from
    // inside the boundary. Everything here is read-only and worker-side.
    {
        int k;
        for (k = 0; k < 300; k++)                  // ~3 s
        {
            if (ScanDrivers) CodeIntegrityInfo();
            if (k % 4 == 0) { HookLateModules(); ResolveCallerAddrs(); }
            Sleep(10);
        }
    }
    { WCHAR msg[900]; StringCchPrintfW(msg, 900, L"settings file: %s", g_iniPath); Write(L"[INFO]", msg); }
    if (ScanOtherHooks) { ScanNtdllHooks(1, ScanOtherHooksMax); Trace(L"hook scan done"); }
    if (ScanShadow || MonMemory) { SafeCall(PollMemory); Trace(L"memory baseline done"); }
    if (ScanShadow) { ShadowSurvey(1); Trace(L"shadow survey done"); }
    if (ScanLdr) { LdrSurvey(1); Trace(L"ldr survey done"); }
    if (RedirectCrt) { RedirectGameCrt(); Trace(L"crt redirected"); }
    {
        WCHAR msg[200];
        StringCchPrintfW(msg, 200, L"attached, PID %lu", g_pid);
        Write(L"[console]", msg);
    }
    if (ShowLegend)
    {
        Write(L"[console]", L"legend: [ODS module+off] OutputDebugString, named by caller | [x.log] tailed file");
        Write(L"[console]", L"[MOD] DLL load/unload | [CRASH]/[EXC] exceptions | [THR] threads | [MEM] new exec memory");
        Write(L"[console]", L"[NET] TCP | [FS] files | [HOOK] hooked Nt* calls | [HB] heartbeat | [raw] = written straight to the console");
    }
    Trace(L"entering log tail loop");
    TailLoop();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        g_hinst = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        {
            HANDLE t = CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
            if (t) CloseHandle(t);
        }
    }
    return TRUE;
}

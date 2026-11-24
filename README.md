# GameConsole

**One plain-C DLL** that opens a debug console inside a game process, captures output from
many sources, saves it to a log file, and can trace what a specific mod DLL does.

No .NET, no runtime, no helper DLLs. Everything — console, log file, monitors, hooks,
exception/OutputDebugString/crash capture, module notifications — lives in a single
native module that self-starts from `DllMain`. That matters because all of it can run on
arbitrary game threads: a managed runtime in that position crashed game hosts during
development (Native AOT's reverse-pinvoke thread attach), which is why this is C.

## Build

Requires Visual Studio with the C++ workload (no .NET SDK needed).

    build.cmd

Produces `GameConsole.dll` (~250 KB, x64).

## Deploy

Run `deploy.cmd` (edit its `GAME=` line if your game folder differs). It copies
`GameConsole.dll` into the game folder.

The game's loader must load it. For a loader that reads a JSON list of DLLs and only
calls `LoadLibraryW`:

    "load_dlls": ["gameconsole.dll", "dse.dll"]

`GameConsole.dll` self-starts: its `DllMain` spawns a worker thread that does all the
real work, so nothing is done under the loader lock. There is no exported `Init`.

## Settings

`GameConsole.ini` next to the DLL. Created from defaults if missing; **missing keys are
appended** to your existing file automatically on startup. Edit and restart the game.
The defaults below are the `Opts[]` table in `GameConsole.c`; the ini shipped in the
repo overrides four of them: `HoldOnCrash=false`, `HookModules=artifact.dll,dse.dll`,
`MaxHookReports=10000`, `CensusAllMax=1000`.

### Console window

| switch | default | meaning |
|---|---|---|
| `ConsoleVisible` | `true` | Show the console window. `false` = hidden (the saved log file keeps working) |
| `AllocConsole` | `true` | `false` = no console at all (log-file-only mode); diagnostic for compat-shim conflicts |
| `ConsoleTitle` | `Game Debug Console` | Title of the console window |
| `ConsoleCols` | `300` | Console buffer width, 80-1000; wide = long lines do not wrap in the saved log |
| `ConsoleRows` | `20000` | Console buffer height, 200-30000; when nearly full it is wiped after saving |
| `ShowTimestamps` | `true` | Prefix tagged lines with `HH:mm:ss.fff` |
| `ShowColors` | `true` | Colour the tags (ANSI codes) |
| `ShowLegend` | `true` | Print the tag legend at startup |
| `ShowStartupInfo` | `true` | Print exe path, command line, working directory and OS at startup |
| `HoldOnCrash` | `true` | On an unhandled crash, freeze the process so the console stays readable. The shipped ini sets `false`: a held process looks exactly like a hang |

### Saving

| switch | default | meaning |
|---|---|---|
| `SaveLogFile` | `true` | Save the console to a file |
| `LogDirectory` | *(empty)* | Folder for saved logs; empty = a `logs` folder next to `GameConsole.dll` |
| `SaveTaggedLines` | `true` | Save lines written by GameConsole (the ones with a `[tag]`) |
| `SaveRawLines` | `true` | Save lines written straight to the console by the game or mods (marked `[raw]`) |
| `WriteTraceFile` | `true` | Write `GameConsole.log` in `%TEMP%` (startup steps, handy if something fails to start) |
| `WriteCrashFile` | `true` | Write `GameConsole_crash.log` in `%TEMP%` (crash reports) |
| `CsvExport` | `true` | Write a machine-readable `GameConsole_<pid>.csv` next to the log (census/survey/hook/mailbox rows) |

### Capture (listen or redirect output only, nothing is patched)

| switch | default | meaning |
|---|---|---|
| `CaptureOutputDebugString` | `true` | Listen for `OutputDebugString` calls |
| `RedirectCrt` | `true` | Point the game's `printf`/stdout/stderr (UCRT) at the console |
| `RedirectStdHandles` | `true` | Point the process standard handles at the console (Win32 writes to stdout/stderr) |
| `EnableCrashHandler` | `true` | Install an unhandled-exception filter that logs the final crash |

### Categories (`false` hides the category from the console **and** the saved file)

| switch | default | tags |
|---|---|---|
| `Show_ODS` | `true` | `[ODS]` OutputDebugString text |
| `Show_LogFiles` | `true` | `[<file.log>]` tailed log files, **and** `[SCAN]` — `CategoryOn` falls through to this gate for any tag without its own switch |
| `Show_Modules` | `true` | `[MOD]` DLL load/unload |
| `Show_Crash` | `true` | `[CRASH]` access violations, stack overflows and similar |
| `Show_Exceptions` | `true` | `[EXC]` other exceptions (also gates `[EH]`) |
| `Show_Threads` | `true` | `[THR]` threads |
| `Show_Memory` | `true` | `[MEM]` new executable memory |
| `Show_Network` | `true` | `[NET]` TCP connections |
| `Show_FileSystem` | `true` | `[FS]` files created/deleted/renamed |
| `Show_Heartbeat` | `true` | `[HB]` periodic counts |
| `Show_SharedData` | `true` | `[SD]` KUSER_SHARED_DATA changes (the Dr-armed mailbox) |
| `Show_System` | `true` | `[SYS]`/`[KRNL]`/`[SVC]` hypervisor, kernel modules and services |
| `Show_Info` | `true` | `[INFO]` startup information |
| `Show_Console` | `true` | `[console]` GameConsole's own status messages |
| `Show_Hooks` | `true` | `[HOOK]` hooked Nt* API activity (file reads/writes, memory-protection changes) |

### OutputDebugString, log files

| switch | default | meaning |
|---|---|---|
| `OdsShowCaller` | `true` | Name the module that called `OutputDebugString` in the tag |
| `TailLogFiles` | `true` | Follow `*.log` files in the game folder while the game runs |
| `TailUnityPlayerLog` | `true` | Also follow Unity `Player.log` under AppData LocalLow |
| `TailTxtFilesWithLogInName` | `true` | Also follow `.txt` files with `log` in the name |
| `TailSkipExistingContent` | `false` | `true` = only show lines added after the file was first seen |
| `LogScanDepth` | `3` | How many subfolder levels under the game folder to search (0-10) |

### Modules

| switch | default | meaning |
|---|---|---|
| `ModulesPresent` | `true` | List the modules already loaded at startup |
| `ModulesLoaded` | `true` | Log DLLs as they load |
| `ModulesUnloaded` | `true` | Log DLLs as they unload |
| `ModulesHideSystem` | `false` | Hide DLLs from the Windows folder (shows only game and mod DLLs) |
| `ModulesShowPath` | `true` | Show the full path of each present module |

### Exceptions

| switch | default | meaning |
|---|---|---|
| `LogAllExceptions` | `true` | Log non-crash exceptions (C++ throws, breakpoints and so on) |
| `ExceptionCrashCodes` | `true` | Log access violations and similar when they are thrown (first chance) |
| `ExceptionStackTrace` | `true` | Include a stack trace in crash reports |
| `ExceptionCpp` | `true` | Include C++ throws |
| `ExceptionClr` | `true` | Include .NET CLR exceptions |
| `ExceptionBreakpoints` | `true` | Include breakpoints and single-steps |
| `ExceptionOther` | `true` | Include any other exception code |
| `MaxCrashReports` | `25` | Cap on first-chance crash-code reports (some are harmless) |
| `MaxOtherExceptions` | `500` | Cap on other exception reports |

### Threads, memory, network, files, heartbeat

| switch | default | meaning |
|---|---|---|
| `MonThreads` | `true` | Watch threads |
| `ThreadsPresent` | `true` | List threads that already exist at startup |
| `ThreadsStarted` | `true` | Log new threads with their start module |
| `ThreadsExited` | `true` | Log threads that exit |
| `MonMemory` | `true` | Watch for executable memory not owned by any DLL or EXE |
| `MemoryBaseline` | `true` | Print how many such regions exist at startup |
| `MonNetwork` | `true` | Watch this process's IPv4 TCP connections |
| `NetNew` | `true` | Log new connections |
| `NetChanges` | `true` | Log connection state changes |
| `NetClosed` | `true` | Log connections that close |
| `MonGameFolder` | `true` | Watch the game folder (recursive) for created/deleted/renamed files |
| `MonTempDir` | `true` | Watch the `%TEMP%` folder too |
| `FsCreated` | `true` | Log created files and folders |
| `FsDeleted` | `true` | Log deleted files and folders |
| `FsRenamed` | `true` | Log renamed files and folders |
| `MonHeartbeat` | `true` | Print thread/handle/memory counts periodically |
| `HeartbeatSeconds` | `10` | Seconds between heartbeats (1-3600); the heartbeat drives the periodic rescans and monitors |

### Hooks (opt-in: instrument import tables + the KERNELBASE interposition layer to log them — no code bytes are ever patched)

| switch | default | meaning |
|---|---|---|
| `HookApis` | `true` | Install the hook layer; `false` = a purely passive, survey-only session |
| `HookFileReads` | `true` | Log `NtReadFile` calls (file reads) |
| `HookFileWrites` | `true` | Log `NtCreateFile`/`NtOpenFile`/`NtWriteFile` calls |
| `HookMemPatches` | `true` | Log `NtProtectVirtualMemory` calls when execute rights are involved (code patching) |
| `HookOnlyGameModules` | `true` | `true` = skip callers inside the Windows folder (game/mod DLLs only) |
| `HookModules` | *(empty)* | Comma-separated module names whose IAT is instrumented, e.g. `artifact.dll`. Empty = no per-module IAT hooks, but the KERNELBASE interposition layer is still installed and logs all callers |
| `HookExe` | `false` | Also instrument the game executable's own import table. OFF by default: a protected exe checks its own imports, and repointing them can fail its tamper check (`Corrupt pak index detected`) |
| `HookHiddenModules` | `false` | Instrument the import table of a module that is in no loader list - found only by caller address, e.g. a protection helper DLL. OFF by default: such a module hides itself for a reason, and repointing its imports while it runs has ended the game at that exact instant on every launch that reached it. The module is still detected, censused and reported when this is false |
| `HookDelayMs` | `0` | Wait this many ms after startup before the first IAT write. `0` = install immediately (normal - the mod loader brings the DLL in). Set it (e.g. `8000`) when the DLL is injected at instruction zero with `tools\EarlyLoad.c`: a protected module that finds its import table repointed *before* its own startup fails its tamper check and aborts the session ("something went wrong" + a support.codefusion.technology link). Everything read-only - surveys, DSE sampling, module log - still runs from t=0 |
| `MaxHookReports` | `2000` | Cap on hook log lines so a chatty caller cannot flood the console |
| `GpaThunks` | `true` | `GetProcAddress` calls through `HookModules`' import table are logged AND return a logging thunk (counts + last caller, reported each heartbeat). `false` if a game verifies the returned pointer |

### Read-only surveys

| switch | default | meaning |
|---|---|---|
| `ScanOtherHooks` | `true` | Report every ntdll export whose entry is a jump instead of a clean syscall stub (hooked by another module / alias / non-export cave). Runs at startup and each heartbeat (new sites only) |
| `ScanOtherHooksMax` | `400` | Max `[SCAN]` detail lines per image per survey |
| `ShadowDetailMax` | `24` | Shadow-DLL survey: max export-redirect detail lines per shadow module (per-module summary is always written) |
| `CensusAll` | `true` | Binding census of every loaded module's import table; details imports not bound to loader-loaded images (Denuvo-style manual-mapped copies). Startup and late-loading modules |
| `CensusAllMax` | `300` | Cap on detail lines from the all-module census |
| `ScanShadow` | `true` | Survey manually-mapped shadow DLLs' exports (trampolines into the protected image?) plus a prologue decode of executable non-image regions |
| `ScanLdr` | `true` | Walk the PEB loader list read-only; report modules toolhelp32 does not show and entries pointing at non-`MEM_IMAGE` memory |
| `ScanEh` | `true` | Report the real VEH chain (who registered which handler, in what order) and log later VEH/UEH registrations |
| `ScanServices` | `true` | Enumerate Windows services read-only; report `ServiceWatch` matches and new ones that appear while the game runs |
| `ScanDrivers` | `true` | Enumerate loaded kernel drivers read-only; report `ServiceWatch` matches - the kernel/hypervisor component a protection service installs. Also samples the Code Integrity options, so driver-signature enforcement being switched off and back on is reported |
| `ServiceWatch` | *(vendor list)* | Comma-separated words matched case-insensitively against service names, display names, binary paths, kernel driver paths and the CPUID hypervisor vendor |
| `ServiceWatchAll` | `false` | Also list EVERY running service / loaded kernel driver, not just `ServiceWatch` matches (noisy) |
| `WatchSharedData` | `true` | Watch the last bytes of KUSER_SHARED_DATA (`0x7FFE0FE0..0x1000`) for changes |
| `MemGuardScan` | `true` | Report committed non-image pages carrying `PAGE_GUARD` or `PAGE_NOACCESS` (anti-debug tripwires) |
| `MemGuardAll` | `false` | Also list low-address (thread-stack/heap) guard pages — usually noise; the protection's tripwires in the high module range are always listed |

Missing keys are appended to an existing file with these defaults on startup;
unknown keys stay in the file and are ignored. `HoldOnCrash=true` is the one to
watch when copying an ini between machines: it turns an unhandled crash into a
process that looks hung.

## Output

- Console tags: `[INFO]`, `[MOD]`, `[THR]`, `[MEM]`, `[NET]`, `[FS]`, `[HB]`,
  `[SD]`, `[ODS module+off]`, `[EXC module+off]`, `[EH]`, `[CRASH]`, `[HOOK]`,
  `[SYS]`, `[KRNL]`, `[SVC]`, `[SCAN]`, `[console]`, `[<file.log>]` for tailed
  log files.
- A machine-readable `GameConsole_pid<pid>.csv` is written next to the log
  (`CsvExport`).
- Lines the game or a mod writes straight to the console are marked `[raw]`.
- Saved log: `<dll folder>\logs\GameConsole_<time>_pid<pid>.log` (or `LogDirectory`).
  Every tagged line (ours) is appended to the file **directly** when it is written, so
  the log is immune to console-buffer wipes, cursor jumps and wrap re-scrapes. The
  console screen buffer is still scraped to relay the game's own console output as
  `[raw]` lines in order.
- `%TEMP%\GameConsole.log` — startup trace (handy when something fails to start).
- `%TEMP%\GameConsole_crash.log` — crash reports.
- Reading a run: `tools\RunReport.ps1` turns a log into a summary — hook surface,
  the driver/service install trace, loader-list tampering, hidden modules, the
  VEH chain, kernel/service matches and the census tallies. Pass several logs to
  compare sessions, or `-Full` for the matching lines.

## Hooks — what the mod actually does

Two modes, selected by `HookModules`:

- **`HookModules` names one or more DLLs** (e.g. `artifact.dll`) — **IAT mode**, the
  recommended one. GameConsole re-points entries in that module's **own import table**
  for its whole import surface: file IO (`CreateFileW`, `ReadFile`, `WriteFile`,
  `DeleteFileW`, `FindFirstFileExW`, `CopyFileExW`, and `NtReadFile`/`NtWriteFile`/
  `NtOpenFile` imports by ordinal), memory (`VirtualAlloc`/`Protect`/`Free`), processes
  (`CreateProcessW`, `ShellExecuteW`, `TerminateProcess`), threads (`SuspendThread`,
  `ResumeThread`, `SetThreadContext`, `GetThreadContext`), services (`CreateServiceW`,
  `StartServiceW`, `DeleteService`, `OpenSCManagerW`), the registry
  (`NtCreateKey`/`ZwCreateKey`/`NtSetValueKey`/`ZwSetValueKey`/`RegCreateKeyExW`/
  `RegSetValueExW` — this is how a driver's service key gets its `ImagePath`),
  `LoadLibraryA/W`,
  `GetProcAddress`, `MessageBoxW`, `IsDebuggerPresent`, `CreateDirectoryW`.
  **No code bytes are modified anywhere** — import tables are data — so this coexists
  safely with mods that inline-hook ntdll themselves.

- **KERNELBASE interposition layer (part of IAT mode).** Every Win32 file call in the
  process funnels through KERNELBASE's own imports of `NtCreateFile`/`NtOpenFile`/
  `NtReadFile`/`NtWriteFile`/`NtProtectVirtualMemory` — i.e. through the trampolines of
  any mod that has inline-hooked those ntdll stubs. Hooking those import entries shows
  the traffic being interposed, without touching a single ntdll byte. The monitored
  module's own calls are never sub-capped; the KERNELBASE-layer create/open/read/write
  lines share a sub-cap of `MaxHookReports/4` so bulk pak IO cannot drown the rare
  events. No-op protects (`old == new`) are skipped.

  The layer also interposes the KERNELBASE imports that make up the anti-debug
  and mutual-probing surface, logged unconditionally as `[HOOK]` lines. Hooking
  them at the KERNELBASE IAT means they are caught even when a caller reaches
  the syscall API directly instead of through kernel32:

  | line | API | what to look for |
  |---|---|---|
  | `qip class=N` | `NtQueryInformationProcess` | class **7** = `ProcessDebugPort`, **30** = `ProcessDebugObjectHandle`, **31** = `ProcessDebugFlags` — debugger probes |
  | `sit class=0x11` | `NtSetInformationThread` | class **0x11** = `ThreadHideFromDebugger` |
  | `qit class=N` | `NtQueryInformationThread` | thread-info reads (`ThreadHideFromDebugger` read-back, start address) |
  | `sip class=N` | `NtSetInformationProcess` | debug-port/flags **writes** (class 0x1E/0x1F) |
  | `cthex flags=N` | `NtCreateThreadEx` | create-flag bit **0x4** = hide-from-debugger thread |
  | `avm prot=N` | `NtAllocateVirtualMemory` | who reserves executable memory (`prot=0x40`) |
  | `ntsetctx Dr0..7` | `NtSetContextThread` | debug-register arming/scrubbing via the **syscall** path (with the Dr values) |
  | `ntgetctx Dr0..7` | `NtGetContextThread` | who **reads** the debug registers back (a tamper check) |
  | `rvm` / `wvm` | `NtReadVirtualMemory` / `NtWriteVirtualMemory` | **who reads/writes whose memory** (cross-process scans) |
  | `qvmm` | `NtQueryVirtualMemory` | memory-layout probing |
  | `openthr` | `NtOpenThread` | opening another thread (to read/alter its context) |

  The monitored module's own import table additionally hooks `VirtualQuery`,
  `VirtualQueryEx` and `ReadProcessMemory`, so its own memory-walking shows as
  `vquery` / `vqueryex` / `readproc` lines (address, resulting region, caller),
  plus the exception-handler registration APIs:
  | line | API | meaning |
  |---|---|---|
  | `veh add first\|last` | `AddVectoredExceptionHandler` / `RtlAddVectoredExceptionHandler` | a handler was registered; the **handler's module+offset** and the caller are printed |
  | `veh remove` | `RtlRemoveVectoredExceptionHandler` | a handler was unregistered (a protection hiding its handler) |
  | `veh add continue` / `veh remove continue` | `RtlAddVectoredContinueHandler` / `RtlRemoveVectoredContinueHandler` | the continuation chain (runs after everything else) |
  | `ueh set` | `SetUnhandledExceptionFilter` | the top-level filter was replaced; the **previous** filter is printed too |
  | `regkey CREATED` | `NtCreateKey` | a registry key was **created** (not just opened) — who installed this |
  | `regset` | `NtSetValueKey` | a registry value was written: key path (resolved with `NtQueryKey`), value name, type, size and data |
  | `syst` | `NtSetSystemInformation` | a system-wide information *write* (rare): class, length and the first dword of the payload |

  Two ntdll probes that a protection imports **directly** (so the KERNELBASE
  layer never sees them, because it watches KERNELBASE's own imports) are hooked
  in the module's import table as well:

  | line | API | what it reveals |
  |---|---|---|
  | `qit(iat) class=N` | `NtQueryInformationThread` | thread-info reads — start address, hide-from-debugger read-back |
  | `qvif class=N serial=… dev=… fs="…"` | `NtQueryVolumeInformationFile` | **hardware fingerprinting**: volume serial number and creation time, device type/characteristics, file-system name |

  `NtQueryInformationThread` is hooked in two places (the KERNELBASE layer and
  the module's own import table), so the tag says which one saw the call: a bare
  `qit` line came through KERNELBASE, `qit(iat)` can only come from the slot
  inside the monitored module. That is how an installed-but-never-fired module
  hook is visible instead of being assumed to work.

  Several probe lines now carry the **data**, not just the call: `qip`/`qit`
  append the first 16 bytes the OS returned, `qvmm` decodes the
  `MEMORY_BASIC_INFORMATION` it got back (`base/size/state/prot/type` - private
  versus image, committed versus free, executable or not, which is how a
  protection tells its own manual-mapped copy from a real module), `rvm` shows
  what was read out of the target process and `wvm` shows the bytes being
  written into it. That is the "what came back / what was patched" half of the
  trace; the caller is on every line.

  Large cross-process writes get more than 16 bytes: the first 12 writes of
  128 B - 256 KB each print the bytes **from the first non-zero byte onwards**
  (three 96-byte `wvm ... blob from+0x208 part 1/3 [hex]` lines - chunked
  because one log line has a fixed width). The zero-skip matters:
  injected blobs usually start with hundreds of zero bytes, so a preview from
  offset 0 would show nothing. Once the budget is spent a single
  `previews: no more wide wvm dumps (cap 12 blobs)` line says so, and later
  writes fall back to the 16-byte preview. When the caller cannot be named yet
  (a hidden module that has not been attributed), the line reads
  `(caller filtered or not resolvable - address queued)` and the bytes are
  still shown - an observation is never dropped because attribution is
  missing.

  The registry pair is how a service or a **kernel driver is registered**
  (`ImagePath`, `Type`, `Start`), so a driver install reads:

  ```
  [HOOK] regkey st=0x00000000 CREATED "\REGISTRY\MACHINE\SYSTEM\ControlSet001\Services\hyperkd"  <- artifact.dll+0x27140
  [HOOK] regset st=0x00000000 "\REGISTRY\MACHINE\...\Services\hyperkd"\ImagePath type=2 size=78 = \SystemRoot\System32\drivers\hyperkd.sys  <- artifact.dll+0x271C0
  ```

  The KERNELBASE layer only sees the loader-loaded KERNELBASE image. A protection
  that routes its own calls through a manual-mapped `KERNELBASE#1`/`advapi32#1`
  copy would bypass it, so the **same registry calls are hooked inside the
  instrumented module's own import table** as well. When a call passes through
  both, only one line is printed (the module-side handler notices that the
  layer already reported it); when it only passes through the shadow copy, the
  module-side hook still records it.

  Be clear about what that can and cannot prove. Dumping artifact.dll's imports
  shows **no registry API at all** — its `advapi32` imports are the service
  control manager (`CreateServiceW`, `StartServiceW`, `OpenSCManagerW`,
  `OpenServiceW`, `ControlService`, `DeleteService`) and its `ntdll` imports are
  seven probes, not the registry pair. A service created through the SCM has its
  key written by **services.exe**, in that process, so `regkey`/`regset` for a
  driver install may never appear in the game's own log. The install is proven
  instead by `svc create`/`svc openmgr`/`svc start`, by the driver appearing in
  `[KRNL]`/`[SVC]`, and by the code-integrity transition below.

- **Hook targets that load later** (`HookModules`/`HookExe`). artifact/dse load
  *after* this DLL, so a name that cannot be resolved at startup is not dropped:
  it is logged once and retried each heartbeat —
  `(iat) dse.dll is not loaded yet - will retry each heartbeat` →
  `(iat) dse.dll loaded late - instrumented now (N imports)`. A module that is
  in **no** loader list at all (`dse.dll`) cannot be resolved by name, so its
  base is taken from the caller attribution: it is reported, censused and added
  to the caller table, and then it is **left alone** —
  `(iat) dse.dll is a hidden module (absent from toolhelp32) - reported but NOT
  instrumented (HookHiddenModules=false)`. `HookHiddenModules=true` restores the
  older behaviour of repointing its imports by address; that is off by default
  because on PB0 every launch that reached a hidden module died at exactly that
  point (hidden-module line → census → `HookExe`-style fatal error), and a run
  that kills the game is a failed run.

- **`HookExe`** (default **off**) adds the game executable's own basename to the
  instrumented list, so its file/registry/memory/exception-handler imports are
  covered. Leave it off for a Denuvo-protected exe: the protection checks its own
  import table, and repointing it makes it fail its own tamper check — which
  shows up as `Corrupt pak index detected` in Unreal titles (both PB0 and FFRS
  did this). The KERNELBASE interposition layer already covers calls that go
  through KERNELBASE without touching the protected image.

- **`HookModules` empty** - no per-module IAT hooks, but the **KERNELBASE
  interposition layer is still installed**, so calls from *any* module through
  KERNELBASE's imported `Nt*` functions are logged. This is the right setting for
  a game whose mod you do not know yet.

Hook activity and the per-API install lines appear as `[HOOK]` lines (dark magenta,
gated by `Show_Hooks`), capped by `MaxHookReports`. There is no separate hook log file;
hook handlers do no file I/O at all — they push events into a lock-free ring that the
worker thread drains, so nothing in the hooked path touches the disk or the console.

## ntdll survey — who has already patched ntdll (`[SCAN]`)

`ScanOtherHooks=true` (default) runs a **read-only** survey of every ntdll image in the
process (this game has **two** copies — one is a private/proxy image, see below). It
walks all exports and reports every one whose entry is a **jump instead of a clean
syscall stub** (`4C 8B D1 B8 …`), with the target resolved and **classified** — nothing
is filtered out, only labeled (`ntdll#<image>!<name>` when there is more than one copy):

- `hooked: FF25 abs jmp` / `hooked: rel jmp` → target is **another module**
  (`ntdll!NtCreateFile -> artifact.dll+0xF789`) — the classic inline hook. A jump into
  the *other* ntdll image is labeled as such.
- `same-image alias` → target is the entry of another ntdll export. Most of these are
  Windows' own folded/alias thunks (compare a clean process: ~50 entries at the time of
  writing), but they are reported so you can judge.
- `same-image NON-EXPORT target: cave or patched body` → the jump lands on code that is
  **not any export's entry** inside ntdll itself — exactly what a code-cave hook looks
  like. Also produced by OS thunk tables; resolve the offset before panicking.
- Targets where `GetModuleHandleEx` **fails** print as a bare `0x…` address — that
  region belongs to no loaded module, i.e. a **manual-mapped / unlinked image**.

If one ntdll copy has >80% of its exports forwarding into the other copy, it is a
**private proxy ntdll** (not OS behavior): it gets one summary line
(`ntdll#1 @0x…: 2400/2402 exports are jump-forwarders into ntdll@0x…`) and only its
forwarders that leave the ntdll pair (apphelp, unlinked regions) are listed in detail.

The survey runs at startup and again on each heartbeat; rescans report only **new**
sites, so hooks installed after startup are still caught — a summary line appears
whenever the total changes. It never writes to any page, so it cannot collide with
whatever installed the hooks.

### Binding census — which ntdll a module's imports actually use

For every module named by `HookModules`, before any of its imports are repointed, a
**read-only census** of its import table is emitted as `[SCAN]`:

- `binding census for <module>: N imports (U unbound) in D descriptor(s) across K targets
  (tally sums to N): KERNEL32.DLL x67, ntdll.dll x8 …` — one tally line per module; the
  `tally sums to` value is a cross-check that the walk didn't overrun (see below).
- Every import that resolved into an ntdll copy gets its own detail line naming the
  function, **which ntdll copy** (by `ntdll#<image>` and base address, so it can be matched
  against the proxy-copy summary above), and the bound offset
  (`binding census: artifact.dll!NtCreateFile -> ntdll#2 @0x… (bound ntdll.dll+0x…)`) —
  this answers whether a mod's `Nt*` ordinal imports go through the real ntdll or the
  private proxy copy.

### All-module census (`CensusAll`, default on)

`CensusAll=true` runs the same read-only census for **every loaded module** — not just
`HookModules` — at startup, and again later only for modules that appear after startup.
With this many modules the per-import detail would be noise, so the all-module pass
details only one thing: imports resolved into memory the OS loader did **not** map as an
image:

    binding census: <mod>!<import> -> <where> (bound to NON-IMAGE memory: manual-mapped copy or unlinked code)

That is the fingerprint of a Denuvo-style **manually mapped shadow DLL** (the loader's
`MEM_IMAGE` never appears for it) or of unlinked code — in a protected game this is how
you find out *which modules, if any, actually import through the private copies* rather
than the real system DLLs. `CensusAllMax` (default 300) caps these detail lines.

Two diagnostics are emitted when a module's import table is not what the loader wrote:

- `import descriptor #N has NO null terminator in its IAT array (4096 cap hit)` — a
  hand-crafted/corrupt import table; the walk spilled into neighboring memory, so counts
  near the end of that descriptor are not real imports. The `tally sums to N` value on
  the summary line lets you verify the tally against the import count at a glance.

### GetProcAddress logging thunks (`GpaThunks`, default on)

In IAT mode, `GetProcAddress` calls made by the monitored module are logged (name,
target DLL, caller), **and** the pointer handed back is a tiny logging thunk in
GameConsole's own RWX allocation: it records the caller's return address, counts the
call, and jumps to the real function. Nothing in the game's or system's code pages is
touched. Each heartbeat reports what the thunks saw:

    [HOOK] gpa    NtOpenFile called 3 (total 5) last caller artifact.dll+0x12AB0

so you see not just *what* a mod resolved dynamically but *whether and how often it
actually calls it, and from where*. Only code-looking targets (resolvable into an
executable section of a loaded image) are thunked; data exports are returned unchanged.
`GpaThunks=false` restores plain pass-through resolution — use it if a game verifies a
resolved pointer against its own tables, or in test harnesses that patch the resolved
pointer directly.

### Anti-debug and interaction capture

Beyond the API hooks, a few read-only captures target what a protected game and
an emu do *to each other*:

- **`[SD]` KUSER_SHARED_DATA mailbox watch** (`WatchSharedData`). artifact arms
  hardware breakpoint **Dr3 at `KUSER_SHARED_DATA+0xFF0`** repeatedly and never
  reads it back — a data address that never executes cannot be a working
  breakpoint, so the page may be a handshake. GameConsole snapshots
  `0x7FFE0FE0..0x1000` at startup and each heartbeat and logs a `[SD]` line with
  the hex bytes whenever it changes (`baseline …` once, then `old -> new`).

- **`[EXC]` single-step / breakpoint enrichment.** For `0x80000004` and
  `0x80000003` the log now carries the trap `rip`, `Dr6`, and the armed
  `Dr0–3`/`Dr7`, plus a `which=` field decoding `Dr6` bits 0–3 (which hardware
  breakpoint matched) — so a **fired** breakpoint is told apart from one merely
  armed. Same offsets as the `SetThreadContext` capture.

- **Guard / no-access pages** (`MemGuardScan`). Committed non-image pages with
  `PAGE_GUARD` or `PAGE_NOACCESS` are reported as `[MEM] guard page …` /
  `no-access page …` — the classic user-mode anti-debug tripwires. Pages in the
  high module range (`>= 0x7F0000000000`) and executable guards are always
  listed; low stack/heap guards — the vast majority — are only counted
  (`N low-address guard/no-access pages seen`) unless `MemGuardAll=true`.
  Deduped across rescans and capped.

- **Shadow-DLL and stub-page survey** (`ScanShadow`). Every manually-mapped
  module (the Denuvo shadow copies — the OS loader never mapped them, so
  `VirtualQuery` says non-`MEM_IMAGE`) has its export table walked read-only:
  `shadow survey: KERNEL32 @0x…!NtCreateFile -> <dest> (FF25 abs jmp)` plus a
  per-module summary, so you can see whether its exports are trampolines into
  the protected image and where each goes. Executable non-image regions get a
  prologue decode (`exec region @0x… prologue FF 25 … -> <dest>`), naming the
  Denuvo stub pages and where they forward. Never writes anything. Each shadow
  module gets its own detail quota (`ShadowDetailMax`, default 24) so one big
  module cannot starve the others; the summary line and a
  `… N further redirects not listed` note are always written, and modules that
  appear after startup are picked up by the rescan (no duplicate reports).

- **`[THR]` non-image start addresses.** A thread whose start address resolves
  to no image is marked `[non-image]` — those are the protection's own worker
  threads, separated from the game's.

- **PEB loader-list survey** (`ScanLdr`). Toolhelp32 walks the loader list with
  its own validation; a manually-mapped DLL can be invisible to it while
  `GetModuleHandle`/`GetProcAddress` still resolve it by name (the protection
  usually needs its own DLL findable). The survey walks all three real PEB lists
  — load-order, memory-order, init-order — read-only, each with cycle detection,
  and reports:
  `ldr ghost: NAME @0x… size … type … protect … - in the <list> loader list, NOT
  visible to toolhelp32`, `ldr non-image: …` for loader entries pointing at
  memory that is not `MEM_IMAGE`, `ldr: <list> cycles after N distinct entries`
  when a list has been relinked - reported the first time and then only when N
  changes, with the skipped rescans counted on the next change
  (`... (was 88, 3 unchanged rescan(s) not repeated) ...`); a list that stops
  cycling is reported too (`ldr: <list> no longer cycles (N distinct entries)
  - the relink is gone`), so the dedupe can never hide a state change - and a
  one-line `ldr survey: load-order N / memory-order M / init-order K …`
  summary on the first pass.
  A module that `GetModuleHandle` resolves but toolhelp32 cannot see is queued
  from the hook path and verified on the worker thread (taking a toolhelp
  snapshot inside a game thread can deadlock on the loader lock), then reported
  as `hidden module: NAME @0x… size … type … protect …`. This is how `dse.dll`
  was found writing into other processes while never appearing in
  `[MOD] present`. Read-only, never writes anything.

- **Exception-handler chain** (`ScanEh`). The process's real vectored-exception
  chain is a circular doubly-linked list and `RtlAddVectoredExceptionHandler`
  returns the node itself, so GameConsole registers a pass-through VEH and uses
  its own node as the anchor. The walk is read-only and reports every handler
  **in the order Windows calls them**:

  ```
  [EH] veh chain: 3 handler(s) - listed in the order Windows calls them
  [EH] veh chain[0] (first): artifact.dll+0x1A2C40
  [EH] veh chain[1]: D3D12Core.dll+0x2C8110
  [EH] veh chain[2] (last): GameConsole.dll+0x17810 [GameConsole]
  ```

  On current Windows builds the handler slot is stored as
  `RtlEncodePointer(handler)`, so the layout is discovered at runtime (the slot
  holding our own handler, the adjacent `LIST_ENTRY` whose links point back) and
  decoded with `RtlDecodePointer` — no hard-coded offsets, and the discovered
  layout is logged once. Changes are re-checked each heartbeat, with
  `veh removed:` for handlers that disappear. The chain is only walkable from our
  own node; continuation handlers are covered by the IAT hooks instead
  (`veh add continue` / `veh remove continue`).

- **Hypervisor and the protection's service/driver** (`ScanEh`, `ScanServices`,
  `ScanDrivers`). A protection that runs as a service — or ships a hypervisor or
  kernel driver — is visible without touching it:

  ```
  [SYS] hypervisor: PRESENT - cpu vendor "GenuineIntel", hypervisor vendor "Microsoft Hv", leaves 0x40000000-0x40000006, feature word 0x1
  [KRNL] driver: denuvo64.sys size 0x3A000 load-order 141 - kernel module matching ServiceWatch: \SystemRoot\System32\drivers\denuvo64.sys
  [SVC] service: DenuvoService "Denuvo Anti-Tamper" state=RUNNING pid=1832 type=0x10 controls=0x5 - "C:\Program Files\Denuvo\denuvo.exe"
  ```

  `[SYS]` reports `CPUID.1:ECX[31]` and the hypervisor vendor leaf, so a
  hypervisor-based protection (or Hyper-V/VBS underneath it) is named.
  `[KRNL]` lists loaded kernel modules via `NtQuerySystemInformation`
  (`SystemModuleInformation`) — names, sizes and load order are exact, while
  kernel base addresses are redacted by Windows for ordinary callers.
  `[SVC]` enumerates the service control manager read-only (name, display name,
  state, PID, binary path). All three match the same case-insensitive keyword
  list, `ServiceWatch` (default includes `denuvo`, `hypervisor`, `hvservice`,
  `vgk`/`vgc`, `vanguard`, `easyanticheat`, `battleye`, `nprotect`, `gameguard`,
  `xigncode`, `arxan`, `vmprotect`, `themis`, `xtrap`) plus the known Denuvo
  hypervisor driver names `hyperkd` and `simplesvm`, and re-check each
  heartbeat so a service that starts, stops or appears mid-session is reported.
  `ServiceWatchAll=true` lists everything instead of matches.

  There is also a **registry view of kernel drivers** (no elevation needed —
  the object manager's `\Driver` directory returns `STATUS_ACCESS_DENIED` to a
  normal game process), which answers "is it installed, and has it loaded yet":

  ```
  [SVC] installed driver: hyperkd type=0x1 start=demand image=\SystemRoot\System32\drivers\hyperkd.sys - matches ServiceWatch (NOT currently loaded)
  [SVC] installed driver services: 459 kernel/fs driver key(s) known, 1 match ServiceWatch (1 loaded)
  ```

  and a driver registered *while the game runs* (artifact/dse setting up the
  hypervisor after our DLL loaded) is reported whatever it is called:

  ```
  [SVC] driver service registered: <name> type=0x1 start=demand image=... - kernel driver service added while the game ran
  [SVC] installed driver: hyperkd ... (is now LOADED - the game started it)
  ```

  A driver key whose `ImagePath`/`Start` is rewritten mid-session is reported as
  `driver service key changed`. Note `NtLoadDriver`/`ZwLoadDriver` are not used
  by this protection (they are absent from artifact.dll, dse.dll and
  KERNELBASE), so the load goes through the service control manager and appears
  above — and, when dse.dll is instrumented, as its own `svc start` line.

  Because the protection hypervisor is not signed by a trusted publisher, the
  helper (`dse.dll`) has to **switch driver signature enforcement off**, let the
  service start, then switch it back on. That is the one step that leaves no
  user-mode call to hook, but its *effect* is readable: Windows exposes the Code
  Integrity options through
  `NtQuerySystemInformation(SystemCodeIntegrityInformation)`, and bit 0 of that
  value is `CODEINTEGRITY_OPTION_ENABLED` (a kernel driver must be signed). It is
  sampled at startup and on the driver-scan cadence, change-detected, and logged
  with decoded flags:

  ```
  [SYS] code integrity: options 0x0000C001 (enabled undecoded 0x6) - kernel-mode driver signing enforced (DSE on)
  [SYS] code integrity CHANGED 0x0000C001 -> 0x0000C000 (undecoded 0x6) - DSE was turned OFF (an unsigned kernel driver can load now)
  [SYS] code integrity CHANGED 0x0000C000 -> 0x0000C001 (enabled undecoded 0x6) - DSE was turned back ON
  ```

  An `enabled → off → enabled` pair around a `svc create`/`svc start` is the
  install observed end to end, and it is read-only (one query, no handles, no
  writes). Bits Windows does not document are printed as `undecoded 0x…` rather
  than guessed at, and a build where the query is refused says so once instead of
  staying silent.

  Catching that pair is a race against this DLL's own startup: the loader reads
  `artifact.json` and runs the whole sequence (dse.dll loads, DSE goes down, the
  hypervisor service starts, DSE comes back up) within the first few tens of
  milliseconds of the process. GameConsole therefore does the least possible
  before its hooks are in — settings, the log file, the lock-free caller table,
  exception capture, the module log, hooks — and only then allocates the console,
  opens the CSV and runs the surveys. After that a **startup window** samples the
  DSE state every 10 ms for about three seconds and re-tries hooking a module
  that loads after this DLL on the same tight loop (normally once per second), so
  `dse.dll` is instrumented within tens of milliseconds of loading and its
  `LoadLibraryA`/`GetProcAddress` resolutions — and through them the calls it
  makes — are logged. Outside the process, `tools\HostWatcher.c` can watch the
  same transitions from a second window, which also shows the teardown
  (service stopped / key removed, driver unloaded) that a log inside the game
  cannot survive to record.

- **CSV export** (`CsvExport`). A machine-readable `GameConsole_pid<pid>.csv`
  is written next to the log with one quoted row per census summary,
  non-image binding, shadow-survey finding, exec prologue, guard page, mailbox
  change, exception-handler, kernel-module, service and hook line — so the
  artifact↔denuvo map can be built offline.

- Calls the monitored module resolves dynamically (`GetProcAddress`) are logged, and with
  `GpaThunks=true` the returned pointer is a logging thunk — the *calls* through it are
  then visible too (`gpa` heartbeat lines). A pointer obtained some other way
  (walking `ntdll`'s export table directly) still bypasses everything.
- Calls made by raw `syscall` instructions bypass every user-mode hook.
- What a mod's own trampolines *change* about an intercepted call is not visible — the
  KERNELBASE layer shows the call arriving, not the rewritten result.
- Nothing before GameConsole's worker starts is captured (a mod's `DllMain` work runs
  before it). Use Sysinternals Process Monitor for that first window.
- File names for read/write come from `NtQueryInformationFile` and can be relative
  (no drive prefix).

### If a game crashes or hangs

Two failure modes were found the hard way and are now designed out — check these
first if a title misbehaves:

- **`Corrupt pak index detected` (Unreal) / the protection failing a tamper
  check.** The EXE's own import table was being repointed (`HookExe=true`). Set
  `HookExe=false` (the new default). The KERNELBASE interposition layer keeps
  working and does not touch the protected image.
- **The game hangs at startup, especially while loading DLLs.** Caller
  attribution must never take the loader lock: the hook path names callers from
  a plain table the worker keeps up to date, and anything it cannot name is
  queued by address for the worker to resolve. If you add a call to a hooked
  path, it may not use `GetModuleHandle*`, `GetModuleFileName*`, toolhelp or any
  allocation.

Bisect order if something still goes wrong: `HookExe=false` → `GpaThunks=false`
→ `HookApis=false` (surveys keep running, only the hook layer is off).

## Exceptions, OutputDebugString, crashes, module log

All native, all in the same ring: the vectored-exception handler, the unhandled-exception
filter and the `LdrRegisterDllNotification` callback run on arbitrary game threads and on
loader-lock threads (in the same broadcast as any compat-shim engine's `SE_DllLoaded`), so
they record events and return — never printing, never allocating. The worker formats
`[ODS module+off]`, `[EXC module+off]`, `[CRASH]` and `[MOD]` lines.

On an unhandled crash the filter writes the formatted report synchronously (console, and
`%TEMP%\GameConsole_crash.log`), so it survives a fast exit; `HoldOnCrash` freezes the
process so the console stays readable. Options: `LogAllExceptions`, `ExceptionCrashCodes`,
`ExceptionCpp`/`Clr`/`Breakpoints`/`Other`, `MaxOtherExceptions`, `MaxCrashReports`.

## Monitors

- `[MOD]` present / loaded / unloaded modules (with paths; `ModulesHideSystem` to cut noise)
- `[THR]` threads appearing and exiting, with the start address's module + offset
- `[MEM]` committed executable memory that belongs to no image: JIT, packers, injected code.
  At startup a **baseline census** lists every such region (address, size, protection,
  type) — the Denuvo private/manual-mapped image is expected to show up here; new
  regions appearing later are reported individually.
- `[NET]` this process's IPv4 TCP connections: new / state changes / closed
- `[FS]` files created / deleted / renamed in the game folder and `%TEMP%`
  (`ReadDirectoryChangesW`, so renames are seen as renames)
- `[HB]` heartbeat: threads / handles / working set
- `[x.log]` tailed log files (game folder recursive to `LogScanDepth`, plus Unity
  `Player.log` under `AppData\LocalLow` if enabled)

`SetThreadContext` calls are logged with the **debug-register contents of the CONTEXT
being written** (`Dr0–Dr7` + `ContextFlags`), so debug-register *scrubbing* (all zeros,
the common anti-debug move) is distinguishable from someone **arming hardware
breakpoints** (non-zero `Dr0–Dr3`/`Dr7` — that would be very interesting).

## Layout

    GameConsole.c        the whole tool (single DLL)
    build.cmd            builds GameConsole.dll
    deploy.cmd           copies it into the game folder
    GameConsole.ini      sample settings
    tools\               test hosts and diagnostics used during development
      SingleHost.c         loads GameConsole.dll and exercises every capture path
      ScanTest.c           the regression harness (what a good run must contain)
      VehProbe.c           raw lock-free-ring probe (event kinds, frames)
      symlookup.c/.exe     resolves image+offset to symbol via DbgHelp/PDB
      HostWatcher.c        external watcher: driver load/unload + DSE transitions,
                           for the part of the sequence that runs outside the game
                           process (start it before launching the title)
      EarlyLoad.c/.exe     starts the game suspended, injects GameConsole.dll, then
                           resumes - the DLL is mapped before instruction zero
                           (Denuvo titles detect the injection; see AGENTS.md)
      RunReport.ps1        summarises/compares a run log
      vehname.ps1          names a "[EH] veh chain" handler: section, export-table
                           honesty, probable function start, cross-image search
      pesect.ps1           PE section table with per-section entropy (packed or not)
      peimpnames.ps1       import/export names of a PE (e.g. a driver's kernel
                           touch-list)
      StressTest.c         legacy: multithreaded file-IO hammer (loads DLLs that
                           no longer exist - use ScanTest/SingleHost instead)
      TestHost.c           legacy: loader-order test (same caveat)
      CrashProbe.c         legacy: unhandled-crash probe (same caveat)

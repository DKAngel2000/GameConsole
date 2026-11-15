# AGENTS.md — working notes for GameConsole

Context for anyone (human or agent) picking this repository up. Read this before
changing behaviour; most of it was learned the hard way on real Denuvo titles.

## What this is

One plain-C DLL (`GameConsole.c` → `GameConsole.dll`) loaded by a game's mod
loader via `artifact.json` (`load_dlls`). It self-starts from `DllMain` (which
only spawns a worker thread) and then logs and classifies everything that
happens between the game/mod and the protection:

* a coloured, width-clipped copy on the console,
* a **plain-text log that is the artifact of record** (untruncated, no ANSI),
* a machine-readable CSV (`GameConsole_pid<pid>.csv`).

It is deliberately generic: everything is driven by `GameConsole.ini`, with no
game-specific code, so the same build can be pointed at another title.

## Hard rules (do not violate these)

1. **Never write to another module's code pages.** The only mutation the tool
   performs is rewriting *import tables* (IAT) and its own allocations. The
   legacy ntdll stub patcher (`HookLegacyNtdll`) is off by default and will race
   artifact.dll's own ntdll hooks into a crash — the opt-in exists only so a
   blanked ini can never silently enable it.
2. **All surveys are read-only.** `[SCAN]`, `[KRNL]`, `[SVC]`, `[EH]` walks read
   memory and enumerate OS state; they never write, allocate in another process,
   or take a handle to someone else's object. This is a Denuvo game: a survey
   that pokes at the protection is a bug, not a feature.
3. **Never filter silently.** Log it and classify it. Attribution is separate
   from observation — if something is skipped (a cap, a budget) say so in the
   log with the threshold.
4. **The saved log is authoritative.** Tagged lines are appended directly to the
   file inside `Write()`; the console is scraped only to relay the game's own
   output as `[raw]`. Never route tagged lines through the console buffer.
5. **No work under the loader lock.** `DllMain` spawns the worker and returns.
6. **The hook path must be lock-free. No loader APIs, no toolhelp, no
   `GetModuleHandleExW`/`GetModuleFileNameW`, no file I/O, no allocation.**
   Anything the hook path needs is either a plain read of a worker-maintained
   table (`g_crange` via `CallerRangeFor`) or queued for the worker
   (`NoteCallerAddr` → `ResolveCallerAddrs`, `HookAddPending` →
   `HookLateModules`). This is not a style rule: a loader thread holding the
   loader lock and waiting on a thread sitting in one of our hooks deadlocks,
   and the game **hangs** — which is exactly what happened when caller
   attribution was still calling `GetModuleHandleExW`.
7. **Keep the game stable.** A crash or a hang is a failed run. The apphelp
   `SE_DllLoaded+0x3ae` `0xC0000409` fast-fail at process exit is cosmetic and
   accepted; nothing else is.
8. **Do not instrument a protected exe's own imports by default.** Denuvo checks
   the EXE's import table; repointing it makes the protection fail its own
   tamper check, which surfaces as `Corrupt pak index detected` in Unreal
   titles. That is why `HookExe` defaults to **false** — the KERNELBASE
   interposition layer already covers calls that go through KERNELBASE, without
   touching the protected image.

## Build, deploy, validate

```
build.cmd                          # finds VS via vswhere, cl /LD /O2 /W3 -> GameConsole.dll
deploy.cmd                         # copy DLL+ini next to a game (see its arguments)
tools\ScanTest.c                   # the regression harness (see below)
tools\peimports.ps1 <pe-file>      # raw PE import dump: proves the binding census is complete
tools\VehProbe.c                   # standalone probe: how the Win11 VEH node layout was established
```

The user copies `GameConsole.dll` + `GameConsole.ini` into each game folder
themselves.

### Regression harness

Build `tools\ScanTest.c` to `%TEMP%\scantest\ScanTest.exe` and run it from that
directory with the DLL and an ini whose `HookModules=` is empty and
`GpaThunks=false`. The harness:

* loads `GameConsole.dll`,
* installs two real `FF25` detours on `ntdll!NtOpenFile` and
  `ntdll!NtQueryInformationFile` (allocating slots/trampolines within ±2 GB),
* registers a real VEH and UEH from the exe,
* loads `version.dll` **after** the DLL (late-load retry path),
* writes a registry value shaped like a driver `ImagePath`.

A good run has `exit=0` and all of:

```
ntdll#1!NtOpenFile -> ScanTest.exe+0x1000  (hooked: FF25 abs jmp)
[EH] veh chain: 1 handler(s) ...      then on the rescan: 2 handler(s)
[HOOK] (iat) version.dll is not loaded yet - will retry each heartbeat ...
[HOOK] (iat) version.dll loaded late - instrumented now (N imports)
[HOOK] regkey st=0x00000000 CREATED "..."   +   [HOOK] regset ... ImagePath type=.. = ...
[MEM] 1 low-address guard/no-access pages tracked (...)
[SCAN] ldr survey: load-order N / memory-order N / init-order N ...
```

…plus a log with **zero** `0x1B` (ESC) bytes, one guard-page summary line, and
`bindings tally` lines that sum correctly.

## Source map (`GameConsole.c`)

| area | functions / notes |
|---|---|
| ring + worker | `GCEv`/`EvPush`/`EvPop`, `RingInit`, `TailLoop`, `RunMonitors`, `SafeCall` |
| log/console/CSV | `Write`, `FormatTagLine`, `CategoryOn` (per-tag show gate), `CsvRow`, `ScrapeConsole` |
| ini | settings table (~`SettingsTable`), `B()`/`I()`/`V()` readers, `TrimToW` |
| exceptions | `VehNative`, `UehNative`, `DrainVehEvents`, `InstallVehCapture` |
| caller attribution | `DescribeCaller`, `DescribeModuleOf`, `RefreshModuleRanges`/`CensusDescribe` (shadow naming `NAME#k @base+off`) |
| IAT hook layer | `HookIatApi`/`HookIatName`/`HookIatByValue`, every `HkIat*`, `InstallIatHooks`, `HookOneModule`, `HookLateModules`, `InstallKernelbaseInterposition` |
| KERNELBASE interposition | 19 `Nt*` incl. file/memory/thread/context + `NtCreateKey`/`NtSetValueKey`; `HkIatKb*` |
| census | `CensusBindings`, `CensusAllModules` |
| surveys | `ScanNtdllHooks` (+`ScanNtdllImage`), `ShadowSurvey`/`DecodeExecPrologue`, `LdrSurvey`/`LdrWalkList`, `HiddenModulesVerify` |
| exception chain | `VehChainSurvey`, `VehFindLayout`, `VehHandlerOf`, `VehDescribe` |
| system view | `CpuTopologyInfo`, `DriversScan`, `InstalledDriversScan`, `ServicesScan` (all driven by `WatchMatch`/`ServiceWatch`) |
| monitors | threads, exec memory, network, filesystem watchers, heartbeat, guard pages, KUSER_SHARED_DATA mailbox |

## Settings (`GameConsole.ini`) — groups

* saving/log: `SaveLogFile`, `LogDirectory`, `SaveTaggedLines`, `SaveRawLines`, `WriteTraceFile`, `WriteCrashFile`, `CsvExport`
* capture: `CaptureOutputDebugString`, `RedirectCrt`, `RedirectStdHandles`, `EnableCrashHandler`
* show gates: `Show_*` (one per tag group, incl. `Show_System` for `[SYS]/[KRNL]/[SVC]`, `[EH]` shares `Show_Exceptions`)
* hooks: `HookApis`, `HookFileReads`, `HookFileWrites`, `HookMemPatches`, `HookOnlyGameModules`, `HookLegacyNtdll`, `HookModules`, `HookExe`, `MaxHookReports`, `GpaThunks`
* surveys: `CensusAll`, `CensusAllMax`, `ScanShadow`, `ShadowDetailMax`, `ScanLdr`, `ScanOtherHooks`, `ScanOtherHooksMax`, `ScanEh`, `ScanServices`, `ScanDrivers`, `ServiceWatch`, `ServiceWatchAll`, `WatchSharedData`, `MemGuardScan`, `MemGuardAll`

## Output tags

`[INFO]` `[MOD]` `[THR]` `[MEM]` `[NET]` `[FS]` `[HB]` `[SD]` `[ODS]` `[EXC]`
`[CRASH]` `[EH]` `[HOOK]` `[SCAN]` `[SYS]` `[KRNL]` `[SVC]` `[console]`
`[<file.log>]` (tailed logs) and `[raw]` (game's own console output).

Hook kinds seen on `[HOOK]` lines: `create/open/read/write/...` (file+memory),
`qip/sit/sip/qit/setctx/getctx/cthex/avm/rvm/wvm/qvmm/openthr` (anti-debug and
mutual probing), `veh add|remove`, `veh add continue`, `ueh set`, `regkey`
(CREATED only), `regset`, `syst` (`NtSetSystemInformation`), `svc
openmgr|create|start|delete`, `gpa`, `iat`.

## Domain knowledge from real runs (observational)

* **Two Denuvo re-bind styles.** FFRS routes the protected EXE's Win32 imports
  through one RWX thunk page (`0x7FF4FD430000`, `MEM_PRIVATE`, starts `MZ`);
  PB0/PZ2 point the EXE IAT straight into manual-mapped *shadow copies*
  (`KERNEL32#1`, `USER32#1`, `ntdll#1`, `GDI32#1`, `advapi32#1`, `CRYPT32#1`,
  `KERNELBASE#1`, `shlwapi#1`) that are `MEM_PRIVATE`, not `MEM_IMAGE`.
* **Shadow set** (same bases in all three titles): ntdll#1 `0x7FF4FDC90000`,
  KERNEL32#1 `0x7FF4FDA80000`, KERNELBASE#1 `0x7FF4FD870000`,
  USER32#1 `0x7FF4FD450000`, GDI32#1 `0x7FF4FD660000`,
  advapi32#1 `0x7FF4FD240000`, CRYPT32 `0x7FF4FCE20000`,
  SHLWAPI `0x7FF4FD030000`. PB0/PZ2 report a uniform fake `0x201000`
  `SizeOfImage`; FFRS reports real sizes. Never trust `SizeOfImage` here.
* **`artifact.dll`** is the mod: it carries the literal string `hyperkd`,
  forwards `ntdll#1!LdrGetDllHandle` and `RtlAddVectoredExceptionHandler` to
  itself, and starts the driver through the SCM. `apphelp.dll` takes
  `Nt*File`/`ZwSetValueKey` forwards (AppCompat shim).
* **`dse.dll`** is a hidden helper: in no toolhelp list and no PEB loader list,
  but resolvable by address; it does cross-process `NtWriteVirtualMemory`
  (2 writes per target into ~20 processes) and writes registry/service keys.
* **Hypervisor drivers**: `hyperkd.sys` (ships in `<game>\Artifact_Intel\`;
  PZ2 registers it under a service key called `artifact`, so match a service's
  *ImagePath basename*, never the key name, when asking "is it loaded"),
  `simplesvm.sys` (not seen on the dev machine), plus Microsoft's
  `hvservice.sys`. `NtLoadDriver`/`ZwLoadDriver` are imported by none of them, so
  the load goes through `CreateServiceW` + `StartServiceW` and the registry
  (`ImagePath`/`Type`/`Start`).
* **The hypervisor can be loaded without advertising itself**: on PZ2
  `hyperkd.sys` was loaded (load-order 252, service `artifact` RUNNING) while
  `CPUID.1:ECX[31]` was 0. So CPUID alone is not evidence — `[SYS]` is
  cross-referenced against the loaded `ServiceWatch` kernel modules for exactly
  this reason, and is re-probed each heartbeat so arming/unloading is caught.
* **artifact's VEH handler never appears in the OS chain**: with `ScanEh` the
  process VEH list contained only GameConsole's own handler, because the
  protection proxies `ntdll#1!RtlAddVectoredExceptionHandler` and keeps a
  private handler list. Registrations are only visible through the IAT hooks
  when the module doing them is instrumented (`HookModules`).
* **Anti-debug**: both PB0 and PZ2 run a self-probe single-step loop
  (byte-identical: `Dr6=FFFF0FF0`, `Dr7=0x440`, `rip=Dr3`), and probe
  `ProcessDebugPort` (class 0xC), `AmILastThread` (12), `ThreadHideFromDebugger`
  (0x11) etc. FFRS shows none of that; its KUSER_SHARED_DATA mailbox stays
  `…37133713`.
* **Crashes/exceptions** that are *not* the protection: FFRS's
  `steam_api64.dll+0x9C381B` throws `0xE06D7363` ~100–200× per run.

## Windows quirks this code accounts for

* There is no reliable *documented* user-mode hypervisor query on this build:
  `tools/SysInfoProbe.c` swept `NtQuerySystemInformation` classes 0x00–0x120 and
  none returned an unambiguous hypervisor structure (the classes that succeed
  are all zeros when no hypervisor is armed). Hypervisor presence is therefore
  reported from CPUID **plus** the loaded kernel-module list, not from an
  undocumented query — do not "fix" that by guessing a class number.
* Kernel base addresses are **redacted** for ordinary callers via
  `SystemModuleInformation` — `[KRNL]` reports names/sizes/load order and says so.
* The object manager's `\Driver` directory returns `STATUS_ACCESS_DENIED`
  (`0xC0000022`) to a game process, so installed drivers are enumerated from
  `HKLM\SYSTEM\CurrentControlSet\Services` instead (works without elevation).
* On current Windows builds a VEH node stores its handler as
  `RtlEncodePointer(handler)`; the layout (list links, handler slot, encoding) is
  discovered at runtime by `VehFindLayout` — do not hard-code offsets.
* `winternl.h`/`UNICODE_STRING` are not available under `WIN32_LEAN_AND_MEAN`;
  declare the equivalent struct locally (`GC_UNI`, `GC_USTR`).

## Gotchas already paid for

* Empty `HookModules` means **KERNELBASE interposition for all callers**, not
  "no hooks"; `HookExe` (off by default) additionally instruments the game exe's
  own IAT — and that is what triggers a Denuvo tamper check.
* Caller naming comes from `g_crange`, refreshed by the worker. A caller that
  is not in it is queued by address; the worker refreshes the table **before**
  deciding, otherwise an ordinary DLL that loaded on demand gets mislabelled
  "hidden module" (that happened with `NSI.dll`).
* A mod's helper DLL loads *after* this DLL: unresolved hook targets are logged,
  retried each heartbeat, and a hidden module is hooked **by address** from
  caller attribution.
* The guard-page scan must stay summarised (it once produced 481 lines/run).
* The shadow survey needs a **per-module** detail quota (`ShadowDetailMax`);
  a global budget let one module eat every line.
* Hidden-module detection must not run toolhelp inside a hooked game thread.
* Adding a new tagged line: add the tag to `CategoryOn` if it needs a show gate,
  emit the matching `CsvRow`, document it in the ini and in `README.md`.

## Conventions

* Plain C (MSVC, `/W3` clean), Windows-only, no external dependencies, no C++.
* Keep ini comments adjacent to the switch they document.
* `README.md` is the user-facing description: any new switch, tag or hook kind
  belongs there too.
* Build outputs (`*.dll`, `*.exe`, `*.obj`, logs, CSVs) are gitignored — this
  repository is the source of the tool, not the deployed artifact.

# AGENTS.md — working notes for GameConsole

Context for anyone (human or agent) picking this repository up, written as a
handover: state first, then the rules and the knowledge. Most of it was learned
the hard way on real Denuvo titles.

## Status / handover

**Repository** `https://github.com/DKAngel2000/GameConsole` (branch `main`,
remote `origin`). Source-only: `*.dll`, `*.exe`, `*.obj`, `logs/`, `*.log`,
`*.csv` are gitignored, so the built DLL is *not* in the repo — run `build.cmd`.
`.gitattributes` pins LF for text (`*.c/*.h/*.md/*.ini/*.ps1`, CRLF for
`*.cmd/*.bat`, binary for build outputs); `core.autocrlf=true` is set, so
`git ls-files --eol` should show `i/lf w/lf` for sources and docs, and
`i/lf w/crlf` for `*.cmd`/`*.bat` (checkout conversion, by design — not drift).

**Last behaviour-validated commit**: `aaa58ce` ("Log the hypervisor properly,
fix the loaded-driver check, drop legion") — ScanTest green and all three games
ran to a normal exit with it. ScanTest was re-run green at `69a499b` on
2026-11-16 (Win11 x64, VS 2022 Build Tools), so the harness half still holds at
the tip; the three-game half has **not** been re-run since `aaa58ce`. Update this
line whenever you validate a change, so the next session knows what the last
*proven* state was (`git log` is the tip; this line is about evidence, not
recency).

**Tooling on the old box (Windows 11, x64)**: MSVC via `vswhere` (no VS project
files), `git` with the Windows credential manager stored credential for GitHub.
There is **no `gh` CLI**, so PRs/issues/releases are not scripted here. Pushing
works with `git push origin main` (set `GIT_TERMINAL_PROMPT=0` so it fails fast
instead of blocking on a prompt). Shell note: the agent shell cannot parse
`<<'EOF'` heredocs — write commit messages to a temp file and use
`git commit -F <file>`. Before assuming you can build, check for `vswhere`:
a machine without the VS C++ tools installed cannot run `build.cmd` or the
harness, and the repo carries no binaries, so validation has to happen on a box
with MSVC (or after installing the Build Tools).

### Deployed state (old machine; paths are per-machine, configs are portable)

Three titles were instrumented, all with the **same ini recipe**:

```
HookModules=artifact.dll,dse.dll
HookExe=false
HookApis=true
GpaThunks=true
CensusAll=true
ScanOtherHooks=true
ScanShadow=true   ScanLdr=true   ScanEh=true
ScanServices=true ScanDrivers=true
ServiceWatch=denuvo,hypervisor,hvservice,hyperkd,simplesvm,vgk,vgc,vanguard,easyanticheat,battleye,beservice,bedaisy,nprotect,gameguard,xigncode,arxan,vmprotect,themis,xtrap
ServiceWatchAll=false
HeartbeatSeconds=10
HoldOnCrash=false        # true = Sleep(INFINITE) on an unhandled crash, which looks like a hang
```

| game | exe (folder) | notes |
|---|---|---|
| PB0 "Phantasy Blade 0" | `PBZ.exe` in `…\platform\steam\` | newer artifact: relinked loader lists, artifact's VEH visible in the OS chain |
| PZ2 "Planet Zoo 2" | `PlanetZoo2.exe` in the game root | Cobra engine + V8; newest artifact: relinked loader lists, private VEH list |
| FFRS "Final Fantasy Resonance" | `FFRS-Win64-Shipping.exe` in `…\FFRS\Binaries\Win64\` | Unreal; **older** artifact: clean loader lists, real `SizeOfImage`, no CPUID/hypervisor probing |

`deploy.cmd` copies `GameConsole.dll` into a game folder (edit its `GAME=` line;
default FFRS) and reminds that `artifact.json` must list `gameconsole.dll` in
`load_dlls`. The **ini is hand-maintained** in each game folder and is not copied
by the script. Logs land in `<game>\logs\` (`.log` + `.csv`).

### Outstanding / next (in priority order)

1. **Capture the driver *install*** — not yet observed, because `hyperkd` was
   already installed and loaded before every session so far. To provoke it:
   stop the game, delete `HKLM\SYSTEM\CurrentControlSet\Services\artifact`,
   reboot (to unload `hyperkd`), then run a title. Expected sequence:
   `[KRNL] driver image written to disk` → `[HOOK] regkey … CREATED
   "\REGISTRY\MACHINE\SYSTEM\…\Services\artifact"` → `[HOOK] regset …
   ImagePath/Type/Start` → `[HOOK] svc create` / `svc start` → `[SVC] driver
   service registered: …` → `is now LOADED` → `[KRNL] driver: hyperkd.sys`.
   Until then `regkey`/`regset`/`svc`/`syst` and the `[FS]` `.sys` classifier
   have **no** in-game validation.
2. **Hidden-module PE-size fix is unvalidated in a game**: `ImageSizeAt` now
   reports `SizeOfImage` instead of the first region (`dse.dll` showed `0x1000`)
   but no hidden module appeared in the last runs to exercise it.
3. **dse.dll detection is event-driven**: it is found only when caller
   attribution catches it calling something we instrument. The last runs had no
   `wvm` at all from it (earlier PB0 had 40 cross-process writes), so nothing was
   reported. A session where the protection acts is needed.
4. Optional polish: the `[SCAN] ldr: <list> cycles after N distinct entries`
   lines repeat once per rescan (that is the list size changing, e.g. 88→150→
   193→194 on PB0). Dedupe to first-report-plus-change if it becomes noisy.
5. Optional RE: name artifact's VEH handler for PZ2/FFRS. It is *not* in the OS
   chain (private list), so it needs a read-only hunt for the protection's own
   list — nothing yet.

### How to read a run (quick recipe)

```powershell
$c = Get-Content <log>
$c | Select-String '\[SYS\]|\[KRNL\]|installed driver:|driver service registered'
$c | Select-String '\[EH\] veh chain'                     # who runs in what order
$c | Select-String '\[HOOK\] (\w+)' | ForEach-Object { [regex]::Match($_.Line,'\[HOOK\] (\w+)').Groups[1].Value } | Group-Object | Sort-Object Count -Descending
$c | Select-String 'cycles after|ldr survey|hidden module' # loader-list tampering
($c | Select-String ([char]27)).Count                      # must be 0 (plain-text log)
```

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

1. **Never write to another module's code pages.** The only mutation is
   rewriting *import tables* (IAT) and our own allocations. The legacy ntdll stub
   patcher (`HookLegacyNtdll`) is off by default and would race artifact.dll's
   own ntdll hooks into a crash; the opt-in exists only so a blanked ini can
   never silently enable it.
2. **All surveys are read-only.** `[SCAN]`, `[KRNL]`, `[SVC]`, `[EH]` walks read
   memory and enumerate OS state; they never write, allocate in another process,
   or open someone else's object. A survey that pokes at the protection is a bug.
3. **Never filter silently.** Log it and classify it; attribution is separate
   from observation. If something is skipped (a cap, a budget), say so in the log
   with the threshold.
4. **The saved log is authoritative.** Tagged lines are appended to the file
   inside `Write()`; the console is scraped only to relay the game's own output
   as `[raw]`. Never route tagged lines through the console buffer.
5. **No work under the loader lock.** `DllMain` spawns the worker and returns.
6. **The hook path must be lock-free: no loader APIs, no toolhelp, no
   `GetModuleHandleExW`/`GetModuleFileNameW`, no file I/O, no allocation.**
   Anything it needs is a plain read of a worker-maintained table (`g_crange` via
   `CallerRangeFor`/`Describe`/`DescribeModuleOf`) or is queued for the worker
   (`NoteCallerAddr` → `ResolveCallerAddrs`, `HookAddPending` →
   `HookLateModules`). Not a style rule: a loader thread holding the loader lock
   while waiting on a thread inside one of our hooks deadlocks, and the game
   **hangs** — which is exactly what happened when caller attribution still
   called `GetModuleHandleExW`.
7. **Keep the game stable.** A crash or a hang is a failed run. The apphelp
   `SE_DllLoaded+0x3ae` `0xC0000409` fast-fail at process exit is cosmetic;
   nothing else is.
8. **Do not instrument a protected exe's own imports by default.** Denuvo checks
   the EXE's import table, so repointing it makes the protection fail its own
   tamper check — `Corrupt pak index detected` in Unreal titles. `HookExe`
   therefore defaults to **false**; the KERNELBASE interposition layer already
   covers calls that go through KERNELBASE without touching the protected image.
9. **A protection hypervisor may be loaded while the CPUID bit is clear** (PZ2:
   `hyperkd.sys` loaded, `CPUID.1:ECX[31]=0`). Never conclude "no hypervisor"
   from CPUID alone — report both and cross-reference the kernel module list.

## Build, deploy, validate

```
build.cmd                          # finds VS via vswhere, cl /LD /O2 /W3 -> GameConsole.dll
deploy.cmd                         # copies the DLL into a game folder (edit GAME= at the top)
tools\ScanTest.c                   # the regression harness (below)
tools\SingleHost.c                 # single-DLL host: LoadLibrary + hammered APIs + ODS + first-chance exception
tools\peimports.ps1 <pe-file>      # raw PE import dump: proves the binding census is complete
tools\symlookup.c                  # resolve <image>+<hex-offset> through the image's PDB (dbghelp)
tools\VehProbe.c                   # how the Win11 VEH node layout/encoding was established
tools\SysInfoProbe.c               # swept NtQuerySystemInformation classes 0x00-0x120 (see quirks)
```

`tools\CrashProbe.c`, `tools\StressTest.c` and `tools\TestHost.c` are **legacy**
from the pre-single-DLL layout: they load `Bootstrap.dll`/`GameHooks.dll`, which
no longer exist, so they will not exercise the current DLL. Keep them only as
history; use `SingleHost.c`/`ScanTest.c` instead. (`SingleHost.c` needs
`GameConsole.dll` in its own folder; the ini is created from defaults if absent.)

### Regression harness

Build `tools\ScanTest.c` to `%TEMP%\scantest\ScanTest.exe` with
`cl /nologo /O2 /W3 ScanTest.c /Fe:ScanTest.exe` (it links `advapi32` via
`#pragma comment`), copy `GameConsole.dll` in beside it, and run it **from that
directory**. It appends `progress.txt` in its working directory, so a failed run
can be read in order. It:

* loads `GameConsole.dll`,
* installs two real `FF25` detours on `ntdll!NtOpenFile` / `ntdll!NtQueryInformationFile`
  (slots and copy-prologue trampolines allocated within ±2 GB),
* registers a real VEH and UEH from the exe,
* loads `version.dll` **after** the DLL (late-load retry path),
* writes a registry value shaped like a driver `ImagePath`.

The ini is part of the test — four switches have to be set, each for a different
assertion (do not "simplify" this back to an empty `HookModules`):

```
HookModules=version.dll   # without a name to retry there are no late-load lines
HookExe=true              # else the exe's own IAT is untouched -> no veh/ueh lines
GpaThunks=false           # else GetProcAddress returns a logging thunk, the detours
                          # patch that thunk, and the survey finds no ntdll site
HeartbeatSeconds=10
HoldOnCrash=false
```

`GpaThunks=true` is the one real trade-off: it produces the
`[HOOK] gpa <api> called N ... last caller ScanTest.exe+0x…` counters, but then
the detours land on the thunk rather than the ntdll stub and the
`ntdll#1!NtOpenFile -> …` line does **not** appear. A single run cannot show both.
Also delete `HKCU\Software\GameConsoleScanTest` before a run: `regkey` logs
CREATED only, so on a second run the key already exists and `regset` appears
alone.

A good run is `exit=0` (`progress.txt` ends `wmain: returning`) plus all of:

```
[HOOK] (iat) KERNELBASE interposition layer: 20 Nt* imports hooked across 1 KERNELBASE image(s)
[HOOK] (iat) version.dll is not loaded yet - will retry each heartbeat ...
[HOOK] (iat) version.dll loaded late - instrumented now (6 imports)
[HOOK] regkey st=0x00000000 CREATED "Software\GameConsoleScanTest"  <- ScanTest.exe+0x...
[HOOK] regset st=0x00000000 "\...\GameConsoleScanTest"\ImagePath type=1 size=82 = \SystemRoot\System32\drivers\hyperkd.sys  <- ScanTest.exe+0x...
[HOOK] veh    add first handler=ScanTest.exe+0x... st=0x00000000  <- ScanTest.exe+0x...
[HOOK] ueh    set filter=ScanTest.exe+0x... previous=0x...  <- ScanTest.exe+0x...
[EH] veh chain: 1 handler(s) ...      then on the rescan: 2 handler(s), both named
[SCAN] ntdll#1!NtOpenFile -> ScanTest.exe+0x1000  (hooked: FF25 abs jmp)
[SCAN] ntdll survey across 1 image(s): 4 anomalous entries - 4 hooked outside ntdll, ...
[SCAN] ldr survey: load-order N / memory-order N / init-order N ...
[SYS] hypervisor: no CPUID hypervisor leaf ...; ServiceWatch kernel module(s) loaded: hvservice.sys ...
[SVC] installed driver: hvservice ... (currently LOADED)
[MEM] 1 low-address guard/no-access pages tracked (...)
```

The detour shows up as **four** anomalous entries (the Nt and Zw stubs share
their code, so each detour shows twice, counted as "hooked outside ntdll"), and
the `veh`/`ueh` lines are padded (`veh    add`, `ueh    set`). …plus **zero**
`0x1B` (ESC) bytes in the log, one guard-page summary line, no `hidden module:`
false positives (an on-demand DLL must not be mislabelled), no `<- 0x…`
unattributed callers, and every `binding census … (tally sums to N)` line
agreeing with the import count it reports. `[SYS]/[SVC]/[MEM]` depend on the box
(`hvservice.sys` here); the rest do not.

Verified 2026-11-16 at `69a499b` on Win11 Pro x64 with VS 2022 Build Tools
17.14.45 / SDK 22621: exit=0, all of the above, 0 ESC bytes, no false positives.

## Source map (`GameConsole.c`)

| area | functions / notes |
|---|---|
| ring + worker | `GCEv`/`EvPush`/`EvPop` (non-blocking, drops when full), `RingInit`, `TailLoop`, `RunMonitors`, `SafeCall` |
| log / console / CSV | `Write`, `FormatTagLine`, `CategoryOn` (per-tag show gate), `CsvRow`, `ScrapeConsole` |
| ini | settings table, `B()`/`I()`/`V()` readers, `TrimToW` |
| exceptions | `VehNative`, `UehNative`, `DrainVehEvents`, `InstallVehCapture` (installs the anchor VEH when `ScanEh` is on) |
| caller attribution (LOCK-FREE) | `g_crange` + `CallerRangeFor`, `CallerAddRange`, `RefreshCallerRanges`, `NoteCallerAddr`, `ResolveCallerAddrs`, `Describe`, `DescribeModuleOf`, `DescribeCaller` |
| module ranges / shadow naming | `RefreshModuleRanges`, `CensusDescribe` (`NAME#k @base+off`) |
| IAT hook layer | `HookIatApi`/`HookIatName`/`HookIatByValue`, every `HkIat*`, `InstallIatHooks`, `HookOneModule`, `HookLateModules`, `HookAddPending`/`IsPendingHook`/`HookClearPending`, `InstallKernelbaseInterposition` |
| KERNELBASE interposition | **20** `Nt*`: file/memory/thread/context probing + `NtCreateKey`/`NtSetValueKey`/`NtSetSystemInformation`; handlers `HkIatKb*`, `RegPathOfKey` (NtQueryKey), `HkIatKbNtSsi` |
| census | `CensusBindings`, `CensusAllModules` |
| surveys | `ScanNtdllHooks`(+`ScanNtdllImage`), `ShadowSurvey`/`DecodeExecPrologue`, `LdrSurvey`/`LdrWalkList` (per-list cycle detection) |
| exception chain | `VehChainSurvey`, `VehFindLayout` (runtime layout + `RtlEncodePointer`), `VehHandlerOf`, `VehDescribe` |
| system view | `CpuTopologyInfo` (CPUID + cross-reference, change-detected), `DriversScan` (`g_drvMatch`), `InstalledDriversScan` (`DrvIsLoaded` by image basename), `ServicesScan`, `WatchMatch`/`ServiceWatch` |
| hidden modules | `ImageSizeAt` (PE `SizeOfImage`), `g_hiddenReported2` |
| monitors | threads, exec memory, network, filesystem watchers (incl. the `.sys` classifier), heartbeat, guard pages, KUSER_SHARED_DATA mailbox |

## Settings groups

* console: `ConsoleVisible`, `AllocConsole` (false = log-file-only), `ConsoleTitle`, `ConsoleCols`, `ConsoleRows`, `ShowTimestamps`, `ShowColors`, `ShowLegend`, `ShowStartupInfo`
* saving/log: `SaveLogFile`, `LogDirectory`, `SaveTaggedLines`, `SaveRawLines`, `WriteTraceFile`, `WriteCrashFile`, `CsvExport`
* capture: `CaptureOutputDebugString`, `RedirectCrt`, `RedirectStdHandles`, `EnableCrashHandler`, `HoldOnCrash` (**freezes** the process on an unhandled crash; shipped ini sets false)
* show gates: one `Show_*` per tag group; `Show_System` gates `[SYS]/[KRNL]/[SVC]`, `[EH]` rides on `Show_Exceptions`, `Show_Hooks` gates `[HOOK]`, and `[SCAN]` has no switch of its own — `CategoryOn` falls through to `Show_LogFiles` for any tag it does not recognise
* hooks: `HookApis`, `HookFileReads`, `HookFileWrites`, `HookMemPatches`, `HookOnlyGameModules`, `HookLegacyNtdll`, `HookModules`, `HookExe` (false), `MaxHookReports`, `GpaThunks`
* surveys: `CensusAll`, `CensusAllMax`, `ScanShadow`, `ShadowDetailMax`, `ScanLdr`, `ScanOtherHooks`, `ScanOtherHooksMax`, `ScanEh`, `ScanServices`, `ScanDrivers`, `ServiceWatch`, `ServiceWatchAll`, `WatchSharedData`, `MemGuardScan`, `MemGuardAll`
* the remaining groups (modules, log tailing, exceptions, threads, memory, network, files, heartbeat, ODS) live in `README.md`'s settings table with their defaults — keep that table in sync with `Opts[]`

## Output tags and hook kinds

Tags: `[INFO]` `[MOD]` `[THR]` `[MEM]` `[NET]` `[FS]` `[HB]` `[SD]` `[ODS]`
`[EXC]` `[CRASH]` `[EH]` `[HOOK]` `[SCAN]` `[SYS]` `[KRNL]` `[SVC]` `[console]`
`[<file.log>]`, plus `[raw]` for the game's own console output.

Hook kinds on `[HOOK]` lines: `create/open/read/write/…` (file+memory),
`qip/sit/sip/qit/setctx/getctx/cthex/avm/rvm/wvm/qvmm/openthr` (anti-debug and
mutual probing), `veh add|remove`, `veh add continue`, `ueh set`, `regkey`
(CREATED only), `regset`, `syst` (`NtSetSystemInformation`),
`svc openmgr|create|start|delete`, `gpa`, `iat`.

## Domain knowledge from real runs (observational)

* **Two Denuvo re-bind styles.** FFRS routes the protected EXE's Win32 imports
  through one RWX thunk page (`0x7FF4FD430000`, `MEM_PRIVATE`, starts `MZ`);
  PB0/PZ2 point the EXE IAT straight into manual-mapped *shadow copies*
  (`KERNEL32#1`, `USER32#1`, `ntdll#1`, `GDI32#1`, `advapi32#1`, `CRYPT32#1`,
  `KERNELBASE#1`, `shlwapi#1`) that are `MEM_PRIVATE`, not `MEM_IMAGE`.
* **Shadow set** (same bases everywhere): ntdll#1 `0x7FF4FDC90000`,
  KERNEL32#1 `0x7FF4FDA80000`, KERNELBASE#1 `0x7FF4FD870000`,
  USER32#1 `0x7FF4FD450000`, GDI32#1 `0x7FF4FD660000`,
  advapi32#1 `0x7FF4FD240000`, CRYPT32 `0x7FF4FCE20000`,
  SHLWAPI `0x7FF4FD030000`. PB0/PZ2 report a uniform fake `0x201000`
  `SizeOfImage`; FFRS reports real sizes. Never trust `SizeOfImage` from a
  loader entry here.
* **PB0 and PZ2 relink the PEB loader lists; FFRS does not.** Walking them never
  reaches the head, so the tool reports every list per rescan:
  `[SCAN] ldr: load-order list cycles after 88 distinct entries - loader list
  relinked (not normal)` and `ldr survey: load-order 88 / memory-order 88 /
  init-order 87 distinct entries, … 24 pointing at non-image memory`. FFRS, same
  code: `load-order 57 / memory-order 57 / init-order 56 …, 15 pointing at
  non-image memory`, no cycles. Counts grow/shrink across a session
  (PB0: 88→150→193→194). This is why the old fixed 512-entry cap only ever saw
  ~70 modules.
* **`artifact.dll`** is the mod: it carries the literal string `hyperkd`, forwards
  `ntdll#1!LdrGetDllHandle` and `RtlAddVectoredExceptionHandler` to itself (newer
  builds), and starts the driver through the SCM. `apphelp.dll` takes
  `Nt*File`/`ZwSetValueKey` forwards (AppCompat shim).
* **`dse.dll`** is a hidden helper: in no toolhelp list and no PEB loader list,
  resolvable only by address; it does cross-process `NtWriteVirtualMemory` (2
  writes per target into ~20 processes). Detection is **event-driven** — it shows
  up only when it calls something we instrument.
* **Hypervisor**: `hyperkd.sys` in `<game>\Artifact_Intel\`, registered under
  service key `artifact` (PZ2), so compare a service's **ImagePath basename**
  with loaded modules, never the key name. Plus Microsoft's `hvservice.sys`.
  `NtLoadDriver`/`ZwLoadDriver` are imported by none of them: the load goes
  through `CreateServiceW` + `StartServiceW` + registry (`ImagePath`/`Type`/`Start`).
  Loaded without the CPUID hypervisor bit (see hard rule 9).
* **The protection's VEH handler is only visible in PB0.** PB0's chain:
  `[EH] veh chain[0] (first): GameConsole.dll+… [GameConsole]` then
  `[EH] veh chain[1] (last): artifact.dll+0x1211A`. PZ2/FFRS show **only** our
  handler — the newer artifact keeps a private handler list behind its
  `RtlAddVectoredExceptionHandler` proxy. No `veh add` line appears in any run
  because artifact arms before our hooks exist and/or goes through its shadow
  `ntdll#1` export table.
* **Anti-debug**: PB0 and PZ2 run a self-probe single-step loop (byte-identical:
  `Dr6=FFFF0FF0`, `Dr7=0x440`, `rip=Dr3=…`), and probe `ProcessDebugPort` (0xC),
  `AmILastThread` (12), `ThreadHideFromDebugger` (0x11) etc. FFRS shows none of
  that; its KUSER_SHARED_DATA mailbox stays `…37133713`.
* **Not the protection**: FFRS's `steam_api64.dll+0x9C381B` throws `0xE06D7363`
  ~100–200× per run.

## Windows quirks this code accounts for

* **No reliable documented user-mode hypervisor query**: `tools/SysInfoProbe.c`
  swept `NtQuerySystemInformation` 0x00–0x120 and none returns an unambiguous
  hypervisor structure (the ones that succeed are zeroed with no hypervisor
  armed). Presence is derived from CPUID **plus** the kernel module list — do not
  "fix" that by guessing a class number.
* Kernel base addresses are **redacted** for ordinary callers via
  `SystemModuleInformation`; `[KRNL]` reports names/sizes/load order and says so.
* The object manager's `\Driver` directory returns `STATUS_ACCESS_DENIED`
  (`0xC0000022`) to a game process, so installed drivers come from
  `HKLM\SYSTEM\CurrentControlSet\Services` (works unelevated).
* A VEH node stores its handler as `RtlEncodePointer(handler)` on current builds;
  the layout and encoding are discovered at runtime by `VehFindLayout` — never
  hard-code offsets.
* `winternl.h`/`UNICODE_STRING` are unavailable under `WIN32_LEAN_AND_MEAN`;
  declare the equivalents locally (`GC_UNI`, `GC_USTR`).
* A `LoadLibraryW`'d DLL used by the harness must be re-checked after it loads:
  the caller table is a worker snapshot, so "hidden" means *absent from a fresh
  toolhelp list*, not "absent from my cache".

## Gotchas already paid for

* Empty `HookModules` means **KERNELBASE interposition for all callers**, not "no
  hooks"; `HookExe` (off) additionally instruments the exe's own IAT — which is
  what trips Denuvo's tamper check.
* Instrumenting a protected EXE ⇒ `Corrupt pak index detected` (PB0 + FFRS).
  That is why `HookExe=false`.
* Loader APIs in the hook path ⇒ the game hangs at startup (PZ2). See hard rule 6.
* Caller naming must not use the loader; unresolved callers are queued by
  address and resolved on the worker, which also refreshes the table **before**
  deciding hidden-or-not (an on-demand DLL was once mislabelled, `NSI.dll`).
* A mod's helper DLL loads *after* this DLL: unresolved hook targets are logged,
  retried each heartbeat, and a hidden module is hooked **by address**.
* `HoldOnCrash=true` + an unhandled crash = a process frozen in `Sleep(INFINITE)`
  ("held open"), which looks exactly like a hang. Set it to false when testing.
* A service key name is not a driver file name (service `artifact` →
  `hyperkd.sys`); loaded-state checks must use the ImagePath basename.
* The guard-page scan must stay summarised (it once produced 481 lines/run).
* The shadow survey needs a **per-module** detail quota (`ShadowDetailMax`); a
  global budget let one module eat every line.
* Adding a tagged line: add the tag to `CategoryOn` if it needs a show gate, emit
  the matching `CsvRow`, and document it in the ini and `README.md`.

## Conventions

* Plain C (MSVC, `/W3` clean), Windows-only, no external dependencies, no C++.
* Keep ini comments adjacent to the switch they document; `README.md` is the
  user-facing description and must describe every switch, tag and hook kind.
  The settings section of `README.md` carries a full table of all `Opts[]`
  entries (key, default, meaning), so adding a setting means adding a row there
  too — the table is the check that no switch is undocumented.
* Build outputs are gitignored — the repo is the source of the tool, not the
  deployed artifact. After changing behaviour: `build.cmd`, run ScanTest, then
  commit; if a game was involved, say which title and paste the lines that prove
  it (the log is the evidence).

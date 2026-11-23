# AGENTS.md — working notes for GameConsole

Context for anyone (human or agent) picking this repository up, written as a
handover: state first, then the rules and the knowledge. Most of it was learned
the hard way on real Denuvo titles.

## Status / handover

### SESSION HANDOVER — 2026-11-23. Read this block first; it supersedes the 16 Nov block below.

**Machine swap done, and the research question is answered.** This box: Win11,
titles on `H:` (**not** `F:` — every path in the 16 Nov block below is stale):
PB0 `H:\Games\Phantasy Blade 0\platform\steam\PBZ.exe`, PZ2
`H:\Games\Planet Zoo 2\PlanetZoo2.exe`, FFRS
`H:\Games\FINAL FANTASY RESONANCE\FFRS\Binaries\Win64\FFRS-Win64-Shipping.exe`.
MSVC via VS 18 Community (`vcvars64.bat` at
`C:\Program Files\Microsoft Visual Studio\18\Community\...`).

**Repo state** `main` = `8fe51a1`, pushed (b0dfe3c = wide wvm preview;
8fe51a1 = EarlyLoad quoted-path fix). Deployed `GameConsole.dll` is `514A8D5E`
(351,232 B), built from the `b0dfe3c` source, hash-verified in all three game
folders; ScanTest green on this box at that build (exit=0, 0 ESC, 0
unattributed). Compare source commits across machines, never hashes across
builds.

**HVCI gate (this box's 10-second silent death).** With HVCI/VBS on, PB0 exits
~10 s in with no protection activity, no logs and no WER. dse's readme says it
needs HVCI/VBS off; `Win32_DeviceGuard` now reads VBS=0 / HVCI Enabled=0 and
runs go clean. Note the trap: `[SYS] code integrity` still shows the
`hvci-kmci` bit set (`0x0000C301`) with HVCI *actually off* — that bit is
configured, not armed; don't read it as HVCI being on.

**Four PB0 runs, all with HostWatcher + dse logging (`dse.ini logging=true`):**

* Run A (22:02, loader path): clean 75 s, normal exit, `worker started
  +4611 ms` — the baseline, and first clean in-game run of this source.
* Run B (22:11): `dse-dll.log` (dse's own log, written next to dse.dll) is the
  whole install from inside the game process: extract `RTCore64.sys` to
  `%TEMP%\dsedll\`, BYOVD patch `SeCiCallbacks` → `ZwFlushInstructionCache`
  (original saved to registry), DSE restored when the game's first window
  appears (+8 s), watchdog only handles the exit path. **The `[SYS] code
  integrity` DSE pair can never fire** — dse patches the CI callback, not CI
  policy; our sampler inside the disabled window still logged "DSE on", and so
  did the watcher's 200 ms poll. The observables of the toggle are dse's log,
  its registry patch state and the RTCore64 mtime — nothing else.
* Run C (22:16, wide-preview fix): clean; captured dse's injected blob. It
  writes 0x11D8 bytes into ~6–10 processes (2 writes per target): the blob,
  then an 8-byte pointer at a fixed per-process slot ending `0x2D8` holding
  the blob's base. Blob layout: 520 zero bytes, then `D8 11 00 00` (own
  length), magic `0xAC0DEDAB`, version 1, machine `0x8664`, four u32 offsets
  inside ntdll.dll's size range, tail `0x821 / 1 / 16×0x11`. A structured
  descriptor, not code.
* Run D (23:02, EarlyLoad t=0 with `HookApis=false` — zero IAT writes):
  injection succeeded (`LoadLibraryW returned 0xE7690000`), game died on
  Denuvo's anti-tamper page after 30 s. **Decisive: the injection itself is
  what Denuvo detects; writes are excluded.** The 21:11 open question is
  closed.

**Loader-lock starvation (mechanism behind "dse locks out loading").** Even at
t=0, run D's worker logged `+3926 ms`. DllMain only spawns the worker; the
worker's `LdrInitializeThunk` queues on the **loader lock**, which the main
thread — resumed microseconds later — holds for its whole init and mod chain
(winmm → artifact.json → GameConsole → dse ≈ 3 s of dbghelp PDB work under
the lock → coldloader). The worker runs only when that chain finishes. So a
t=0-injected worker would arrive *after* the install even if anti-tamper
let it live.

**Verdict on the old item 1 (capture the install in-process): dead end, and
now a documented negative.** Two independent walls: anti-tamper kills t=0
injection regardless of writes, and loader-lock starvation would delay the
worker past the install anyway. The sanctioned loader path delivers the DLL at
+4.4 s, after the install (+1.3–3.2 s). The install is instead fully
instrumented by: dse's own `dse-dll.log`, `tools\HostWatcher.exe` (external:
DSE poll, kernel images, service lifecycle, helper processes, teardown), and
the in-process `wvm`/`avm` capture of dse's cross-process writes. PZ2/FFRS run
the same artifact family — expect the same two walls; do not burn attempts.

**State to leave the box in** (currently true): all three `GameConsole.ini`
on the sanctioned recipe (`HookApis=true`, `HookExe=false`,
`HookHiddenModules=false`, `HookDelayMs=0`); PB0's `dse.ini` has
`logging=true` deliberately — `dse-dll.log` is now the install record; turn it
off only if the user asks. Evidence transcripts on this box:
`tools\HostWatcher.run{A-20261123,A2-ok-2202,B-2211,C-2216,D-2302}.log`,
`tools\EarlyLoad.runD.log`, game logs under each title's `logs\`.

**What is left:** nothing blocking. The tool is a validated observer under the
sanctioned loader path on this box. Optional polish from the 16 Nov list (ldr
line dedupe, naming artifact's private VEH) remains optional; the 16 Nov
next-steps below are superseded by the verdict above.

**Repository** `https://github.com/DKAngel2000/GameConsole` (branch `main`,
remote `origin`). Source-only: `*.dll`, `*.exe`, `*.obj`, `logs/`, `*.log`,
`*.csv` are gitignored, so the built DLL is *not* in the repo — run `build.cmd`.
`.gitattributes` pins LF for text (`*.c/*.h/*.md/*.ini/*.ps1`, CRLF for
`*.cmd/*.bat`, binary for build outputs); `core.autocrlf=true` is set, so
`git ls-files --eol` should show `i/lf w/lf` for sources and docs, and
`i/lf w/crlf` for `*.cmd`/`*.bat` (checkout conversion, by design — not drift).

**Last behaviour-validated commit**: `8975eb6` for the three-game half — the
deployed DLL in all three folders hashed identical to the repo build, and **all
three** titles ran to a normal exit: PB0 13:02:56→13:04:00, FFRS
13:02:09→13:02:37, PZ2 13:04:15→13:04:57. FFRS's only WER entry is the known
cosmetic `apphelp.dll` `0xC0000409`; PB0 and PZ2 left none. That run is also the
first game-side evidence for the code-integrity monitor (one `[SYS] code
integrity: options 0x0000C001 (enabled undecoded 0x6) … DSE on` per run, no
change — the driver was already loaded) and for the module-level ntdll probe
hooks. Before it, `0cabdee` had all three exit cleanly except PB0 (item 6, now
explained), and `aaa58ce` before that.

ScanTest was re-run green on 2026-11-16 (Win11 x64, VS 2022 Build Tools) at
`69a499b`, again with the module-IAT registry bridge on top of `e0b2ede`, again
at `8975eb6` with the code-integrity monitor, the direct-ntdll module probes
(`qit`/`qvif`) and the returned-data previews, at `b645e8f` with the `qit(iat)`
layer tag, at `630b2fd` with the startup reorder + 3 s DSE window, and again with
the hidden-module guard and the process-start diagnostic (`9391ed4`):
exit=0, one `regkey`/`regset` each, 8 FF25 sites, VEH chain 1→2, decoded `qvmm`
data, the DSE baseline line and `process created … worker started +N ms later`,
0 ESC bytes, 0 unattributed callers.

**Build ↔ run map for 2026-11-16** (deployed hash → repo commit → what it ran):
`1E3C7FC0` = `9391ed4` (hidden-module guard) → **all three titles clean at 17:22/
17:23/17:25**, the run that validated the guard; `660B39C4` = `8b75f7b` (kernel
base redaction + watcher process tracking) → **PB0 clean at 20:22** (elevated
launch); `9D09CCCA` = `3b04943` (hidden = toolhelp AND PEB) → the 21:11 EarlyLoad
run that died on Denuvo's anti-tamper page; `2385E81A` = `b6358c3` (`HookDelayMs`)
→ the 22:16/22:18 runs that also failed anti-tamper but were **not** early-loaded
(`+4374`/`+4402 ms`). Update
this line whenever you validate a change, so the next session knows what the last
*proven* state was (`git log` is the tip; this line is about evidence, not
recency).

### SESSION HANDOVER — 2026-11-16 late evening. (Superseded by the 23 Nov block above; kept for its rules, quirks and domain knowledge.)

**The guard IS now validated in a game** (this supersedes the paragraph above):
the 17:22/17:23/17:25 runs of PB0/PZ2/FFRS all went to a normal exit on the
guard build, and PB0 logged the hidden `dse.dll`, then
`(iat) dse.dll is a hidden module … reported but NOT instrumented
(HookHiddenModules=false)`, and ran on for 64 s. The 14:14/14:15/14:16 deaths
were the by-address IAT write into dse.dll; that is settled.

**Where the deployed state is.** `GameConsole.dll` (343 KB, SHA256 starts
`251103D7`) is copied into all three game folders and is the build of `b6358c3`,
the last code commit (later commits are docs and `tools` only). Rebuild and
redeploy on the new machine with `build.cmd` + the three `Copy-Item`s (see the
cheat sheet below) — the DLL is gitignored, so it does **not** travel with the
repo, and MSVC embeds a timestamp, so two builds of the same source have
different hashes: compare **source commits** across machines, never hashes across
builds.

**Outstanding trap: `HookDelayMs=8000` is set in PB0's ini right now**
(`F:\5068070\platform\steam\GameConsole.ini`, added for the EarlyLoad test). It
must be `0` for ordinary loader launches, or every run there gets its hooks 8 s
late. The repo's sample `GameConsole.ini` documents both `HookDelayMs` and
`HookHiddenModules`; the game-folder inis are hand-maintained per machine and are
**not** in the repo, so on a new box copy the recipe block below into each game
folder.

**What the last experiments established (PB0):**

* Launched from an **elevated** prompt the game does not relaunch: one `PBZ.exe`,
  elevated, and the whole protection startup inside its first 1.6 s —
  `RTCORE64.SYS` from `%TEMP%\dsedll\` at +0.45 s, the two watchdog helpers at
  +0.75 s, the three hypervisor images and the DSE restore at +1.0 s, service
  `artifact` RUNNING at +1.6 s. That run (20:22) was **clean**.
* The watchdogs are `rundll32.exe "…\dse.dll",DseWatchdog 724 0 1` and
  `rundll32.exe "…\artifact.dll",Watchdog <game pid>` — separate elevated
  processes, so the SCM calls and the DSE work are not in the game process at all.
* Double-clicking adds `dse_elevator.exe` + UAC and **relaunches** the game
  elevated; the process we instrument is the second one.
* Even elevated, the loader brings `gameconsole.dll` in at **+4.4 to +4.8 s**
  (`process created … this worker started +N ms later`), i.e. after everything.
  That number is the fastest way to tell whether a run was early-loaded or not:
  **small N (tens of ms) = injected at instruction zero; ~4.4 s = the loader did
  it, and the run says nothing about early capture.**
* `tools\EarlyLoad.c` (suspended create + remote `LoadLibraryW` + resume) does
  inject successfully — the 21:11 run printed `LoadLibraryW returned 0xBFD00000
  (loaded)` — and that run died with **Denuvo's anti-tamper page**
  (`support.codefusion.technology/antitamper/…`). Cause not fully isolated: with
  `HookDelayMs=0` our IAT writes land *before* the protection's own startup, which
  is a new condition (the loader path writes at +4.4 s, after it, and never
  tripped it).
* Two later runs (22:16 `HookDelayMs=8000`, 22:18 `HookApis=false`) still failed
  anti-tamper — but both logged `+4374 ms` / `+4402 ms`, so our DLL was **not**
  early-loaded in them, and in the `HookApis=false` run there were no writes at
  all. Those two runs therefore prove nothing about early capture, and they leave
  two candidates open: (a) EarlyLoad was run from a non-elevated prompt, so the
  game relaunched and our DLL sat in the discarded first process; (b) Denuvo's
  anti-tamper state is sticky after the 21:11 failure. `EarlyLoad.exe` now
  **refuses to run from a non-elevated prompt** (it prints why) so (a) cannot
  recur silently.

**Next steps, in order:**

1. **Control run A — is the machine's state clean?** Ordinary launch (double-click
   or started from the elevated prompt, no EarlyLoad), PB0 ini with
   `HookDelayMs=0`. If PB0 is clean like 20:22, we are free to test EarlyLoad
   again; if it *still* fails anti-tamper with no injection involved, the
   protection's state is poisoned (reboot / wait / re-check) and none of the
   22:1x conclusions stand.
2. **Control run B — EarlyLoad, elevated, with the watcher running.**
   `HostWatcher.exe` first, then `EarlyLoad.exe "<game>\PBZ.exe"` from an
   **Administrator** prompt. In the watcher: if `dse_elevator.exe` appears, the
   game still relaunched (so EarlyLoad is the wrong tool for that launch); if it
   does not, and the game log shows a **small** `+N ms`, we finally have t=0
   capture. Diff from the 20:22 baseline.
3. If t=0 capture works with `HookDelayMs=8000`: read the `[SYS] code integrity`
   pair (the first in-process proof of the install — the state is system-wide and
   sampled every 10 ms), then consider making the watchdog spawns visible without
   touching artifact.dll by hooking **KERNELBASE's import of
   `NtCreateUserProcess`** in the interposition layer (idea, not built).
4. Set `HookDelayMs=0` again.
5. Then PZ2/FFRS: same recipe; whether PZ2 spawns the same watchdog pair is
   unknown, and FFRS's older artifact has never shown one.

**Cheat sheet (paths are per-machine; commands are the same):**

```powershell
cd F:\Projects\GameConsole
cmd /c build.cmd                                     # -> GameConsole.dll (/LD /O2 /W3)
foreach ($g in 'F:\5068070\platform\steam','F:\Planet Zoo 2','F:\FINAL FANTASY RESONANCE\FFRS\Binaries\Win64') {
  Copy-Item GameConsole.dll "$g\GameConsole.dll" -Force }        # deploy
# harness (ini recipe matters - see the ScanTest section)
reg delete "HKCU\Software\GameConsoleScanTest" /f
Copy-Item GameConsole.dll "$env:TEMP\scantest\GameConsole.dll" -Force
cd "$env:TEMP\scantest"; .\ScanTest.exe; Get-Content .\progress.txt | Select-Object -Last 1
# tools
cl /nologo /O2 /W3 tools\HostWatcher.c /Fe:tools\HostWatcher.exe   # from a vcvars64 shell
cl /nologo /O2 /W3 tools\EarlyLoad.c   /Fe:tools\EarlyLoad.exe     # same
.\tools\RunReport.ps1 -Dir '<game>\logs'                           # read a run
```

**Known-good reference logs** (per machine, not in the repo):
PB0 `GameConsole_20261116_202257_pid2220.log` (elevated, clean, watchdog lines in
the watcher) and `…_172204_pid25724.log`; PZ2 `…_172357_pid27776.log`; FFRS
`…_172512_pid25604.log`. The watcher's own transcript from the decisive run is
saved by the user at `C:\Users\creat\OneDrive\Desktop\hostwatcher.txt`.

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
HookHiddenModules=false  # never rewrite the import table of a module that hides itself (item 6)
HookDelayMs=0            # >0 only while testing tools\EarlyLoad.exe; 0 for ordinary launches
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

1. **Capture the driver *install* — and it happens in-process, at startup.** The
   real sequence was described by the user on 2026-11-16 and supersedes the
   earlier "delete the key and reboot" recipe:

   1. launch the game exe by hand;
   2. `winmm.dll` (the mod loader) reads `artifact.json` and loads its `load_dlls`
      in order — `gameconsole.dll` **first**, so this DLL is in before the
      protection's own startup;
   3. `dse.dll` loads;
   4. dse takes **DSE down** (driver signature enforcement off);
   5. `artifact.dll` loads/registers and **starts the hypervisor service**;
   6. dse puts **DSE back on**;
   7. the hypervisor stays running, the game runs;
   8. the game exits → the hypervisor is stopped, and the `artifact` service key
      is gone afterwards (`sc query artifact` → error 1060 on a box that has run
      a title and then been left alone).

   So this is **not** provoked by deleting the key and rebooting a *game* — the
   install is part of every launch — but it is a race against GameConsole's own
   startup, because the whole thing completes within the first few tens of
   milliseconds. That race is now fought explicitly: settings → log file →
   caller table → VEH capture → module log → `InstallHooks` all run before the
   console allocation, the CSV header and every survey (time from `DllMain` to
   hooks ≈ 3 ms in the harness), and a **3 s startup window** then samples the DSE
   state every 10 ms and retries pending hook targets every ~40 ms so a `dse.dll`
   that loads after us is instrumented while it is still working.

   What a successful capture looks like (all read-only, all already
   implemented):
   * `(iat) dse.dll loaded late - instrumented now (21 imports)` — then its
     `gpa` lines for `LoadLibraryA`/`GetProcAddress`, which is how that stub
     resolves everything it uses.
   * `[HOOK] svc openmgr` → `svc create` → `svc start`, or with the service
     already registered, `svc openmgr`/`svc start` — artifact imports the whole
     SCM set from `advapi32` (`OpenSCManagerW`, `CreateServiceW`,
     `StartServiceW`, `OpenServiceW`, `ControlService`, `DeleteService`).
   * `[SYS] code integrity CHANGED … - DSE was turned OFF` … later
     `… - DSE was turned back ON`. That pair is the proof that an unsigned
     driver was loaded by this session.
   * `[SVC] driver service registered: …` / `is now LOADED - the game started
     it`, and `[KRNL] driver: hyperkd.sys`.

   If a run instead shows `currently LOADED (already up at first scan - the mod
   loads it during process startup)` with no `svc` lines and no DSE pair, the
   race was lost for that launch — compare the timestamps of the first log line,
   `hooks: IAT capture on` and the first `[SVC]` line before concluding anything
   about the protection.

   External cross-check: `tools\HostWatcher.c` (`cl /O2 /W3 HostWatcher.c`) polls
   the DSE state every 50 ms, the process list every 200 ms (creation, exit,
   parent, elevation flag, command line) and the kernel module list plus the
   driver-service table every 500 ms, and prints only transitions. Start it
   *before* launching the title: it sees the service appear and go away, the
   driver load/unload, and the protection's helper processes, including the
   teardown a log inside the game cannot survive to record.

   **What the watcher then showed (PB0, 2026-11-16 18:49 and 20:22), and why the
   install is still not in the game log.**

   *Double-click path* (18:49): `PBZ.exe` (unelevated) spawns
   `dse_elevator.exe` → UAC `consent.exe` → `RTCORE64.SYS` loads from
   `%TEMP%\dsedll\` (DSE off) → a **second `PBZ.exe` starts ELEVATED**, as a child
   of the elevator, while the original and the elevator exit → the three
   hypervisor images load and RTCore64 unloads (DSE back on) → service `artifact`
   `START_PENDING` → `RUNNING`. So the game **relaunches itself elevated**; the
   process we instrument is the second one.

   *Elevated path* (20:22, launched from an elevated prompt by hand): no elevator,
   no relaunch, one `PBZ.exe` pid 2220 ELEVATED, and the whole protection startup
   inside its first 1.6 s. The two helpers are spawned by the game and are the
   answer to "dse via a watchdog" — they are **separate processes**:

   ```
   20:22:53.161 KERNEL + RTCORE64.SYS      %TEMP%\dsedll\RTCore64.sys      (DSE off)
   20:22:53.540 PROC   + rundll32.exe  pid 28636 parent 2220 ELEVATED
                cmd=rundll32.exe "F:\5068070\platform\steam\dse.dll",DseWatchdog 724 0 1
   20:22:53.540 PROC   + rundll32.exe  pid 28304 parent 2220 ELEVATED
                cmd=rundll32.exe "F:\5068070\platform\steam\artifact.dll",Watchdog 2220
   20:22:53.792 KERNEL + HYPERKD.SYS / HYPERHV.DLL / HYPEREVADE.DLL, - RTCORE64.SYS
   20:22:53.806 SERVICE artifact START_PENDING -> RUNNING (20:22:54.434)
   ```

   Both watchdogs are `rundll32.exe` hosting the protection's own DLLs — so the
   SCM calls and the DSE work happen in *those* processes, not in the game, and no
   hook inside the game could ever see them. (Do not inject into the watchdog:
   that is the protection's own process, and it is a write into somebody else's
   code, which is exactly what hard rule 1 forbids.)

   *And the loader is the other half of the problem*: even elevated, the game log
   still says `process created 20:22:52.714 - this worker started +4761 ms later`,
   i.e. `gameconsole.dll` is not brought in until ~4.8 s in, long after the
   protection's ~1.6 s startup. That is the loader's staging of `load_dlls`, not
   elevation, and it is why the in-process log can never contain the install.
   The route that does put us at instruction zero without modifying any file:
   `tools\EarlyLoad.c` — `CreateProcessW(CREATE_SUSPENDED)` + remote
   `LoadLibraryW(GameConsole.dll)` + resume. Run it from the elevated prompt so
   the child inherits elevation. The loader's later `LoadLibrary` of the same DLL
   is then just a refcount bump, so the hooks and the worker are in place before
   the first instruction. What that should finally put in the log: the game-side
   `CreateProcessW` of both watchdogs (we hook it in artifact.dll's IAT) with
   their command lines, and anything artifact/dse do in the game process after
   that.

   **Do not expect `regkey`/`regset` for the service key from the game.** A
   service created through the SCM has its key written by `services.exe`, in
   that process; artifact.dll imports no registry API at all (its 9 `advapi32`
   imports are the SCM ones). The earlier expectation that the key write would
   appear as `[HOOK] regkey … CREATED "\REGISTRY\MACHINE\SYSTEM\…"` was wrong.
   `regkey`/`regset`/`syst` and the `[FS]` `.sys` classifier therefore still have
   **no** in-game validation, and the module-IAT registry bridge is inert for
   these three titles — keep it for modules that do import the registry pair, but
   do not count on it for item 1.

   Then run `tools\RunReport.ps1 -Dir <game>\logs`: its "Driver / service install
   trace" section states outright whether any of that was captured. Take the
   run's service key **name** from the log, not from this note.
2. **Hidden-module PE-size fix — validated in a game** (PB0, 2026-11-16):
   `hidden module: dse.dll @0x7FF9A81B0000 size 0x60000` and
   `hidden module: nvldumdx.dll @0x7FF9A8580000 size 0xD1000`, real
   `SizeOfImage` values rather than the old first-region `0x1000`. dse.dll is
   listed in `load_dlls` next to gameconsole.dll, and the census walked its
   imports (21, all `tally sums to 21`).
3. **dse.dll detection is event-driven**: it is found only when caller
   attribution catches it calling something we instrument. **It never appeared at
   all in the 13:02/13:04 runs** (`hidden modules: 0` in all three, and no
   `dse.dll loaded late` line) — because the hypervisor was already installed and
   running, so the helper had nothing to do. The PB0 run at 12:38 did catch it
   (item 2) once, after the last heartbeat, so `HookLateModules` never got to
   instrument it. A session where the protection actually acts — i.e. the
   install in item 1 — is still what is needed to hook it and see its `wvm`/`svc`
   calls.
4. Optional polish: the `[SCAN] ldr: <list> cycles after N distinct entries`
   lines repeat once per rescan (that is the list size changing, e.g. 90→150→
   193→194 on PB0). Dedupe to first-report-plus-change if it becomes noisy.
5. Optional RE: name artifact's VEH handler for PZ2/FFRS. It is *not* in the OS
   chain (private list), so it needs a read-only hunt for the protection's own
   list — nothing yet.
6. **PB0 dies the moment a hidden module is instrumented by address — root-caused
   and fixed** (2026-11-16). History: the 12:38 PB0 run ended abruptly with no
   `[CRASH]`, no crash file and no WER entry; the 13:02 run survived and I
   wrongly closed it as a one-off. The three launches at 14:14/14:15/14:16 settle
   it: **all three** end on the same two lines —

   ```
   [SCAN] hidden module: dse.dll @0x7FF980940000 size 0x60000 type 0x1000000 protect 0x2 ...
   [SCAN] binding census for dse.dll: 21 imports (0 unbound) in 19 descriptor(s) ...
   ```

   — and nothing after, 3.7–5 s into the session, with the UE fatal-error dialog
   on screen. The surviving 13:02 run is the one that never detected dse.dll
   (`hidden modules: 0`). The next thing that code does is `HookOneModule` on the
   hidden module, i.e. **`VirtualProtect` + rewrite of dse.dll's import table by
   address** — the one write this tool performs in another module's memory that
   the owning module has not asked for. `HookHiddenModules` (default **false**)
   now stops there: a hidden module is detected, censused, reported and added to
   the caller table, but not repointed. Re-run and if PB0 survives to a normal
   exit the cause is confirmed; the newest logs also carry
   `process created HH:MM:SS.mmm - this worker started +N ms later`, which is the
   number to quote when judging whether a launch was in time at all.

   Related, and worth reading before the next attempt: the install itself is
   visible from outside even when it is not visible in-process —
   `tools\HostWatcher.log` from that session shows the whole cycle (see the
   kernel-agent bullet in Domain knowledge).

### How to read a run (quick recipe)

Use the script; it is this recipe, kept in one place:

```powershell
.\tools\RunReport.ps1                          # newest log under .\logs
.\tools\RunReport.ps1 -Dir <game>\logs         # newest log in a game folder
.\tools\RunReport.ps1 -Log a.log,b.log         # compare sessions side by side
.\tools\RunReport.ps1 -Log x.log -Full         # also dump the matching lines
```

It prints: ESC/unattributed-caller integrity, the hook surface (which modules and
which hook kinds), the **driver/service install trace with a verdict** (item 1),
loader-list cycles/ghosts, hidden modules, the VEH chain, `[SYS]/[KRNL]/[SVC]`,
anti-debug probes, and census tallies. Read-only.

By hand, the same queries:

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
tools\RunReport.ps1 [log]          # summarise/compare a run log (the recipe, scripted)
tools\HostWatcher.c                # external watcher: DSE + driver load/unload + helper processes
                                   #   from outside the game (cl /O2 /W3 HostWatcher.c)
tools\EarlyLoad.c                  # start the game suspended, LoadLibraryW(GameConsole.dll),
                                   #   then resume - the DLL is in before instruction zero (item 1)
tools\KmodProbe.c                  # one-shot: are kernel-module ImageBase values real or redacted
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
| registry bridge (module IAT) | `HkIatModNtCreateKey`/`HkIatModNtSetValueKey`/`HkIatRegCreateKeyExW`/`HkIatRegSetValueExW` + the shared `RegLogKeyEvent`/`RegLogValueEvent` loggers, so a module that imports the registry pair is covered even if it reaches a shadow `KERNELBASE#1`; thread-local `t_regLogged` snapshot in the `Reg*` handlers keeps it to one line when the KERNELBASE layer has already reported the call. **Inert for artifact.dll — it imports no registry API at all** (measured with `dumpbin /imports`), so a driver's service key is written by services.exe and cannot appear in the game's log |
| direct-ntdll module probes | `HkIatModNtQit` / `HkIatModNtQvif` + `LogQit`, for the probes a module imports straight from ntdll (`NtQueryInformationThread`, `NtQueryVolumeInformationFile`) instead of through KERNELBASE |
| returned data / written bytes | `BytesPreview` — bounded SEH-guarded hex of a caller buffer, used by `qip`/`qit` (what the OS answered), `qvmm` (decoded `MEMORY_BASIC_INFORMATION`), `rvm` (what was read out) and `wvm` (what was written in) |
| census | `CensusBindings`, `CensusAllModules` |
| surveys | `ScanNtdllHooks`(+`ScanNtdllImage`), `ShadowSurvey`/`DecodeExecPrologue`, `LdrSurvey`/`LdrWalkList` (per-list cycle detection) |
| exception chain | `VehChainSurvey`, `VehFindLayout` (runtime layout + `RtlEncodePointer`), `VehHandlerOf`, `VehDescribe` |
| system view | `CpuTopologyInfo` (CPUID + cross-reference, change-detected), `CodeIntegrityInfo` (DSE state via `NtQuerySystemInformation` class 103, change-detected), `DriversScan` (`g_drvMatch`), `InstalledDriversScan` (`DrvIsLoaded` by image basename), `ServicesScan`, `WatchMatch`/`ServiceWatch` |
| hidden modules | `ImageSizeAt` (PE `SizeOfImage`), `g_hiddenReported2`; detected in `ResolveCallerAddrs`, which reports/censuses a hidden module and adds its range, then stops unless `HookHiddenModules` is on (see item 6) |
| monitors | threads, exec memory, network, filesystem watchers (incl. the `.sys` classifier), heartbeat, guard pages, KUSER_SHARED_DATA mailbox |

## Settings groups

* console: `ConsoleVisible`, `AllocConsole` (false = log-file-only), `ConsoleTitle`, `ConsoleCols`, `ConsoleRows`, `ShowTimestamps`, `ShowColors`, `ShowLegend`, `ShowStartupInfo`
* saving/log: `SaveLogFile`, `LogDirectory`, `SaveTaggedLines`, `SaveRawLines`, `WriteTraceFile`, `WriteCrashFile`, `CsvExport`
* capture: `CaptureOutputDebugString`, `RedirectCrt`, `RedirectStdHandles`, `EnableCrashHandler`, `HoldOnCrash` (**freezes** the process on an unhandled crash; shipped ini sets false)
* show gates: one `Show_*` per tag group; `Show_System` gates `[SYS]/[KRNL]/[SVC]`, `[EH]` rides on `Show_Exceptions`, `Show_Hooks` gates `[HOOK]`, and `[SCAN]` has no switch of its own — `CategoryOn` falls through to `Show_LogFiles` for any tag it does not recognise
* hooks: `HookApis`, `HookFileReads`, `HookFileWrites`, `HookMemPatches`, `HookOnlyGameModules`, `HookLegacyNtdll`, `HookModules`, `HookExe` (false), `HookHiddenModules` (false), `MaxHookReports`, `GpaThunks`
* surveys: `CensusAll`, `CensusAllMax`, `ScanShadow`, `ShadowDetailMax`, `ScanLdr`, `ScanOtherHooks`, `ScanOtherHooksMax`, `ScanEh`, `ScanServices`, `ScanDrivers`, `ServiceWatch`, `ServiceWatchAll`, `WatchSharedData`, `MemGuardScan`, `MemGuardAll`
* the remaining groups (modules, log tailing, exceptions, threads, memory, network, files, heartbeat, ODS) live in `README.md`'s settings table with their defaults — keep that table in sync with `Opts[]`

## Output tags and hook kinds

Tags: `[INFO]` `[MOD]` `[THR]` `[MEM]` `[NET]` `[FS]` `[HB]` `[SD]` `[ODS]`
`[EXC]` `[CRASH]` `[EH]` `[HOOK]` `[SCAN]` `[SYS]` `[KRNL]` `[SVC]` `[console]`
`[<file.log>]`, plus `[raw]` for the game's own console output.

Hook kinds on `[HOOK]` lines: `create/open/read/write/…` (file+memory),
`qip/sit/sip/qit/qit(iat)/qvif/setctx/getctx/cthex/avm/rvm/wvm/qvmm/openthr`
(anti-debug, fingerprinting and mutual probing), `veh add|remove`,
`veh add continue`, `ueh set`, `regkey` (CREATED only), `regset`, `syst`
(`NtSetSystemInformation`), `svc openmgr|create|start|delete`, `gpa`, `iat`.
`qit` is logged by two layers; the `(iat)` suffix marks the module's own import
slot, so a hook that is installed but never fires cannot be mistaken for one
that works.

Data now appears on the probe lines, not just the call: `qip`/`qit` append the
first 16 bytes the OS returned, `qvmm` decodes the returned
`MEMORY_BASIC_INFORMATION` (`base/size/state/prot/type`), `rvm` shows the bytes
read out and `wvm` the bytes written in. When adding a hook, prefer reporting the
*returned/transferred data* for queries and writes — "what came back" and "what
was patched" is the point of the tool, and a bare pointer is not evidence.

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
  Its import surface was dumped for PB0 (1.28 MB image, 14 descriptors, 134
  imports) — worth knowing before hooking anything:
  * `advapi32` (9) is **the whole SCM path**: `OpenSCManagerW`, `CreateServiceW`,
    `StartServiceW`, `OpenServiceW`, `ControlService`, `DeleteService`,
    `CloseServiceHandle`, `OpenProcessToken`, `GetTokenInformation`.
  * `ntdll` (8) is **probes only, no registry**: `NtQueryVolumeInformationFile`
    (volume serial/label fingerprinting), `NtQueryInformationThread`,
    `NtReadFile`, `NtOpenFile`, `NtTerminateProcess`, `RtlNtStatusToDosError`,
    `RtlAddVectoredExceptionHandler`, plus one ordinal-bound entry.
  * `kernel32` (87) includes `CreateToolhelp32Snapshot` + `Thread32First/Next`
    (thread enumeration), `OpenThread`, `GetThreadContext`/`SetThreadContext`,
    `CopyFileExW`, `QueryDosDeviceW`, `GetFinalPathNameByHandleW`,
    `SetFileInformationByHandle`, `GetModuleHandleExW`, `CreateProcessW`,
    `ExitProcess`, `TerminateProcess`.
  * **No `Reg*` and no `Nt*Key` at all** — that is why the registry bridge is
    inert here, and why a service key write cannot come from the game process.
* **`dse.dll`** is a hidden helper: in no toolhelp list and no PEB loader list,
  resolvable only by address; it does cross-process `NtWriteVirtualMemory` (2
  writes per target into ~20 processes). Detection is **event-driven** — it shows
  up only when it calls something we instrument.
  Its job (user-confirmed, and consistent with its imports): **switch DSE off**
  so the unsigned `hyperkd.sys` can start, wait for the hypervisor service, then
  **switch DSE back on**. It imports only 21 functions — `advapi32!FreeSid`,
  `Cabinet!FDICopy`, `dbghelp!SymFromNameW`, `KERNEL32!LoadLibraryA`/
  `GetProcAddress`/`VirtualProtect`, the rest CRT — i.e. a self-unpacking stub
  that resolves every ntdll call at run time. That is why hooking its IAT by name
  finds nothing useful, and why the **Code Integrity state** is the reliable
  observable of its work.
* **Hypervisor**: `hyperkd.sys` in `<game>\Artifact_Intel\`, registered under
  service key `artifact` (PZ2), so compare a service's **ImagePath basename**
  with loaded modules, never the key name. Plus Microsoft's `hvservice.sys`.
  `NtLoadDriver`/`ZwLoadDriver` are imported by none of them: the load goes
  through `CreateServiceW` + `StartServiceW` + registry (`ImagePath`/`Type`/`Start`).
  Loaded without the CPUID hypervisor bit (see hard rule 9).
* **The whole install/teardown cycle, observed from outside** (HostWatcher, PB0
  2026-11-16 — three launches). Each launch, in order:
  * three kernel images appear together from `<game>\Artifact_Intel\`:
    `hyperkd.sys` (0x7000), `hyperhv.dll` (0x8A000), `hyperevade.dll` (0x7000) —
    note two of the three carry a `.dll` name while being kernel modules;
  * service `artifact` goes `START_PENDING` → `RUNNING` about 0.6 s later
    (ImagePath is `hyperkd.sys`, so the key name still says nothing about the
    file);
  * **DSE is taken down by loading a vulnerable signed driver, not by a CI API**:
    `RTCORE64.SYS` (0x6000) appears from
    `%TEMP%\dsedll\RTCore64.sys` — MSI Afterburner's known-vulnerable driver, the
    classic BYOVD route to disable driver signature enforcement — and is gone
    again within ~0.6 s (it is only caught when the 500 ms poll lands inside that
    window, which is why it shows in one cycle out of three);
  * on exit: the three images unload, the service goes `RUNNING` → `STOPPED`, and
    the `artifact` key is **removed** ~0.7 s later (`sc query artifact` → 1060),
    so a box that has run a title and then been left alone has no service and no
    loaded driver.
  * Cycle-to-cycle timings from the same log: install 14:16:08.556 → service
    RUNNING 14:16:09.210, teardown 14:16:33.338 → key gone 14:16:33.994.
* **The install happens before GameConsole's worker starts** — that is why no run
  has ever shown `svc create` from inside. In the 14:16 launch the driver was up
  at 14:16:08.5 while the game's first log line is 14:16:12.050, i.e. **3.5 s
  later**, and `GetModuleHandleW(L"dse.dll")` still returned NULL at 12.066 even
  though dse had already done the DSE work. Two readings are possible (our worker
  was starved, or the loader loads `load_dlls` after the driver is up); the new
  `process created … this worker started +N ms later` line answers it on the next
  run. Either way, an in-process capture of the SCM calls needs `gameconsole.dll`
  earlier in `artifact.json`, and until then the watcher is the instrument for
  the install.
* **dse.dll calls back into us ~4 s after the game starts** (~7–8 s after the
  service started): in all three launches its first hooked-API call arrived at
  3.7/4.0/4.4 s and was what got it detected. That call itself is invisible as a
  `[HOOK]` line because the hook path can only queue an address it cannot name —
  which is exactly how the hidden module is found a moment later.
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
* **The module-level ntdll probe hooks are installed but still unexercised**
  (2026-11-16 13:02/13:04). PB0 and PZ2 log
  `(iat) NtQueryInformationThread hooked in artifact.dll` and the same for
  `NtQueryVolumeInformationFile`; FFRS logs neither, because its older artifact
  does not import them at all. After that nothing arrived through those two
  slots: every `qit` line named **PBZ.exe** as the caller (so it came through the
  KERNELBASE layer, class 30 = thread-group information, returning
  `FF FF 00 00 …`), and `qvif` never appeared once. So artifact either calls
  those APIs only on a path these sessions did not take (licence/bind at first
  run) or has cached the pointers in its own table. Do not read "hooked in
  artifact.dll" as "we see artifact's volume queries"; grep `qit(iat)`/`qvif` in
  the log to see whether the module's own slot ever executed.
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
* A mod's helper DLL loads *after* this DLL: unresolved hook targets are logged
  and retried (every ~40 ms during the 3 s startup window, then once a second).
  A hidden module is **not** hooked by default — rewriting the import table of a
  module that hides itself from the loader killed PB0 at that instant, every
  launch that reached it (item 6). Report and census it; `HookHiddenModules=true`
  only when a dying session is acceptable.
* Repointing an import table is a *write*, even though it is not code: for a
  module the tool did not load and cannot name through the loader, treat it as
  tampering and leave it alone. The KERNELBASE layer still sees its calls.
* "Absent from toolhelp32" is **not** enough to call a module hidden: on the
  elevated PB0 run (2026-11-16 20:22) our own GameConsole.dll, ntdll, KERNEL32,
  KERNELBASE and PBZ.exe itself were all labelled hidden, because the toolhelp
  snapshot was stale while the protection was relinking the loader lists. A
  hidden module must be absent from toolhelp32 **and** from the PEB loader lists
  (`InPebLoadList`), and when it is only a stale snapshot the line says
  `ldr: … was absent from the toolhelp32 snapshot - stale snapshot, NOT a hidden
  module` instead. A false hidden report is not cosmetic: it is what the guard in
  item 6 keys off, and it corrupts caller naming.
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

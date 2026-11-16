<#
RunReport.ps1 - summarise one GameConsole log (or compare several).

The recipe in AGENTS.md ("How to read a run") as a script, so two sessions can be
compared without re-typing regexes. Read-only: it only reads logs.

  .\RunReport.ps1                          # newest log under .\logs
  .\RunReport.ps1 -Log <path>              # one log
  .\RunReport.ps1 -Dir <game>\logs         # newest log in that folder
  .\RunReport.ps1 -Log a.log,b.log         # compare (prints one report each)
  .\RunReport.ps1 -Log x.log -Full         # also dump the matching lines

Exit code is 0 for a report, 1 if a log could not be read.
#>
param(
    [string[]]$Log,
    [string]$Dir,
    [switch]$Full
)

function Get-NewestLog([string]$d)
{
    $f = Get-ChildItem -LiteralPath $d -Filter 'GameConsole_*.log' -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $f) { return $null }
    return $f.FullName
}

function Show($lines, [string]$pattern, [string]$title, [int]$first)
{
    $hit = @($lines | Where-Object { $_ -match $pattern })
    Write-Host ("  {0}: {1}" -f $title, $hit.Count)
    $n = 0
    foreach ($l in $hit) { if (-not $Full -and $n -ge $first) { Write-Host "        ... ($($hit.Count - $first) more)"; break }; Write-Host "        $($l.Trim())"; $n++ }
}

function Report([string]$path)
{
    if (-not (Test-Path -LiteralPath $path)) { Write-Host "cannot read $path"; return $false }
    $lines = @(Get-Content -LiteralPath $path)
    $info = [System.IO.FileInfo]$path

    Write-Host ""
    Write-Host ("=" * 78)
    Write-Host "GameConsole run report"
    Write-Host "  log    : $path"
    Write-Host "  size   : $([math]::Round($info.Length/1KB,1)) KB, $($lines.Count) lines, written $($info.LastWriteTime)"
    Write-Host ("=" * 78)

    # --- integrity (a good log never contains an ESC byte) -------------------
    $esc = @($lines | Where-Object { $_.Contains([char]27) }).Count
    $scripts = @($lines | Where-Object { $_ -match '\[HOOK\]\s+\S+\s*<- 0x' }).Count
    Write-Host ""
    Write-Host "Integrity"
    Write-Host "  ESC (0x1B) bytes      : $esc  (must be 0)"
    Write-Host "  unattributed callers  : $scripts  ('<- 0x...' with no module name)"

    # --- which modules got IAT hooks ----------------------------------------
    Write-Host ""
    Write-Host "Hook surface"
    $lines | Where-Object { $_ -match '\[INFO\] hooks:' } | ForEach-Object { Write-Host "  $($_.Trim())" }
    $lines | Where-Object { $_ -match 'interposition layer' } | Select-Object -First 1 | ForEach-Object { Write-Host "  $($_.Trim())" }
    $lines | Where-Object { $_ -match '\(iat\) .*(loaded late|located by address|is not loaded yet)' } |
        ForEach-Object { Write-Host "  $($_.Trim())" }
    $kinds = @{}
    foreach ($l in $lines)
    {
        if ($l -match '\[HOOK\]\s+(\S+)')
        {
            $k = $Matches[1]
            if ($k -notmatch '^\(iat\)$') { $kinds[$k] = 1 + $(if ($kinds.ContainsKey($k)) { $kinds[$k] } else { 0 }) }
        }
    }
    $top = $kinds.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 14
    if ($top) { Write-Host ("  hook kinds            : " + (($top | ForEach-Object { "$($_.Key) x$($_.Value)" }) -join ', ')) }

    # --- the driver install (outstanding item 1) ----------------------------
    Write-Host ""
    Write-Host "Driver / service install trace"
    $regkey = @($lines | Where-Object { $_ -match '\[HOOK\] regkey' })
    $regset = @($lines | Where-Object { $_ -match '\[HOOK\] regset' })
    $svc    = @($lines | Where-Object { $_ -match '\[HOOK\] svc ' })
    $syst   = @($lines | Where-Object { $_ -match '\[HOOK\] syst' })
    $sysfile= @($lines | Where-Object { $_ -match '\[FS\].*\.sys' })
    Write-Host "  regkey CREATED        : $($regkey.Count)"
    Write-Host "  regset (values)       : $($regset.Count)"
    Write-Host "  svc (SCM calls)       : $($svc.Count)"
    Write-Host "  syst (NtSetSystemInfo): $($syst.Count)"
    Write-Host "  [FS] .sys writes      : $($sysfile.Count)"
    $installLines = @($regkey + $regset + $svc + $syst)
    if ($installLines.Count -eq 0)
    {
        Write-Host "  verdict: NOTHING captured. If the driver was already installed before this"
        Write-Host "           run this is expected: delete HKLM\SYSTEM\CurrentControlSet\Services\artifact,"
        Write-Host "           reboot so hyperkd.sys unloads, then run a title (AGENTS.md item 1)."
    }
    else
    {
        Write-Host "  verdict: install activity captured - read it top to bottom:"
        foreach ($l in ($installLines | Select-Object -First 12)) { Write-Host "        $($l.Trim())" }
    }
    if ($Full) { Show $lines '\[FS\].*\.sys' '[FS] .sys' 40 }

    # --- loader-list tampering / hidden modules -----------------------------
    Write-Host ""
    Write-Host "Loader lists and hidden modules"
    $lines | Where-Object { $_ -match '\[SCAN\] ldr survey' } | ForEach-Object { Write-Host "  $($_.Trim())" }
    $cycles = @($lines | Where-Object { $_ -match '\[SCAN\] ldr: .*cycles after' })
    Write-Host "  cycle warnings        : $($cycles.Count)"
    if ($cycles.Count) { Write-Host "        (first) $($cycles[0].Trim())" }
    $ghosts = @($lines | Where-Object { $_ -match '\[SCAN\] ldr (ghost|non-image)' })
    Write-Host "  ghost/non-image entry : $($ghosts.Count)"
    foreach ($l in ($ghosts | Select-Object -First 5)) { Write-Host "        $($l.Trim())" }
    $hidden = @($lines | Where-Object { $_ -match 'hidden module:' })
    Write-Host "  hidden modules        : $($hidden.Count)"
    foreach ($l in $hidden) { Write-Host "        $($l.Trim())" }

    # --- exception chain ----------------------------------------------------
    Write-Host ""
    Write-Host "Vectored exception chain"
    $lines | Where-Object { $_ -match '\[EH\] veh chain(\[|\: \d)' } |
        ForEach-Object { Write-Host "  $($_.Trim())" }

    # --- system view --------------------------------------------------------
    Write-Host ""
    Write-Host "Hypervisor / kernel / services"
    $lines | Where-Object { $_ -match '\[SYS\] hypervisor' } | ForEach-Object { Write-Host "  $($_.Trim())" }
    $lines | Where-Object { $_ -match '\[KRNL\]' } | Select-Object -First 8 | ForEach-Object { Write-Host "  $($_.Trim())" }
    $lines | Where-Object { $_ -match '\[SVC\] (installed driver|service:)' } | Select-Object -First 8 | ForEach-Object { Write-Host "  $($_.Trim())" }

    # --- anti-debug / mutual probing ---------------------------------------
    Write-Host ""
    Write-Host "Anti-debug and mutual probing"
    Show $lines '\[HOOK\] (qip|sit|qit|sip|cthex|setctx|getctx|ntsetctx|ntgetctx|avm|rvm|wvm|qvmm|openthr)\b' 'probe lines' 8

    # --- shadow / census ----------------------------------------------------
    Write-Host ""
    Write-Host "Shadow copies and binding census"
    Show $lines '\[SCAN\] .*(shadow|#1 @)' 'shadow lines' 6
    $tally = @($lines | Where-Object { $_ -match 'tally sums to (\d+)' })
    $bad = 0
    foreach ($l in $tally)
    {
        if ($l -match '(\d+) imports .*tally sums to (\d+)' -and $Matches[1] -ne $Matches[2]) { $bad++ }
    }
    Write-Host "  census lines          : $($tally.Count) (mismatched tallies: $bad)"
    return $true
}

$ok = $true
$targets = @()
if ($Log) { $targets = $Log }
else
{
    if (-not $Dir) { $Dir = Join-Path (Get-Location) 'logs' }
    $newest = Get-NewestLog $Dir
    if (-not $newest) { Write-Host "no GameConsole_*.log under $Dir - pass -Log or -Dir"; exit 1 }
    $targets = @($newest)
}
foreach ($t in $targets) { if (-not (Report $t)) { $ok = $false } }
if (-not $ok) { exit 1 }

param(
    [Parameter(Mandatory=$true)][string]$Path,     # the DLL carrying the handler
    [Parameter(Mandatory=$true)][int]$Rva,         # handler offset as printed by "veh chain" (module base relative)
    [string]$AlsoSearch                            # optional second image: byte-search it for the same function
)
# Read-only static look at a VEH handler whose only name is <module>+0xRVA
# (as printed by the "[EH] veh chain[n]" line). Says which section it lives in,
# which export (if any) contains or precedes it, where the function probably
# starts (int3/nop padding is the only reliable boundary in a stripped image),
# and can look for the same function bytes in another build of the module.
$ErrorActionPreference = 'Stop'
$b = [IO.File]::ReadAllBytes($Path)
function U16($o) { [BitConverter]::ToUInt16($b, $o) }
function U32($o) { [BitConverter]::ToUInt32($b, $o) }
$lfanew = [int](U32 0x3C)
if ((U32 $lfanew) -ne 0x00004550) { "not PE"; exit }
$coff = $lfanew + 4
$nSec = U16 ($coff + 2)
$optSize = U16 ($coff + 16)
$opt = $coff + 20
$magic = U16 $opt
$ddOff = if ($magic -eq 0x20b) { $opt + 112 } else { $opt + 96 }
$expRva = U32 $ddOff; $expSize = U32 ($ddOff + 4)
$secOff = $opt + $optSize
$secs = @()
for ($i = 0; $i -lt $nSec; $i++) {
    $s = $secOff + $i * 40
    $name = ([Text.Encoding]::ASCII.GetString($b, $s, 8)).TrimEnd([char]0)
    $secs += [pscustomobject]@{ Name=$name; Va=U32 ($s+12); Vsz=U32 ($s+8); Raw=U32 ($s+20); Rsz=U32 ($s+16); Char=U32 ($s+36) }
}
function RvaToOff($rva) {
    foreach ($s in $secs) { if ($rva -ge $s.Va -and $rva -lt ($s.Va + [Math]::Max($s.Vsz, $s.Rsz))) { return $rva - $s.Va + $s.Raw } }
    return -1
}
"file: $Path  ($($b.Length) bytes)"
"target rva: 0x$('{0:X}' -f $Rva)"
$sec = $secs | Where-Object { $Rva -ge $_.Va -and $Rva -lt ($_.Va + $_.Vsz) } | Select-Object -First 1
if ($sec) {
    "section:   $($sec.Name)  (rva 0x$('{0:X}' -f $sec.Va) size 0x$('{0:X}' -f $sec.Vsz), raw 0x$('{0:X}' -f $sec.Raw), chars 0x$('{0:X8}' -f $sec.Char))"
    "offset in section: +0x$('{0:X}' -f ($Rva - $sec.Va))"
} else { "section:   (not found for this rva)" }

# nearest export at/before the target (export table may be stripped in a
# protected build - say so rather than guess)
$expDir = RvaToOff $expRva
if ($expRva -and $expDir -ge 0) {
    $nFunc = U32 ($expDir + 0x18); $nNames = U32 ($expDir + 0x1C)
    $aFunc = RvaToOff (U32 ($expDir + 0x20))
    $aName = RvaToOff (U32 ($expDir + 0x24))
    $aOrd  = RvaToOff (U32 ($expDir + 0x28))
    "exports:   $nFunc functions, $nNames named (dir rva 0x$('{0:X}' -f $expRva))"
    $best = -1; $bestName = $null; $bestFwd = $null
    for ($i = 0; $i -lt $nFunc; $i++) {
        $f = U32 ($aFunc + $i * 4)
        if ($f -eq 0) { continue }
        if ($f -ge $expRva -and $f -lt ($expRva + $expSize)) { continue }   # forwarder
        if ($f -le $Rva -and $f -gt $best) {
            $best = $f
            $bestName = $null
            for ($j = 0; $j -lt $nNames; $j++) {
                if ((U16 ($aOrd + $j * 2)) -eq $i) {
                    $nOff = RvaToOff (U32 ($aName + $j * 4))
                    $e2 = $nOff; while ($b[$e2] -ne 0) { $e2++ }
                    $bestName = [Text.Encoding]::ASCII.GetString($b, $nOff, $e2 - $nOff)
                    break
                }
            }
        }
    }
    if ($best -ge 0) {
        $inside = if ($bestName) { "$bestName (ordinal export)" } else { "(unnamed ordinal)" }
        "nearest export at/before target: 0x$('{0:X}' -f $best) -> $inside  (target is +0x$('{0:X}' -f ($Rva - $best)) into it)"
    } else { "nearest export at/before target: none" }
} else { "exports:   (export directory absent - stripped build)" }

# probable function start: walk back over int3/nop padding, then dump prologue
$off = RvaToOff $Rva
if ($off -ge 0) {
    $start = $off
    for ($k = 1; $k -le 0x400 -and ($off - $k) -ge 0; $k++) {
        $c = $b[$off - $k]
        if ($c -eq 0xCC -or $c -eq 0x90) { $start = $off - $k + 1; break }
    }
    $fnRva = $Rva - ($off - $start)
    "handler bytes at target:"
    "  0x$('{0:X}' -f $Rva): " + (($b[$off..($off+23)] | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
    "probable function start: rva 0x$('{0:X}' -f $fnRva) (module+0x$('{0:X}' -f $fnRva))"
    "  prologue: " + (($b[$start..($start+31)] | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
}

# optional: the same function bytes in another image (private-list twin)
if ($AlsoSearch) {
    $b2 = [IO.File]::ReadAllBytes($AlsoSearch)
    "searching $AlsoSearch ($($b2.Length) bytes) for the handler prologue (64 bytes at target)..."
    if ($off -ge 0 -and $b2.Length -gt 64) {
        $pat = $b[$off..($off+63)]
        $found = 0
        for ($i = 0; $i -le $b2.Length - 64; $i++) {
            $ok = $true
            for ($j = 0; $j -lt 64; $j += 4) {   # stride 4: tolerate relocated immediates
                if ($b2[$i+$j] -ne $pat[$j] -or $b2[$i+$j+1] -ne $pat[$j+1] -or $b2[$i+$j+2] -ne $pat[$j+2] -or $b2[$i+$j+3] -ne $pat[$j+3]) { $ok = $false; break }
            }
            if ($ok) {
                $found++
                # file offset -> rva via the second image's sections is not done
                # here (this search runs on raw bytes); report the offset itself
                "  match at file offset 0x$('{0:X}' -f $i)"
                if ($found -ge 8) { "  (stopping at 8 matches)"; break }
            }
        }
        if (-not $found) { "  no match - the handler bytes differ in this image" }
    }
}

param([string]$Path)
# Read-only PE section table with per-section entropy: tells a packed/encrypted
# section (entropy ~7.9+) from plain code/data (x86 code ~5-6.5) in one glance.
$b = [IO.File]::ReadAllBytes($Path)
function U16($o) { [BitConverter]::ToUInt16($b, $o) }
function U32($o) { [BitConverter]::ToUInt32($b, $o) }
$lfanew = [int](U32 0x3C)
if ((U32 $lfanew) -ne 0x00004550) { "not PE"; exit }
$coff = $lfanew + 4
$nSec = U16 ($coff + 2)
$optSize = U16 ($coff + 16)
$secOff = ($coff + 20) + $optSize
"file: $Path  ($($b.Length) bytes)"
for ($i = 0; $i -lt $nSec; $i++) {
    $s = $secOff + $i * 40
    $nm = ([Text.Encoding]::ASCII.GetString($b, $s, 8)).TrimEnd([char]0)
    $vsz = U32 ($s + 8); $raw = U32 ($s + 20); $rsz = U32 ($s + 16); $ch = U32 ($s + 36)
    $ent = 0.0; $uniq = 0
    if ($rsz -gt 0) {
        $slice = $b[$raw..($raw + $rsz - 1)]
        $uniq = ($slice | Sort-Object -Unique).Count
        $ent = [Math]::Round(-(($slice | Group-Object | ForEach-Object { $p = $_.Count / $slice.Length; $p * [Math]::Log($p, 2) }) | Measure-Object -Sum).Sum, 2)
    }
    '{0,-9} vsz=0x{1:X6} raw=0x{2:X6} rsz=0x{3:X6} chars=0x{4:X8} uniq={5} entropy={6}' -f $nm, $vsz, $raw, $rsz, $ch, $uniq, $ent
}

param(
    [Parameter(Mandatory=$true)][string]$Path,
    [switch]$Exports
)
# Read-only import-name dump (per DLL, every imported function name), and with
# -Exports every export name too. Complements peimports.ps1 (which proves the
# census counts) by naming the surface: for a kernel image this is the list of
# kernel APIs it can touch at all.
$b = [IO.File]::ReadAllBytes($Path)
function U16($o) { if ($o -lt 0 -or $o -gt $b.Length - 2) { return 0 }; [BitConverter]::ToUInt16($b, $o) }
function U32($o) { if ($o -lt 0 -or $o -gt $b.Length - 4) { return 0 }; [BitConverter]::ToUInt32($b, $o) }
function U64($o) { if ($o -lt 0 -or $o -gt $b.Length - 8) { return 0 }; [BitConverter]::ToUInt64($b, $o) }
function CStr($off) {
    if ($off -lt 0 -or $off -ge $b.Length) { return "" }
    $e = $off; while ($e -lt $b.Length -and $b[$e] -ne 0) { $e++ }
    [Text.Encoding]::ASCII.GetString($b, $off, $e - $off)
}
$lfanew = [int](U32 0x3C)
if ((U32 $lfanew) -ne 0x00004550) { "not PE"; exit }
$coff = $lfanew + 4
$nSec = U16 ($coff + 2)
$optSize = U16 ($coff + 16)
$opt = $coff + 20
$magic = U16 $opt
$ddOff = if ($magic -eq 0x20b) { $opt + 112 } else { $opt + 96 }
$secOff = $opt + $optSize
$secs = @()
for ($i = 0; $i -lt $nSec; $i++) {
    $s = $secOff + $i * 40
    $secs += [pscustomobject]@{ Va = U32 ($s + 12); Vsz = U32 ($s + 8); Raw = U32 ($s + 20); Rsz = U32 ($s + 16) }
}
function RvaToOff($rva) {
    foreach ($s in $secs) { if ($rva -ge $s.Va -and $rva -lt ($s.Va + [Math]::Max($s.Vsz, $s.Rsz))) { return $rva - $s.Va + $s.Raw } }
    return -1
}
"file: $Path"
$impRva = U32 ($ddOff + 8)
if ($impRva) {
    $d = RvaToOff $impRva
    for ($k = 0; ; $k++) {
        $off = $d + $k * 20
        $oft = U32 $off; $nameRva = U32 ($off + 12); $ft = U32 ($off + 16)
        if ($nameRva -eq 0) { break }
        $dll = CStr (RvaToOff $nameRva)
        $names = @()
        $src = if ($oft -ne 0) { $oft } else { $ft }
        $o = RvaToOff $src
        if ($o -ge 0) {
            for ($j = 0; (U64 ($o + $j * 8)) -ne 0; $j++) {
                $h = U64 ($o + $j * 8)
                if ($h -band 0x8000000000000000) { $names += "#$($h -band 0xFFFF)" }
                else { $hn = RvaToOff ([int]($h -band 0xFFFFFF)); $names += CStr ($hn + 2) }
            }
        }
        "  $dll ($($names.Count)):"
        "    " + ($names -join ', ')
    }
}
if ($Exports) {
    $expRva = U32 $ddOff
    $expDir = RvaToOff $expRva
    if ($expRva -and $expDir -ge 0) {
        $nFunc = U32 ($expDir + 0x18); $nNames = U32 ($expDir + 0x1C)
        $aName = RvaToOff (U32 ($expDir + 0x24)); $aOrd = RvaToOff (U32 ($expDir + 0x28)); $aFunc = RvaToOff (U32 ($expDir + 0x20))
        "  exports ($nFunc functions, $nNames named):"
        if ($nNames -lt 0x10000 -and $aName -ge 0 -and $aOrd -ge 0) {
            for ($j = 0; $j -lt $nNames; $j++) {
                $ord = U16 ($aOrd + $j * 2); $frva = U32 ($aFunc + $ord * 4)
                $no = RvaToOff (U32 ($aName + $j * 4))
                "    0x$('{0:X}' -f $frva)  $(CStr $no)"
            }
        } else { "    (names count $nNames is garbage - corrupt export directory)" }
    }
}

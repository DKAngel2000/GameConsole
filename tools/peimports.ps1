param([string]$Path)
$b = [IO.File]::ReadAllBytes($Path)
function U16($o) { [BitConverter]::ToUInt16($b, $o) }
function U32($o) { [BitConverter]::ToUInt32($b, $o) }
function U64($o) { [BitConverter]::ToUInt64($b, $o) }
$lfanew = [int](U32 0x3C)
if ((U32 $lfanew) -ne 0x00004550) { "not PE"; exit }
$coff = $lfanew + 4
$nSec = U16 ($coff + 2)
$optSize = U16 ($coff + 16)
$opt = $coff + 20
$magic = U16 $opt
$ddOff = if ($magic -eq 0x20b) { $opt + 112 } else { $opt + 96 }
$impRva = U32 ($ddOff + 8)      # data dir 1
$impSize = U32 ($ddOff + 12)
$secOff = $opt + $optSize
$secs = @()
for ($i = 0; $i -lt $nSec; $i++) {
    $s = $secOff + $i * 40
    $secs += [pscustomobject]@{
        Va = U32 ($s + 12); Vsz = U32 ($s + 8); Raw = U32 ($s + 20); Rsz = U32 ($s + 16)
    }
}
function RvaToOff($rva) {
    foreach ($s in $secs) { if ($rva -ge $s.Va -and $rva -lt $s.Va + [Math]::Max($s.Vsz, $s.Rsz)) { return $rva - $s.Va + $s.Raw } }
    return $rva
}
function CStr($off) { $e = $off; while ($b[$e] -ne 0) { $e++ }; [Text.Encoding]::ASCII.GetString($b, $off, $e - $off) }
"file: $Path"
"import dir rva=0x$('{0:X}' -f $impRva) size=$impSize"
$d = RvaToOff $impRva
$totalNT = 0; $totalIAT = 0; $desc = 0
for ($k = 0; ; $k++) {
    $off = $d + $k * 20
    $oft = U32 $off
    $nameRva = U32 ($off + 12)
    $ft = U32 ($off + 16)
    if ($oft -eq 0 -and $nameRva -eq 0 -and $ft -eq 0) { break }
    if ($nameRva -eq 0) { break }
    $dll = CStr (RvaToOff $nameRva)
    $ntCount = 0
    if ($oft -ne 0) {
        $o = RvaToOff $oft
        while ((U64 ($o + $ntCount * 8)) -ne 0) { $ntCount++ }
    }
    $iatCount = 0
    $fo = RvaToOff $ft
    while ((U64 ($fo + $iatCount * 8)) -ne 0) { $iatCount++ }
    $desc++
    $totalNT += $ntCount; $totalIAT += $iatCount
    "{0,2}. {1,-42} NameTable={2,4}  IAT={3,4}{4}" -f $desc, $dll, $ntCount, $iatCount, $(if ($ntCount -ne $iatCount) { "   <-- DIFF" } else { "" })
}
"descriptors: $desc   NameTable total: $totalNT   IAT total: $totalIAT"

// tools/KmodProbe.c - one-shot diagnostic: dump the kernel module list the way
// GameConsole parses it, and flag entries whose ImageBase is not a canonical
// kernel address.
//
// Why: the [KRNL] line started printing `hyperkd.sys @0x7465536C6F72746E` - which
// decodes as the ASCII bytes "nrolSet" (a fragment of "CurrentControlSet"), i.e.
// text, not an address. Either our struct layout is wrong, Windows obfuscates the
// field, or the protection's driver filters NtQuerySystemInformation to spoof its
// own entry. This prints every entry so the three cases can be told apart: a
// layout bug garbles many entries, obfuscation garbles the module being hidden,
// and a plausible spread of canonical addresses everywhere else means the list is
// selectively spoofed.
//
//   cl /nologo /O2 /W3 KmodProbe.c /Fe:KmodProbe.exe
//
// Read-only: one NtQuerySystemInformation call, no handles, no writes.

#include <windows.h>
#include <stdio.h>

typedef LONG (WINAPI* t_NtQSI)(ULONG, void*, ULONG, ULONG*);

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
} KM_MOD;
typedef struct { ULONG NumberOfModules; KM_MOD Modules[1]; } KM_MODS;

static int Canonical(ULONG_PTR a)
{
    // x64 kernel space starts at 0xFFFF800000000000; driver images live high.
    return a >= 0xFFFF800000000000ull && a != (ULONG_PTR)-1;
}

int wmain(void)
{
    static BYTE buf[16 << 20];
    ULONG ret = 0, i;
    KM_MODS* list = (KM_MODS*)buf;
    t_NtQSI q = (t_NtQSI)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                               "NtQuerySystemInformation");
    int bad = 0, total = 0;
    if (!q) { wprintf(L"ntdll!NtQuerySystemInformation not found\n"); return 1; }
    if (q(11, buf, sizeof(buf), &ret) != 0) { wprintf(L"query failed\n"); return 1; }
    total = (int)list->NumberOfModules;
    wprintf(L"%d kernel modules; ImageBase field as parsed:\n", total);
    for (i = 0; i < (ULONG)total; i++)
    {
        KM_MOD* m = &list->Modules[i];
        WCHAR path[256];
        ULONG_PTR base = (ULONG_PTR)m->ImageBase;
        MultiByteToWideChar(CP_ACP, 0, (const char*)m->FullPathName, -1, path, 256);
        if (!Canonical(base))
        {
            bad++;
            wprintf(L"  NOT-CANONICAL @0x%016llX size 0x%-7X order %-4u %s\n",
                    (unsigned long long)base, m->ImageSize, (unsigned)m->LoadOrderIndex, path);
        }
    }
    wprintf(L"%d of %d entries have a non-canonical ImageBase\n", bad, total);
    wprintf(L"(all-canonical = normal; only a few = the list is being spoofed for those)\n");
    return 0;
}

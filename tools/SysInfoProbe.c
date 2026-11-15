// SysInfoProbe.c - throwaway probe: which NtQuerySystemInformation classes does
// this Windows build support, and what do the hypervisor-related ones return?
// Used to pin the class numbers GameConsole reports on (they are undocumented,
// so they are discovered here rather than guessed).
#include <windows.h>
#include <stdio.h>

typedef LONG (WINAPI* t_NtQSI)(ULONG, PVOID, ULONG, PULONG);

int main(void)
{
    t_NtQSI q = (t_NtQSI)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    static BYTE buf[8192];
    ULONG cls;
    int i;
    if (!q) { printf("no NtQuerySystemInformation\n"); return 1; }
    printf("class  status      ret   dwords\n");
    for (cls = 0; cls <= 0x120; cls++)
    {
        ULONG ret = 0;
        LONG st;
        memset(buf, 0, 64);
        st = q(cls, buf, sizeof(buf), &ret);
        if (st == 0)                        /* STATUS_SUCCESS */
        {
            printf("0x%03X  0x%08X  %-5lu", cls, (unsigned)st, (unsigned long)ret);
            for (i = 0; i < 6; i++) printf(" %08X", ((ULONG*)buf)[i]);
            printf("\n");
        }
    }
    return 0;
}

// vehprobe - throwaway diagnostic: is the VEH node's handler slot an
// RtlEncodePointer value?
#include <windows.h>
#include <stdio.h>
#include <intrin.h>

static volatile int v1, v2;
#pragma optimize("", off)
static LONG NTAPI H1(EXCEPTION_POINTERS* p) { v1 = 1; (void)p; return EXCEPTION_CONTINUE_SEARCH; }
static LONG NTAPI H2(EXCEPTION_POINTERS* p) { v2 = 2; (void)p; return EXCEPTION_CONTINUE_SEARCH; }
#pragma optimize("", on)

int main(void)
{
    typedef PVOID (WINAPI* t_Dec)(PVOID);
    typedef PVOID (WINAPI* t_Enc)(PVOID);
    t_Dec dec = (t_Dec)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlDecodePointer");
    t_Enc enc = (t_Enc)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlEncodePointer");
    PVOID c1, c2;
    int off;
    printf("H1=%p H2=%p dec=%p\n", (void*)H1, (void*)H2, (void*)dec);
    printf("enc(H1)=%p enc(H2)=%p\n", enc ? enc((PVOID)(ULONG_PTR)H1) : 0, enc ? enc((PVOID)(ULONG_PTR)H2) : 0);
    c1 = AddVectoredExceptionHandler(1, H1);
    c2 = AddVectoredExceptionHandler(0, H2);
    for (off = 0; off < 0x40; off += 8)
    {
        unsigned long long a = 0, b = 0;
        __try { a = *(unsigned long long*)((char*)c1 + off); } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        __try { b = *(unsigned long long*)((char*)c2 + off); } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        printf("+%02X  c1=%016llX dec=%016llX   c2=%016llX dec=%016llX%s%s\n", off, a,
               (unsigned long long)(ULONG_PTR)(dec ? dec((PVOID)(ULONG_PTR)a) : 0), b,
               (unsigned long long)(ULONG_PTR)(dec ? dec((PVOID)(ULONG_PTR)b) : 0),
               (ULONG_PTR)dec && (ULONG_PTR)dec((PVOID)(ULONG_PTR)a) == (ULONG_PTR)H1 ? "  <= H1" : "",
               (ULONG_PTR)dec && (ULONG_PTR)dec((PVOID)(ULONG_PTR)b) == (ULONG_PTR)H2 ? "  <= H2" : "");
    }
    if (c1) RemoveVectoredExceptionHandler(c1);
    if (c2) RemoveVectoredExceptionHandler(c2);
    return 0;
}

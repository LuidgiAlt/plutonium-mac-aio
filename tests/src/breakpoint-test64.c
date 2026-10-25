/* 64-bit side of the breakpoint emulation (patch 0002), with the
 * WINE_ROSETTA_* variables set as launch.sh sets them for every process.
 *   x64-remote-syscall  a thread in NtDelayExecution is suspended, has its
 *                       context read and set back, and resumed 1000 times:
 *                       every NtDelayExecution returns STATUS_SUCCESS and no
 *                       exception reaches the thread
 *   x64-idle-cost       an armed thread (breakpoint on code that never runs)
 *                       making 20000 NtYieldExecution calls: at most 1 ms per
 *                       call. Stepping its own code costs ~0.3 ms per call
 *                       under Rosetta; stepping through Wine's unix code as
 *                       well cost 3.4 ms.
 * Build: x86_64-w64-mingw32-gcc -O2 -o breakpoint-test64.exe breakpoint-test64.c */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "aio_test.h"

static volatile LONG exceptions, bad_status, done;
static LONG (WINAPI *pNtDelayExecution)(BOOLEAN, const LARGE_INTEGER *);
static LONG (WINAPI *pNtYieldExecution)(void);

__attribute__((noinline)) static void never_called(void) { InterlockedIncrement(&exceptions); }

static LONG CALLBACK handler(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) InterlockedIncrement(&exceptions);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI delayer(void *arg)
{
    LARGE_INTEGER interval = {{ -10000, -1 }};   /* 1 ms */
    while (!done)
        if (pNtDelayExecution(FALSE, &interval)) InterlockedIncrement(&bad_status);
    return 0;
}

static ULONG64 now_us(void)
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return c.QuadPart * 1000000 / f.QuadPart;
}

static ULONG64 yield_loop_us(BOOL armed)
{
    CONTEXT c;
    ULONG64 t0;
    int i;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    c.Dr0 = armed ? (DWORD64)never_called : 0;
    c.Dr7 = armed ? 1 : 0;
    SetThreadContext(GetCurrentThread(), &c);
    t0 = now_us();
    for (i = 0; i < 20000; i++) pNtYieldExecution();
    return now_us() - t0;
}

static ULONG64 loop_us;
static DWORD WINAPI armed_loop(void *arg) { loop_us = yield_loop_us((BOOL)(ULONG_PTR)arg); return 0; }

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "x64-remote-syscall";
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    HANDLE t;

    pNtDelayExecution = (void *)GetProcAddress(ntdll, "NtDelayExecution");
    pNtYieldExecution = (void *)GetProcAddress(ntdll, "NtYieldExecution");
    AddVectoredExceptionHandler(1, handler);

    if (!strcmp(name, "x64-remote-syscall"))
    {
        CONTEXT c;
        int i;
        t = CreateThread(NULL, 0, delayer, NULL, 0, NULL);
        Sleep(50);
        for (i = 0; i < 1000; i++)
        {
            SuspendThread(t);
            memset(&c, 0, sizeof(c));
            c.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
            GetThreadContext(t, &c);
            SetThreadContext(t, &c);
            ResumeThread(t);
        }
        done = 1;
        WaitForSingleObject(t, INFINITE);
        return report(!bad_status && !exceptions, "x64-remote-syscall: %ld bad NtDelayExecution results, %ld exceptions",
                      bad_status, exceptions);
    }
    if (!strcmp(name, "x64-idle-cost"))
    {
        ULONG64 unarmed, armed;
        t = CreateThread(NULL, 0, armed_loop, (void *)FALSE, 0, NULL);
        WaitForSingleObject(t, INFINITE);
        unarmed = loop_us;
        t = CreateThread(NULL, 0, armed_loop, (void *)TRUE, 0, NULL);
        WaitForSingleObject(t, INFINITE);
        armed = loop_us;
        return report(armed / 20000 <= 1000 && !exceptions,
                      "x64-idle-cost: armed %llu us/call, unarmed %llu us/call (max 1000 armed), %ld exceptions",
                      armed / 20000, unarmed / 20000, exceptions);
    }
    return report(0, "unknown case %s", name);
}

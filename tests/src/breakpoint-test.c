/* Hardware execution breakpoints (DR0/DR7) as Plutonium uses them, emulated
 * by patches 0001/0002 under Rosetta. One case per run, named by argv[1]:
 *   self             the thread arms itself, then calls target(): 1 hit
 *   remote-blocked   another thread arms a thread blocked in Sleep: 1 hit
 *   suspended-start  armed before ResumeThread of a CREATE_SUSPENDED thread
 *   disarm           armed and hit, then disarmed by another thread while it
 *                    waits: its next loop runs at unarmed speed (<= 1.5x)
 *   rearm            disarmed by itself, re-armed by another thread while it
 *                    runs user code: the next call hits
 *   resume-at        a thread suspended in user code is resumed with EIP = an
 *                    armed address, RF clear: the breakpoint fires before
 *                    the first instruction runs
 *   global-bit       DR7 with only G0 set (no L0) still arms the breakpoint
 *   syscall-load     an armed thread alternates 1000 syscalls and calls:
 *                    every call hits (the 64-bit part of each syscall is
 *                    skipped)
 *   pushfd           an armed thread's pushfd does not show the emulated TF
 *   app-tf           on an armed thread, the application's own TF raises one
 *                    single-step (BS)
 *   popfd-keeps      a popfd that clears TF does not end the emulation
 * A vectored handler counts EXCEPTION_SINGLE_STEP with DR6 bit 0 and resumes
 * with RF set. A watchdog fails a case that hangs.
 * Build: i686-w64-mingw32-gcc -O2 -o breakpoint-test.exe breakpoint-test.c */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "aio_test.h"

static volatile LONG hits, calls, phase, app_steps;
static HANDLE ev_a, ev_b;

__attribute__((noinline)) static void target(void)
{
    InterlockedIncrement(&calls);
    __asm__ volatile ("" ::: "memory");
}

__attribute__((noinline)) static DWORD WINAPI resume_target(void *arg)
{
    InterlockedIncrement(&calls);
    ExitThread(0);
}

static LONG CALLBACK handler(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (e->ContextRecord->Dr6 & 1) InterlockedIncrement(&hits);
    else if (e->ContextRecord->Dr6 & 0x4000) InterlockedIncrement(&app_steps);
    e->ContextRecord->EFlags |= 0x10000;   /* RF: run the instruction this time */
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI watchdog(void *arg)
{
    Sleep(20000);
    report(0, "%s: hung (hits %ld, calls %ld, phase %ld)", (const char *)arg, hits, calls, phase);
    ExitProcess(1);
}

/* Arm (addr) or disarm (NULL) DR0 of a thread; another thread is suspended
 * around it, as debuggers and Plutonium do. dr7 is the enable value. */
static BOOL set_dr0(HANDLE thread, void *addr, DWORD dr7, BOOL suspend)
{
    CONTEXT c;
    BOOL ok;

    if (suspend && SuspendThread(thread) == (DWORD)-1) return FALSE;
    memset(&c, 0, sizeof(c));
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    ok = GetThreadContext(thread, &c);
    c.Dr0 = (DWORD)(ULONG_PTR)addr;
    c.Dr7 = addr ? dr7 : 0;
    ok = ok && SetThreadContext(thread, &c);
    if (suspend) ResumeThread(thread);
    return ok;
}

static ULONG64 thread_cpu_us(HANDLE thread)
{
    FILETIME c, x, k, u;
    GetThreadTimes(thread, &c, &x, &k, &u);
    return ((((ULONG64)k.dwHighDateTime << 32) | k.dwLowDateTime) +
            (((ULONG64)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10;
}

static volatile ULONG sink;
static void spin_loop(void)
{
    ULONG i;
    for (i = 0; i < 20000000; i++) sink += i;
}

static ULONG64 loop_us;
static DWORD WINAPI timed_loop(void *arg)
{
    ULONG64 t0 = thread_cpu_us(GetCurrentThread());
    spin_loop();
    loop_us = thread_cpu_us(GetCurrentThread()) - t0;
    return 0;
}

static DWORD WINAPI sleeper(void *arg)  { Sleep(500); target(); return 0; }
static DWORD WINAPI spinner(void *arg)  { InterlockedExchange(&phase, 1); for (;;) sink++; }
static DWORD WINAPI caller(void *arg)   { target(); return 0; }

static DWORD WINAPI disarm_worker(void *arg)
{
    ULONG64 t0;
    set_dr0(GetCurrentThread(), (void *)target, 1, FALSE);
    target();                                   /* hit 1 */
    InterlockedExchange(&phase, 1);
    WaitForSingleObject(ev_a, INFINITE);        /* disarmed by main while waiting */
    t0 = thread_cpu_us(GetCurrentThread());
    spin_loop();
    loop_us = thread_cpu_us(GetCurrentThread()) - t0;
    return 0;
}

static DWORD WINAPI rearm_worker(void *arg)
{
    set_dr0(GetCurrentThread(), (void *)target, 1, FALSE);
    target();                                   /* hit 1 */
    set_dr0(GetCurrentThread(), NULL, 0, FALSE);
    target();                                   /* no hit */
    InterlockedExchange(&phase, 1);
    while (phase != 2) sink++;                  /* user code while main re-arms */
    target();                                   /* hit 2 */
    return 0;
}

static int run_and_join(HANDLE t, const char *name, LONG want_hits)
{
    WaitForSingleObject(t, INFINITE);
    return report(hits == want_hits, "%s: %ld hits, %ld calls (expected %ld hits)", name, hits, calls, want_hits);
}

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "self";
    HANDLE t;

    AddVectoredExceptionHandler(1, handler);
    CreateThread(NULL, 0, watchdog, (void *)name, 0, NULL);
    ev_a = CreateEventA(NULL, FALSE, FALSE, NULL);
    ev_b = CreateEventA(NULL, FALSE, FALSE, NULL);

    if (!strcmp(name, "self"))
    {
        if (!set_dr0(GetCurrentThread(), (void *)target, 1, FALSE)) return report(0, "self: SetThreadContext failed");
        target();
        return report(hits == 1, "self: %ld hits (expected 1)", hits);
    }
    if (!strcmp(name, "global-bit"))
    {
        CONTEXT c;
        if (!set_dr0(GetCurrentThread(), (void *)target, 2, FALSE)) return report(0, "global-bit: SetThreadContext failed");
        target();
        memset(&c, 0, sizeof(c));
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        GetThreadContext(GetCurrentThread(), &c);
        /* As Wine's native x86 path: G bits are folded into L bits. */
        return report(hits == 1 && (c.Dr7 & 1), "global-bit: %ld hits with DR7 = G0 only (expected 1), DR7 read back %#lx (L0 expected)",
                      hits, c.Dr7);
    }
    if (!strcmp(name, "remote-blocked"))
    {
        t = CreateThread(NULL, 0, sleeper, NULL, 0, NULL);
        Sleep(100);                             /* the thread is in Sleep now */
        if (!set_dr0(t, (void *)target, 1, TRUE)) return report(0, "remote-blocked: SetThreadContext failed");
        return run_and_join(t, name, 1);
    }
    if (!strcmp(name, "suspended-start"))
    {
        t = CreateThread(NULL, 0, caller, NULL, CREATE_SUSPENDED, NULL);
        if (!set_dr0(t, (void *)target, 1, FALSE)) return report(0, "suspended-start: SetThreadContext failed");
        ResumeThread(t);
        return run_and_join(t, name, 1);
    }
    if (!strcmp(name, "disarm"))
    {
        ULONG64 baseline;
        t = CreateThread(NULL, 0, timed_loop, NULL, 0, NULL);
        WaitForSingleObject(t, INFINITE);
        baseline = loop_us;
        t = CreateThread(NULL, 0, disarm_worker, NULL, 0, NULL);
        while (phase != 1) Sleep(1);
        Sleep(50);                              /* the worker waits on ev_a now */
        if (!set_dr0(t, NULL, 0, TRUE)) return report(0, "disarm: SetThreadContext failed");
        SetEvent(ev_a);
        WaitForSingleObject(t, INFINITE);
        return report(hits == 1 && loop_us <= baseline * 3 / 2 + 2000,
                      "disarm: %ld hits (expected 1), loop %llu us after disarm vs %llu us unarmed (max 1.5x)",
                      hits, loop_us, baseline);
    }
    if (!strcmp(name, "rearm"))
    {
        t = CreateThread(NULL, 0, rearm_worker, NULL, 0, NULL);
        while (phase != 1) Sleep(1);
        if (!set_dr0(t, (void *)target, 1, TRUE)) return report(0, "rearm: SetThreadContext failed");
        InterlockedExchange(&phase, 2);
        return run_and_join(t, name, 2);
    }
    if (!strcmp(name, "resume-at"))
    {
        /* The thread is suspended in user code (a spin loop) and resumed at
         * resume_target, which is armed, with RF clear. */
        CONTEXT c;
        t = CreateThread(NULL, 0, spinner, NULL, 0, NULL);
        while (phase != 1) Sleep(1);
        SuspendThread(t);
        memset(&c, 0, sizeof(c));
        c.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
        if (!GetThreadContext(t, &c)) return report(0, "resume-at: GetThreadContext failed");
        c.Eip = (DWORD)(ULONG_PTR)resume_target;
        c.EFlags &= ~0x10000;
        c.Dr0 = (DWORD)(ULONG_PTR)resume_target;
        c.Dr7 = 1;
        if (!SetThreadContext(t, &c)) return report(0, "resume-at: SetThreadContext failed");
        ResumeThread(t);
        WaitForSingleObject(t, INFINITE);
        return report(hits == 1 && calls == 1, "resume-at: %ld hits, %ld calls (expected 1, 1)", hits, calls);
    }
    if (!strcmp(name, "syscall-load"))
    {
        LONG (WINAPI *yield)(void) = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtYieldExecution");
        int i;
        if (!set_dr0(GetCurrentThread(), (void *)target, 1, FALSE)) return report(0, "syscall-load: SetThreadContext failed");
        for (i = 0; i < 1000; i++)
        {
            yield();
            target();
        }
        return report(hits == 1000, "syscall-load: %ld hits for 1000 calls between syscalls", hits);
    }
    if (!strcmp(name, "pushfd"))
    {
        DWORD flags;
        if (!set_dr0(GetCurrentThread(), (void *)target, 1, FALSE)) return report(0, "pushfd: SetThreadContext failed");
        __asm__ volatile ("pushfl; popl %0" : "=r"(flags));
        target();
        return report(!(flags & 0x100) && hits == 1, "pushfd: EFLAGS %#lx (TF must be clear), %ld hits", flags, hits);
    }
    if (!strcmp(name, "app-tf"))
    {
        /* Armed (on code this case never calls): the emulation sees every
         * instruction. Unarmed, Rosetta does not deliver the application's TF
         * at all, with or without these patches. */
        if (!set_dr0(GetCurrentThread(), (void *)target, 1, FALSE)) return report(0, "app-tf: SetThreadContext failed");
        __asm__ volatile ("pushfl; orl $0x100, (%%esp); popfl; nop; nop" ::: "memory");
        return report(app_steps == 1 && hits == 0, "app-tf: %ld single-steps (expected 1), %ld breakpoint hits", app_steps, hits);
    }
    if (!strcmp(name, "popfd-keeps"))
    {
        if (!set_dr0(GetCurrentThread(), (void *)target, 1, FALSE)) return report(0, "popfd-keeps: SetThreadContext failed");
        __asm__ volatile ("pushfl; andl $~0x100, (%%esp); popfl" ::: "memory");
        target();
        return report(hits == 1 && app_steps == 0, "popfd-keeps: %ld hits after popfd cleared TF (expected 1)", hits);
    }
    return report(0, "unknown case %s", name);
}

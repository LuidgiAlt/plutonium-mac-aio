/* Lazy exec (patch 0004, WINE_ROSETTA_LAZY_EXEC=1) in a process without
 * NX_COMPAT, so Wine turns DEP off as it does for Plutonium's bootstrapper.
 *   exec:     code written to a PAGE_READWRITE page runs (returns 42)
 *   dep-on:   after SetProcessDEPPolicy(PROCESS_DEP_ENABLE), a data write to
 *             that page succeeds (no access violation), and calling it
 *             raises a DEP access violation, as on Windows
 *   readonly: a write to a PAGE_READONLY page raises an access violation
 *             promptly (no endless fault loop)
 *   selfwrite: code on a PAGE_READONLY page writes within 16 bytes of
 *             itself (Rosetta may report that as a read at the instruction
 *             pointer): an access violation, not an endless fault loop
 *   shared-page: one thread keeps calling code on a page while another keeps
 *             writing data on the same page: no access violation (the code
 *             keeps faulting at the same address, and each fault is fixed)
 * A vectored handler counts access violations and steps over the faulting
 * instruction (skip_len bytes); a DEP fault on a call returns -1 to the
 * caller. A watchdog fails the test if a case hangs.
 * Build: i686-w64-mingw32-gcc -O2 -Wl,--disable-nxcompat -o lazy-exec-test.exe lazy-exec-test.c */
#include <windows.h>
#include <stdio.h>
#include "aio_test.h"

static volatile LONG write_faults, exec_faults;
static volatile DWORD skip_len = 3;   /* "mov byte ptr [eax], 1" */
static volatile const char *current_case = "exec";

static LONG CALLBACK handler(EXCEPTION_POINTERS *e)
{
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    CONTEXT *c = e->ContextRecord;

    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    if (r->ExceptionInformation[0] == 8)        /* execute: return -1 to the caller */
    {
        InterlockedIncrement(&exec_faults);
        c->Eip = *(DWORD *)c->Esp;
        c->Esp += 4;
        c->Eax = (DWORD)-1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    /* write, or a write Rosetta reported as a read: skip the instruction */
    if (r->ExceptionInformation[0] == 1 || r->ExceptionInformation[0] == 0)
    {
        InterlockedIncrement(&write_faults);
        c->Eip += skip_len;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI watchdog(void *arg)
{
    Sleep(5000);
    report(0, "%s: no progress after 5 s (endless fault loop?)", current_case);
    ExitProcess(1);
}

static volatile LONG stop_writer;
static DWORD WINAPI writer(void *arg)
{
    volatile BYTE *data = arg;
    while (!stop_writer) data[0]++;
    return 0;
}

static void write_byte(void *p)
{
    __asm__ volatile ("movb $1, (%0)" : : "a"(p) : "memory");  /* C6 00 01 */
}

int main(void)
{
    static const BYTE ret42[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};  /* mov eax, 42; ret */
    BYTE *page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    BYTE *ro = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
    int (*fn)(void) = (int (*)(void))page;
    DWORD start;
    int r;

    AddVectoredExceptionHandler(1, handler);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);

    /* exec */
    memcpy(page, ret42, sizeof(ret42));
    if ((r = fn()) != 42)
        return report(0, "exec: code on a PAGE_READWRITE page returned %d (expected 42; DEP should be off)", r);

    /* selfwrite (before DEP is turned on, while lazy exec is active):
     * call next; next: pop eax; mov byte ptr [eax+8], 1; ret */
    {
        static const BYTE selfwrite[] = {0xe8, 0x00, 0x00, 0x00, 0x00, 0x58, 0xc6, 0x40, 0x08, 0x01, 0xc3};
        BYTE *code = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD old;
        LONG before = write_faults;

        current_case = "selfwrite";
        memcpy(code, selfwrite, sizeof(selfwrite));
        VirtualProtect(code, 4096, PAGE_READONLY, &old);
        skip_len = 4;
        ((void (*)(void))code)();
        skip_len = 3;
        if (write_faults != before + 1)
            return report(0, "selfwrite: %ld access violations (expected 1)", write_faults - before);
        write_faults = before;
    }

    /* shared-page */
    {
        BYTE *shared = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        int (*shared_fn)(void) = (int (*)(void))shared;
        HANDLE t;
        int i, bad = 0;

        current_case = "shared-page";
        memcpy(shared, ret42, sizeof(ret42));
        t = CreateThread(NULL, 0, writer, shared + 2048, 0, NULL);
        for (i = 0; i < 20000; i++) if (shared_fn() != 42) bad++;
        stop_writer = 1;
        WaitForSingleObject(t, INFINITE);
        if (bad || exec_faults || write_faults)
            return report(0, "shared-page: %d wrong results, %ld exec and %ld write access violations (expected none)",
                          bad, exec_faults, write_faults);
    }

    /* dep-on */
    current_case = "dep-on";
    if (!SetProcessDEPPolicy(PROCESS_DEP_ENABLE))
        return skip("SetProcessDEPPolicy(PROCESS_DEP_ENABLE) failed");
    write_byte(page + 100);
    if (write_faults)
        return report(0, "dep-on: a data write to the page faulted %ld times after DEP was turned on", write_faults);
    r = fn();
    if (r != -1 || exec_faults != 1)
        return report(0, "dep-on: calling the page returned %d with %ld DEP faults (expected a DEP fault)", r, exec_faults);

    /* readonly */
    current_case = "readonly";
    start = GetTickCount();
    write_byte(ro);
    if (write_faults != 1 || GetTickCount() - start > 1000)
        return report(0, "readonly: %ld write faults in %lu ms (expected 1, promptly)", write_faults, GetTickCount() - start);

    return report(1, "exec, selfwrite, shared-page, dep-on, readonly");
}

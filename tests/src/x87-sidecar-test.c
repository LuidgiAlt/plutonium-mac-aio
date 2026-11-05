/* x87 maths under the x87sidecar (patch 0023). WINE_X87_SIDECAR names
 * x87-sidecar-test.exe (tests/cases.txt), so the test runs under the sidecar
 * and starts itself as a child the way Plutonium starts the games; the case
 * checks that both processes logged "x87 acceleration on". The child runs
 * fsin through the x87 unit: the sidecar's fsin may differ from Rosetta's in
 * the last bit (0.7 gives ...19b7 against ...19b8), so one unit in the last
 * place is allowed.
 * Build: i686-w64-mingw32-gcc -O2 -o x87-sidecar-test.exe x87-sidecar-test.c */
#include <windows.h>
#include <string.h>
#include "aio_test.h"

static unsigned long long fsin_bits(double x)
{
    double r;
    unsigned long long bits;

    __asm__ volatile ("fldl %1\n\tfsin\n\tfstpl %0" : "=m"(r) : "m"(x));
    memcpy(&bits, &r, sizeof(bits));
    return bits;
}

static int child(void)
{
    const unsigned long long want = 0x3fe49d6e694619b8ULL;  /* fsin(0.7) */
    unsigned long long got = 0;
    int i;

    /* Enough calls for the block to be translated and run hot. */
    for (i = 0; i < 100000; i++) got = fsin_bits(0.7);
    if (got != want) fprintf(stderr, "x87-sidecar-test: fsin(0.7) = %016llx, stock %016llx\n", got, want);
    return got - want + 1 <= 2 ? 0 : 1;
}

int main(int argc, char **argv)
{
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH + 16], exe[MAX_PATH];
    DWORD code = 1;

    if (argc > 1 && !strcmp(argv[1], "child")) return child();

    GetModuleFileNameA(NULL, exe, sizeof(exe));
    snprintf(cmd, sizeof(cmd), "\"%s\" child", exe);
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return report(0, "CreateProcess failed: %lu", GetLastError());
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0)
    {
        TerminateProcess(pi.hProcess, 1);
        return report(0, "the child did not finish in 60 s");
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return report(code == 0, code == 0 ? "fsin(0.7) within 1 ulp in the child" : "the child got a wrong fsin(0.7) or failed (%lu)", code);
}

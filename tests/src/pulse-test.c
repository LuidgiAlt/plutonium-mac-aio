/* PulseEvent right after SetEvent on a manual-reset event (patch 0015 only
 * yields when a pulse woke a thread): four threads wait on the event; the
 * main thread sets it and pulses it at once; the event must be reset
 * afterwards. Windows releases every waiter; Wine's msync can reset the
 * event before woken waiters see it, in stock CrossOver too (all four woke
 * in 78-90 of 200 rounds here), which a yield after the pulse makes rarer.
 * 0015 without that yield: 2-3 of 200. The test needs MIN_FULL rounds in
 * which all four waiters woke within 200 ms.
 * Build: i686-w64-mingw32-gcc -O2 -o pulse-test.exe pulse-test.c */
#include <windows.h>
#include <stdio.h>
#include "aio_test.h"

#define ROUNDS 200
#define MIN_FULL 50

static HANDLE event, ready;
static volatile LONG woken;

static DWORD WINAPI waiter(void *arg)
{
    ReleaseSemaphore(ready, 1, NULL);
    if (WaitForSingleObject(event, 200) == WAIT_OBJECT_0) InterlockedIncrement(&woken);
    return 0;
}

int main(void)
{
    HANDLE threads[4];
    int round, i, full = 0;

    event = CreateEventA(NULL, TRUE, FALSE, NULL);
    ready = CreateSemaphoreA(NULL, 0, 4, NULL);
    for (round = 0; round < ROUNDS; round++)
    {
        woken = 0;
        for (i = 0; i < 4; i++) threads[i] = CreateThread(NULL, 0, waiter, NULL, 0, NULL);
        for (i = 0; i < 4; i++) WaitForSingleObject(ready, INFINITE);
        Sleep(1);
        SetEvent(event);
        PulseEvent(event);
        WaitForMultipleObjects(4, threads, TRUE, INFINITE);
        for (i = 0; i < 4; i++) CloseHandle(threads[i]);
        if (woken == 4) full++;
        if (WaitForSingleObject(event, 0) != WAIT_TIMEOUT)
            return report(0, "round %d: event still signalled after PulseEvent", round);
    }
    return report(full >= MIN_FULL, "all 4 waiters woke in %d of %d rounds (need %d)", full, ROUNDS, MIN_FULL);
}

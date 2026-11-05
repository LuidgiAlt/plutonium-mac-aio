#include <libproc.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

// The sidecar dies while this process keeps translating x87 code: the
// target must carry on with stock Rosetta and correct results.
//
// A worker thread keeps writing fresh x87 blocks into new pages and running
// them, so translation requests are in flight when the main thread SIGKILLs
// the sidecar (found as the x87sidecar process whose arguments name this
// executable). Both ways a request can meet a dead sidecar are reached:
// mid-request (the kernel answers with a send-once notification) and after
// (the send fails). Afterwards a fresh batch of blocks must still translate
// and compute right. Upstream's stub exited the target with status 137.
//
// Each block computes p[2] = (p[0] + p[1]) * p[3] with x87 maths; the inputs
// are small integers and halves, so the result is exact in any precision.

enum { kSlot = 32, kSlots = 4096 };

// fldl (%rdi); faddl 8(%rdi); fmull 24(%rdi); fstpl 16(%rdi); ret
static const unsigned char kBlock[] = {0xdd, 0x07, 0xdc, 0x47, 0x08, 0xdc, 0x4f,
                                       0x18, 0xdd, 0x5f, 0x10, 0xc3};

static atomic_int g_stop;
static atomic_long g_runs, g_bad;

// One round: a fresh page set full of blocks, each run once (= translated once).
static void round_of_blocks(unsigned seed) {
    unsigned char* code = mmap(NULL, (size_t)kSlot * kSlots, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANON, -1, 0);
    if (code == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    for (int i = 0; i < kSlots; i++) {
        unsigned char* s = code + (size_t)i * kSlot;
        memset(s, 0x90, kSlot);  // nops: each block gets its own start
        memcpy(s + (i % 8), kBlock, sizeof(kBlock));
    }
    for (int i = 0; i < kSlots && !atomic_load(&g_stop); i++) {
        double p[4] = {(double)(seed + i), 0.5, 0.0, (double)(1 + i % 5)};
        ((void (*)(double*))(code + (size_t)i * kSlot))(p);
        const double want = ((double)(seed + i) + 0.5) * (double)(1 + i % 5);
        if (p[2] != want) {
            if (atomic_fetch_add(&g_bad, 1) < 3) {
                printf("bad: block %d got %.17g want %.17g\n", i, p[2], want);
            }
        }
        atomic_fetch_add(&g_runs, 1);
    }
    munmap(code, (size_t)kSlot * kSlots);
}

static void* worker(void* arg) {
    (void)arg;
    for (unsigned r = 0; !atomic_load(&g_stop); r++) {
        round_of_blocks(r * 7919u);
    }
    return NULL;
}

// The sidecar serving this process: an x87sidecar whose argv names our path.
static pid_t find_sidecar(const char* self) {
    pid_t pids[4096];
    const int n = proc_listallpids(pids, sizeof(pids));
    for (int i = 0; i < n; i++) {
        char name[64];
        if (pids[i] == getpid() || proc_name(pids[i], name, sizeof(name)) <= 0 ||
            strcmp(name, "x87sidecar") != 0) {
            continue;
        }
        static char args[16384];
        size_t len = sizeof(args);
        int mib[3] = {CTL_KERN, KERN_PROCARGS2, pids[i]};
        if (sysctl(mib, 3, args, &len, NULL, 0) != 0) {
            continue;
        }
        // argc, exec path, then NUL-separated argv: look for our path.
        for (size_t off = sizeof(int); off < len; off += strlen(args + off) + 1) {
            if (strcmp(args + off, self) == 0) {
                return pids[i];
            }
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    (void)argc;
    char self[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(getpid(), self, sizeof(self)) <= 0) {
        strncpy(self, argv[0], sizeof(self) - 1);
    }
    pid_t sidecar = find_sidecar(self);
    if (sidecar == 0) {
        sidecar = find_sidecar(argv[0]);
    }
    if (sidecar == 0) {
        puts("FAIL no sidecar serves this process (run under --cooperative)");
        return 1;
    }

    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
    usleep(300 * 1000);  // translations in flight
    const long before = atomic_load(&g_runs);
    if (kill(sidecar, SIGKILL) != 0) {
        perror("kill");
        return 1;
    }
    usleep(500 * 1000);
    atomic_store(&g_stop, 1);
    pthread_join(t, NULL);
    atomic_store(&g_stop, 0);

    // Fresh blocks, all translated after the sidecar is gone.
    const long mid = atomic_load(&g_runs);
    round_of_blocks(1000003u);
    const long after = atomic_load(&g_runs) - mid;
    const long bad = atomic_load(&g_bad);
    if (kill(sidecar, 0) == 0) {
        printf("FAIL sidecar %d still alive\n", sidecar);
        return 1;
    }
    if (bad != 0 || after != kSlots) {
        printf("FAIL %ld bad results, %ld of %d post-death blocks ran\n", bad, after, kSlots);
        return 1;
    }
    printf("PASS sidecar killed: %ld blocks before, %ld during, %ld after, 0 bad\n", before,
           mid - before, after);
    return 0;
}

#include <mach-o/dyld.h>
#include <mach-o/dyld_images.h>
#include <mach/mach_vm.h>
#include <mach/vm_attributes.h>
#include <servers/bootstrap.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop_proto.h"
#include "dyld_process_info.hpp"
#include "offset_finder.hpp"
#include "rosetta_core/Opcode.h"
#include "rosetta_core/OpcodeCompatibility.h"
#include "rosetta_core/RosettaCore.h"
#include "rosetta_core/TranscendentalHelper.h"
#include "rosetta_core/TranslationResult.h"
#include "sidecar.hpp"
#include "stub_asm.hpp"
#include "transcendental_constants.hpp"
#include "types.h"

const char* logsEnabled = nullptr;

#define VERBOSE_LOG(fmt, ...)           \
    do {                                \
        if (logsEnabled) {              \
            printf(fmt, ##__VA_ARGS__); \
        }                               \
    } while (0)

// NOTE_EXIT watch on the tracee. The sidecar is not its parent, so waitpid is
// out and kqueue's EVFILT_PROC is the way to learn that it exited. It has to be
// armed while the tracee is still held (before the handshake reply): a small target can run to completion during the release itself, and
// registering on a pid that is already gone fails with ESRCH, after which a
// wait on the empty queue never returns.
static int armExitWatch(pid_t pid) {
    int kq = kqueue();
    if (kq == -1) {
        return -1;
    }
    struct kevent ev;
    EV_SET(&ev, pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, nullptr);
    if (kevent(kq, &ev, 1, nullptr, 0, nullptr) == -1) {
        fprintf(stdout, "[rosettax87] kevent(NOTE_EXIT, %d): %s\n", pid, strerror(errno));
        close(kq);
        return -1;
    }
    return kq;
}

// Block until the watched tracee exits. Without a watch, poll for the pid.
static void waitExitWatch(int kq, pid_t pid) {
    if (kq == -1) {
        while (kill(pid, 0) == 0) {
            sleep(1);
        }
        return;
    }
    struct kevent ev;
    kevent(kq, nullptr, 0, &ev, 1, nullptr);
    close(kq);
}

// The stub filter routes an opcode by its host id against two contiguous x87
// ranges (stub_asm.cpp), and the translator reads a few
// more opcodes by id. The ids come from the runtime's own mnemonic table by
// name, so comparing names against ids proves nothing; what can be wrong is
// the table itself and its shape. Check those, so a runtime this build cannot
// route correctly is refused instead of misrouting instructions.
static bool opcodeTableMatches(const OffsetFinder& f) {
    const std::vector<std::string>& names = f.opcodeNames_;
    // The list must be the runtime's opcodes and nothing else. Without the
    // runtime's own count there is no telling where the table ends.
    if (f.opcodeCount_ == 0) {
        fprintf(stdout,
                "[rosettax87] opcode table found, but not the bound in opcode_to_string that "
                "says where it ends\n");
        return false;
    }
    if (names.size() != f.opcodeCount_) {
        fprintf(stdout, "[rosettax87] opcode table read as %zu names, the runtime has %u opcodes\n",
                names.size(), f.opcodeCount_);
        return false;
    }
    bool ok = true;
    // Every opcode the translator inspects needs an id here. A missing one
    // would silently never match (and a short parse shows up this way too).
    if (const uint16_t op = opcode_first_unmapped_required(); op != kOpcodeUnmapped) {
        fprintf(stdout, "[rosettax87] opcode %s: not in this runtime's table\n", kOpcodeNames[op]);
        ok = false;
    }
    // What the stub filter relies on: both x87 ranges are contiguous and in
    // canonical order under the host numbering, so a host-translated range
    // start plus the canonical range size selects exactly the x87 opcodes.
    struct Range {
        uint16_t first, last;
        const char* name;
    };
    static const Range kRanges[] = {
        {kOpcodeName_fcmovb, kOpcodeName_fucomip, "fcmovb..fucomip"},
        {kOpcodeName_f2xm1, kOpcodeName_fyl2xp1, "f2xm1..fyl2xp1"},
    };
    for (const auto& r : kRanges) {
        const uint16_t base = opcode_internal_to_host(r.first);
        if (base == kOpcodeUnmapped) {
            continue;  // reported above
        }
        for (uint16_t op = r.first; op <= r.last; ++op) {
            const uint16_t host = opcode_internal_to_host(op);
            const auto want = static_cast<uint16_t>(base + (op - r.first));
            if (host != want) {
                fprintf(stdout,
                        "[rosettax87] x87 range %s is not contiguous in this runtime: %s expected "
                        "at host id %u, found %s\n",
                        r.name, kOpcodeNames[op], want,
                        want < names.size() ? names[want].c_str() : "nothing");
                ok = false;
                break;
            }
        }
    }
    return ok;
}

// The checks every launch runs against the installed Rosetta once the on-disk
// analysis is done. Prints what failed; the caller decides what that means.
static bool runtimeAssumptionsHold(const OffsetFinder& f) {
    bool ok = true;
    if (f.opcodeNames_.empty()) {
        fprintf(stdout, "[rosettax87] opcode mnemonic table not found; numbering unknown\n");
        ok = false;
    } else if (!opcodeTableMatches(f)) {
        fprintf(stdout, "[rosettax87] cannot route opcodes for this libRosettaRuntime\n");
        ok = false;
    }
    if (!stub_asm::filterOpcodeIdsFit()) {
        ok = false;
    }
    if (f.translationResultSize_ == 0) {
        fprintf(stdout, "[rosettax87] TranslationResult size not found; unchecked\n");
    } else if (f.translationResultSize_ != sidecar::kStockTRSize) {
        fprintf(stdout, "[rosettax87] TranslationResult is 0x%x bytes, this build expects 0x%zx\n",
                f.translationResultSize_, sidecar::kStockTRSize);
        ok = false;
    }
    return ok;
}

// --probe: locate and validate everything in the installed Rosetta, print the
// report, launch nothing. Exit status 0 means this build has everything a
// launch needs from this Rosetta; 1 means something is off (the report says
// what).
static int probeRuntime() {
    OffsetFinder f;
    const bool runtimeOk = f.determineOffsets();
    const bool libOk = runtimeOk && f.determineRuntimeOffsets();
    printf("/usr/libexec/rosetta/runtime: %s\n", runtimeOk ? "ok" : "NOT SUPPORTED");
    if (runtimeOk) {
        printf(
            "  exports_fetch      0x%llx\n  mmap wrapper       0x%llx\n  g_disable_aot      "
            "0x%llx\n",
            f.offsetExportsFetch_, f.offsetSvcCallEntry_, f.offsetDisableAot_);
    }
    printf("libRosettaRuntime: %s\n", libOk ? "ok" : (runtimeOk ? "NOT SUPPORTED" : "not checked"));
    if (!libOk) {
        return 1;
    }
    rosetta_core_set_runtime_version(f.runtimeVersion_);
    // Only a table cut at the runtime's own bound; without it the check below
    // refuses the runtime, and the report should not claim a name mapping.
    if (!f.opcodeNames_.empty() && f.opcodeCount_ != 0) {
        opcode_set_host_table(f.opcodeNames_);
    }
    printf("  version            0x%llx (%s)\n", f.runtimeVersion_,
           opcode_host_table_active() ? "opcodes mapped by name from the runtime table"
                                      : "opcode table not read");
    printf("  translator_translate %s0x%llx\n",
           f.offsetTranslatorTranslate_ ? "" : "not exported, ", f.offsetTranslatorTranslate_);
    printf("  translate_insn     0x%llx (x87 hook)\n", f.offsetTranslateInsn_);
    printf("  TranslationResult  %s0x%x\n",
           f.translationResultSize_ ? "" : "size not found; expected ",
           f.translationResultSize_ ? f.translationResultSize_
                                    : static_cast<uint32_t>(sidecar::kStockTRSize));
    if (f.opcodeCount_ != 0) {
        printf("  opcode table       %zu names, runtime bound %u\n", f.opcodeNames_.size(),
               f.opcodeCount_);
    } else {
        printf("  opcode table       %zu names, runtime bound not found\n", f.opcodeNames_.size());
    }
    const bool ok = runtimeAssumptionsHold(f);
    if (!ok) {
        printf("NOT fully supported\n");
    } else {
        printf("supported\n");
    }
    return ok ? 0 : 1;
}

// bootstrap_register2 is a private libSystem symbol (the non-deprecated way to
// publish a dynamic service name). It isn't in <servers/bootstrap.h>, so declare
// it like wine's server/mach.c does.
extern "C" kern_return_t bootstrap_register2(mach_port_t bp, name_t service_name, mach_port_t sp,
                                             uint64_t flags);

class MuhDebugger {
private:
    pid_t childPid_ = -1;
    task_t taskPort_ = MACH_PORT_NULL;
    thread_t handshakeThread_ = MACH_PORT_NULL;

public:
    ~MuhDebugger() {
        if (taskPort_ != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), taskPort_);
        }
        if (handshakeThread_ != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), handshakeThread_);
        }
    }

    [[nodiscard]] task_t taskPort() const { return taskPort_; }

    // The control port of the tracee thread that performed the handshake
    // (wine's unix-loader startup thread). Held for the sidecar's lifetime
    // alongside taskPort_.
    [[nodiscard]] thread_t handshakeThread() const { return handshakeThread_; }

    // ── Cooperative attach (no task_for_pid / no ptrace) ────────────────────
    // Adopt the task and thread ports the tracee voluntarily handed over via
    // the bootstrap handshake. Both arrive as COPY_SEND, so we own both send
    // rights; ~MuhDebugger releases them. No ptrace attach, no exec-stop: the
    // tracee is already running and is held quiescent by blocking in the
    // handshake until we reply.
    bool adopt(pid_t pid, task_t task, thread_t handshakeThread) {
        childPid_ = pid;
        taskPort_ = task;
        handshakeThread_ = handshakeThread;
        return true;
    }

    // Like findRuntime(), but disambiguates by content: return the first
    // executable MH_MAGIC_64 region whose bytes at [matchOff, matchOff+matchLen)
    // equal `match`. In a fully-initialized process (cooperative mode) there are
    // several MH_MAGIC regions besides libRosettaRuntime, so the plain
    // first-match heuristic in findRuntime() picks the wrong one; the
    // exports_fetch instruction signature uniquely identifies the runtime.
    [[nodiscard]] auto findRuntimeMatching(uint64_t matchOff, const uint8_t* match,
                                           size_t matchLen) const -> uintptr_t {
        if (matchLen == 0 || matchLen > 64) {
            return 0;
        }
        mach_vm_address_t address = 0;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_port_t objectName = MACH_PORT_NULL;
        while (true) {
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            if (mach_vm_region(taskPort_, &address, &size, VM_REGION_BASIC_INFO_64,
                               reinterpret_cast<vm_region_info_t>(&info), &count,
                               &objectName) != KERN_SUCCESS) {
                break;
            }
            if (info.protection & (VM_PROT_EXECUTE | VM_PROT_READ)) {
                uint32_t magic = 0;
                if (readMemory(address, &magic, sizeof(magic)) && magic == MH_MAGIC_64) {
                    uint8_t buf[64];
                    if (readMemory(address + matchOff, buf, matchLen) &&
                        memcmp(buf, match, matchLen) == 0) {
                        return address;
                    }
                }
            }
            address += size;
        }
        return 0;
    }

    // Scan the tracee's executable memory for a byte pattern; return the live
    // address of the first match, or 0. Locates translate_insn by its prologue
    // signature without the Exports struct (X19), while the tracee is blocked
    // in the handshake.
    [[nodiscard]] uint64_t scanForPattern(const uint8_t* pat, size_t patLen) const {
        if (patLen == 0) {
            return 0;
        }
        mach_vm_address_t address = 0;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_port_t objectName = MACH_PORT_NULL;
        std::vector<uint8_t> buf;
        while (true) {
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            if (mach_vm_region(taskPort_, &address, &size, VM_REGION_BASIC_INFO_64,
                               reinterpret_cast<vm_region_info_t>(&info), &count,
                               &objectName) != KERN_SUCCESS) {
                break;
            }
            const uint64_t regionStart = address;
            const uint64_t regionSize = size;
            address += size;  // advance before any continue
            if ((info.protection & VM_PROT_EXECUTE) == 0) {
                continue;
            }
            if (regionSize == 0 || regionSize > (64ULL << 20)) {
                continue;  // skip absent/implausibly-large executable regions
            }
            buf.resize(regionSize);
            if (!readMemory(regionStart, buf.data(), regionSize)) {
                continue;
            }
            const auto it = std::search(buf.begin(), buf.end(), pat, pat + patLen);
            if (it != buf.end()) {
                return regionStart + static_cast<uint64_t>(std::distance(buf.begin(), it));
            }
        }
        return 0;
    }

    enum Register : std::uint8_t {
        X0,
        X1,
        X2,
        X3,
        X4,
        X5,
        X6,
        X7,
        X8,
        X9,
        X10,
        X11,
        X12,
        X13,
        X14,
        X15,
        X16,
        X17,
        X18,
        X19,
        X20,
        X21,
        X22,
        X23,
        X24,
        X25,
        X26,
        X27,
        X28,
        FP,
        LR,
        SP,
        PC,
        CPSR
    };

    // Read a register from a specific thread port. Used to read X19 from the
    // exact thread reported by the breakpoint exception message — correct even
    // if libRosettaRuntime has spawned other threads by then (task_threads[0]
    // is not guaranteed to be the one that hit the BRK).
    [[nodiscard]] uint64_t readRegister(mach_port_t thread, Register reg) const {
        arm_thread_state64_t state;
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        kern_return_t kr = thread_get_state(thread, ARM_THREAD_STATE64,
                                            reinterpret_cast<thread_state_t>(&state), &count);
        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "Failed to get thread state (error 0x%x: %s)\n", kr,
                    mach_error_string(kr));
            return 0;
        }

        if (reg >= X0 && reg <= X28) {
            return state.__x[reg];
        }
        switch (reg) {
            case FP:
                return state.__fp;
            case LR:
                return state.__lr;
            case SP:
                return state.__sp;
            case PC:
                return state.__pc;
            case CPSR:
                return state.__cpsr;
            default:
                fprintf(stdout, "Invalid register\n");
                return 0;
        }
    }

    [[nodiscard]] bool setRegister(Register reg, uint64_t value) const {
        thread_act_port_array_t threadList;
        mach_msg_type_number_t threadCount;

        kern_return_t kr = task_threads(taskPort_, &threadList, &threadCount);
        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "Failed to get threads (error 0x%x: %s)\n", kr, mach_error_string(kr));
            return false;
        }

        arm_thread_state64_t state;
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        kr = thread_get_state(threadList[0], ARM_THREAD_STATE64,
                              reinterpret_cast<thread_state_t>(&state), &count);

        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "Failed to get thread state (error 0x%x: %s)\n", kr,
                    mach_error_string(kr));
            return false;
        }

        if (reg >= X0 && reg <= X28) {
            state.__x[reg] = value;
        } else {
            switch (reg) {
                case FP:
                    state.__fp = value;
                    break;
                case LR:
                    state.__lr = value;
                    break;
                case SP:
                    state.__sp = value;
                    break;
                case PC:
                    state.__pc = value;
                    break;
                case CPSR:
                    state.__cpsr = value;
                    break;
                default: {
                    fprintf(stdout, "Invalid register\n");
                    return false;
                }
            }
        }

        kr = thread_set_state(threadList[0], ARM_THREAD_STATE64,
                              reinterpret_cast<thread_state_t>(&state), ARM_THREAD_STATE64_COUNT);
        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "Failed to set thread state (error 0x%x: %s)\n", kr,
                    mach_error_string(kr));
            return false;
        }

        // Cleanup
        for (uint i = 0; i < threadCount; i++) {
            mach_port_deallocate(mach_task_self(), threadList[i]);
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threadList),
                      sizeof(thread_t) * threadCount);

        return true;
    }

    [[nodiscard]] bool adjustMemoryProtection(uint64_t address, vm_prot_t protection,
                                              mach_vm_size_t size) const {
        // 4KB page size in rosetta process
        vm_size_t pageSize = 0x1000;
        // align to page boundary
        mach_vm_address_t region = address & ~(pageSize - 1);
        size = ((address + size + pageSize - 1) & ~(pageSize - 1)) - region;

        VERBOSE_LOG("Adjusting memory protection at 0x%llx - 0x%llx\n", (uint64_t)region,
                    (uint64_t)(region + size));

        kern_return_t kr = mach_vm_protect(taskPort_, region, size, false, protection);
        if (kr != KERN_SUCCESS) {
            fprintf(stdout,
                    "Failed to adjust memory protection at 0x%llx - 0x%llx (error 0x%x: %s)\n",
                    static_cast<uint64_t>(region), (region + size), kr, mach_error_string(kr));
            return false;
        }
        return true;
    }

    bool readMemory(uint64_t address, void* buffer, size_t size) const {
        mach_vm_size_t readSize;

        kern_return_t kr = mach_vm_read_overwrite(
            taskPort_, address, size, reinterpret_cast<mach_vm_address_t>(buffer), &readSize);

        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "Failed to read memory at 0x%llx (error 0x%x: %s)\n", address, kr,
                    mach_error_string(kr));
            return false;
        }

        return readSize == size;
    }

    bool writeMemory(uint64_t address, const void* buffer, size_t size) const {
        kern_return_t kr =
            mach_vm_write(taskPort_, address, reinterpret_cast<vm_offset_t>(buffer), size);

        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "Failed to write memory at 0x%llx (error 0x%x: %s)\n", address, kr,
                    mach_error_string(kr));
            return false;
        }

        // Flush the target's instruction cache for the written range. ARM64 I/D
        // caches are not coherent: mach_vm_write lands in the D-cache path only,
        // so a core that has already executed (and cached) this line keeps
        // running the stale bytes. That is fatal for the one breakpoint we plant
        // at exports_fetch: we plant a BRK, catch it, restore the original
        // instruction, then RESUME the held thread — which re-fetches the same
        // PC (ARM64 BRK does not advance PC). If the stale BRK is still in the
        // I-cache at that re-fetch, it re-traps after we have detached and torn
        // down our exception handler → an unhandled SIGTRAP kills the tracee
        // (the flaky post-detach exit=133). Flushing here makes the restore (and
        // every M2 __TEXT patch) coherent, exactly as lldb's debugserver does
        // after writing to a tracee. Best-effort: a failure only regresses to
        // the old behaviour, so log and continue.
        vm_machine_attribute_val_t flush = MATTR_VAL_ICACHE_FLUSH;
        kern_return_t fkr =
            mach_vm_machine_attribute(taskPort_, address, size, MATTR_CACHE, &flush);
        if (fkr != KERN_SUCCESS) {
            fprintf(stdout, "Warning: i-cache flush at 0x%llx failed (error 0x%x: %s)\n", address,
                    fkr, mach_error_string(fkr));
        }

        return true;
    }

    bool copyThreadState(arm_thread_state64_t& state) const {
        thread_act_port_array_t threadList;
        mach_msg_type_number_t threadCount;

        kern_return_t kr = task_threads(taskPort_, &threadList, &threadCount);
        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "Failed to get threads (error 0x%x: %s)\n", kr, mach_error_string(kr));
            return false;
        }

        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        kr = thread_get_state(threadList[0], ARM_THREAD_STATE64,
                              reinterpret_cast<thread_state_t>(&state), &count);

        // Cleanup
        for (uint i = 0; i < threadCount; i++) {
            mach_port_deallocate(mach_task_self(), threadList[i]);
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threadList),
                      sizeof(thread_t) * threadCount);

        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "Failed to get thread state (error 0x%x: %s)\n", kr,
                    mach_error_string(kr));
            return false;
        }

        return true;
    }

    [[nodiscard]] bool restoreThreadState(const arm_thread_state64_t& state) const {
        thread_act_port_array_t threadList;
        mach_msg_type_number_t threadCount;

        kern_return_t kr = task_threads(taskPort_, &threadList, &threadCount);
        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "Failed to get threads (error 0x%x: %s)\n", kr, mach_error_string(kr));
            return false;
        }

        kr = thread_set_state(
            threadList[0], ARM_THREAD_STATE64,
            reinterpret_cast<thread_state_t>(const_cast<arm_thread_state64_t*>(&state)),
            ARM_THREAD_STATE64_COUNT);

        // Cleanup
        for (uint i = 0; i < threadCount; i++) {
            mach_port_deallocate(mach_task_self(), threadList[i]);
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threadList),
                      sizeof(thread_t) * threadCount);

        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "Failed to set thread state (error 0x%x: %s)\n", kr,
                    mach_error_string(kr));
            return false;
        }

        return true;
    }

    [[nodiscard]] auto findRuntime() const -> uintptr_t {
        mach_vm_address_t address = 0;
        mach_vm_size_t size;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t objectName;
        kern_return_t kr;
        __block std::vector<uintptr_t> moduleList;

        auto* processInfo = _dyld_process_info_create(taskPort_, 0, &kr);
        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "Failed to get dyld process info (error 0x%x: %s)\n", kr,
                    mach_error_string(kr));
            return 0;
        }
        _dyld_process_info_for_each_image(processInfo,
                                          ^(uint64_t address, const uuid_t uuid, const char* path) {
                                            VERBOSE_LOG("Module: 0x%llx - %s\n", address, path);
                                            moduleList.push_back(address);
                                          });
        _dyld_process_info_release(processInfo);

        while (true) {
            if (mach_vm_region(taskPort_, &address, &size, VM_REGION_BASIC_INFO_64,
                               reinterpret_cast<vm_region_info_t>(&info), &count,
                               &objectName) != KERN_SUCCESS) {
                break;
            }

            if (info.protection & (VM_PROT_EXECUTE | VM_PROT_READ)) {
                if (std::ranges::find_if(moduleList, [address](const uintptr_t& moduleAddress) {
                        return address == moduleAddress;
                    }) == moduleList.end()) {
                    uint32_t magicBytes;
                    if (readMemory(address, &magicBytes, sizeof(magicBytes)) &&
                        magicBytes == MH_MAGIC_64) {
                        return address;
                    }
                }
            }

            address += size;
        }

        return 0;
    }
};

static void print_loader_usage(const char* prog) {
    std::printf(
        "usage: %s --cooperative <program> [program-args...]\n"
        "       %s --probe\n"
        "\n"
        "Flags:\n"
        "  --cooperative  attach via a voluntary task-port handshake. The target\n"
        "                 must hand over its task port over the bootstrap service\n"
        "                 named in the " X87_COOP_ENV
        " env var (set automatically).\n"
        "                 Required: there is no other attach mode.\n"
        "  --probe        locate and validate everything this build hooks in the\n"
        "                 installed Rosetta, print the report and exit; launches\n"
        "                 nothing. Exit status 0 when a launch has all it needs.\n"
        "  --help         print this message and exit\n"
        "\n"
        "Environment:\n"
        "  X87_LOGS=1     verbose sidecar logging\n",
        prog, prog);
}

// Cooperative-mode bootstrap service name: "x87sidecar.<tracee-pid>".
static std::string coop_service_name(pid_t pid) {
    return std::string("x87sidecar.") + std::to_string(pid);
}

// Wine runs every i386 process through `x87sidecar --cooperative`, so a
// failure to set up the hook must not keep the program from starting. When it
// happens before the target is launched, cooperative mode execs the target in
// place without the hook. The pid Wine's parent tracks stays the same and the
// target gets the argv and environment it would have had if Wine had exec'd it
// directly: the variables the loader adds for it are put back the way they
// were inherited. g_unhookedArgv is set in the original process only.
static char** g_unhookedArgv = nullptr;
static std::optional<std::string> g_inheritedDisableAot;

// The warning shown whenever a target runs without the hook. It has to be
// impossible to miss in a wine log, since the only other symptom is a slow
// game. "RUNNING WITHOUT X87 ACCELERATION" is the stable substring tests look
// for. One write(2), so concurrent processes do not interleave its lines.
static void printUnhookedBanner(const std::string& reason, pid_t pid, const char* prog) {
    std::string upper = reason;
    for (char& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    const std::string rule(64, '#');
    const std::string text =
        rule + "\nWARNING: X87SIDECAR COULD NOT HOOK ROSETTA: " + upper + ".\nPROCESS " +
        std::to_string(pid) + " IS RUNNING WITHOUT X87 ACCELERATION: " + prog +
        "\n32-BIT GAMES THAT USE X87 MATH WILL BE MUCH SLOWER.\n" + rule + "\n";
    fflush(stdout);
    fflush(stderr);
    (void)write(STDERR_FILENO, text.data(), text.size());
}

static int runUnhooked(const std::string& reason) {
    printUnhookedBanner(reason, getpid(), g_unhookedArgv[0]);
    unsetenv(X87_COOP_ENV);
    if (g_inheritedDisableAot) {
        setenv("ROSETTA_DISABLE_AOT", g_inheritedDisableAot->c_str(), 1);
    } else {
        unsetenv("ROSETTA_DISABLE_AOT");
    }
    execv(g_unhookedArgv[0], g_unhookedArgv);
    fprintf(stderr, "[rosettax87] execv %s: %s\n", g_unhookedArgv[0], strerror(errno));
    return 1;
}

// Once the cooperative target has been launched, a sidecar that gives up
// before releasing it leaves it running without the hook: a tracee blocked in
// the handshake resumes when this process exits and the reply right dies with
// it, or earlier when the rollback replies. Say so on the way out, whichever
// return path is taken. After the handshake request arrived, the target is
// known to be running, and gets the warning banner. Before it, the wording
// does not claim that: the target's exec may have failed, or it may never
// perform the handshake.
struct UnhookedNotice {
    const char* prog = nullptr;
    pid_t pid = 0;
    bool handshook = false;
    UnhookedNotice() = default;
    UnhookedNotice(const UnhookedNotice&) = delete;
    UnhookedNotice& operator=(const UnhookedNotice&) = delete;
    ~UnhookedNotice() {
        if (prog == nullptr) {
            return;
        }
        if (handshook) {
            printUnhookedBanner("hook setup failed after the handshake", pid, prog);
        } else {
            fflush(stdout);
            fprintf(stderr, "[rosettax87] x87 hook not installed for %s\n", prog);
        }
    }
};

// Runs a callable when the scope is left by any return or exception, unless
// dismissed first.
template <class F>
struct ScopeExit {
    F fn;
    bool armed = true;
    explicit ScopeExit(F f) : fn(std::move(f)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() {
        if (armed) {
            fn();
        }
    }
};

int main(int argc, char* argv[]) try {
    int argi = 1;
    if (argi < argc && std::string_view(argv[argi]) == "--help") {
        print_loader_usage(argv[0]);
        return 0;
    }
    if (argi < argc && std::string_view(argv[argi]) == "--probe") {
        return probeRuntime();
    }
    if (argi >= argc || std::string_view(argv[argi]) != "--cooperative") {
        std::fprintf(stderr, "%s: --cooperative is required (try --help)\n", argv[0]);
        return 2;
    }
    ++argi;

    if (argi >= argc) {
        std::fprintf(stderr, "%s: missing <program> argument (try --help)\n", argv[0]);
        return 2;
    }

    // X87_LOGS: any value but "" or "0" turns verbose logging on.
    if (const char* v = getenv("X87_LOGS"); v != nullptr && v[0] != '\0' && strcmp(v, "0") != 0) {
        logsEnabled = "1";
    }

    VERBOSE_LOG("Launching debugger.\n");

    // Reverse fork: the ORIGINAL pid execs into the target (keeps the pid for
    // macOS dock/activation) and a double-forked grandchild becomes the
    // sidecar, which obtains the tracee's task port through the handshake.
    char** progArgv = &argv[argi];
    pid_t parentPid = getpid();
    MuhDebugger dbg;
    mach_port_t coopReplyPort = MACH_PORT_NULL;

    // A failure before the target is launched runs the target without the
    // hook (see runUnhooked).
    g_unhookedArgv = progArgv;
    if (const char* v = getenv("ROSETTA_DISABLE_AOT")) {
        g_inheritedDisableAot = v;
    }
    auto notLaunching = [&](const std::string& reason) -> int { return runUnhooked(reason); };

    // Locate what we patch by scanning the installed Rosetta binaries on disk.
    // This runs before anything is launched: a runtime whose code does not
    // match is unsupported, and the right answer is to say so rather than
    // patch guessed addresses.
    OffsetFinder offsetFinder;
    if (!offsetFinder.determineOffsets()) {
        return notLaunching(
            "unsupported Rosetta: /usr/libexec/rosetta/runtime does not match the expected code "
            "patterns");
    }
    VERBOSE_LOG("offset_exports_fetch=%llx offset_svc_call_entry=%llx offset_svc_call_ret=%llx\n",
                offsetFinder.offsetExportsFetch_, offsetFinder.offsetSvcCallEntry_,
                offsetFinder.offsetSvcCallRet_);
    if (!offsetFinder.determineRuntimeOffsets()) {
        return notLaunching(
            "unsupported Rosetta: libRosettaRuntime does not match the expected code patterns");
    }
    VERBOSE_LOG("offset_translate_insn=%llx offset_transaction_result_size=%llx\n",
                offsetFinder.offsetTranslateInsn_, offsetFinder.offsetTransactionResultSize_);
    VERBOSE_LOG("translator_translate=%llx tr_size=0x%x\n", offsetFinder.offsetTranslatorTranslate_,
                offsetFinder.translationResultSize_);

    // Seed the runtime version (OpcodeCompatibility gates 26.4<->26.5 on it)
    // from the on-disk Exports.version. Set here, before the fork, so the sidecar inherits it and the checks below can use it.
    VERBOSE_LOG("Rosetta version: %llx\n", offsetFinder.runtimeVersion_);
    rosetta_core_set_runtime_version(offsetFinder.runtimeVersion_);
    // Map opcode ids by name from the runtime's own mnemonic table when it was
    // found with its bound (a runtime without it is refused below). Also
    // before the fork, for the same reason.
    if (!offsetFinder.opcodeNames_.empty() && offsetFinder.opcodeCount_ != 0) {
        opcode_set_host_table(offsetFinder.opcodeNames_);
    }

    // What the emitted code and the stub filter assume about this runtime,
    // checked against the runtime itself rather than trusted.
    if (!runtimeAssumptionsHold(offsetFinder)) {
        return notLaunching("unsupported Rosetta");
    }

    // Force Rosetta to JIT (call translate_insn) instead of using its AOT /
    // interpreter path: libRosettaRuntime reads ROSETTA_DISABLE_AOT at init, so
    // it MUST be in the environment before the target execs. This is what makes
    // the JIT hook reachable in cooperative mode (which attaches post-init and
    // so can't win the race to write the g_disable_aot global in time). Set
    // only once the runtime is known to be supported, so a target run without
    // the hook keeps its AOT path.
    setenv("ROSETTA_DISABLE_AOT", "1", 1);

    // Cooperative mode publishes its bootstrap service name (derived from the
    // pid the parent keeps across execv) BEFORE forking, so the target inherits
    // it via the environment.
    std::string coopName = coop_service_name(parentPid);
    setenv(X87_COOP_ENV, coopName.c_str(), 1);

    // Nothing buffered may cross the forks below, or it is written twice.
    fflush(stdout);
    fflush(stderr);

    int syncPipe[2];
    if (pipe(syncPipe) == -1) {
        return notLaunching(std::string("pipe: ") + strerror(errno));
    }

    pid_t child = fork();

    if (child == -1) {
        const int err = errno;
        close(syncPipe[0]);
        close(syncPipe[1]);
        return notLaunching(std::string("fork: ") + strerror(err));
    }

    if (child != 0) {
        // PARENT → tracee: wait until the sidecar has attached/registered, then
        // exec the target (keeps the original pid).
        close(syncPipe[1]);
        char buf = 0;
        ssize_t got;
        while ((got = read(syncPipe[0], &buf, 1)) == -1 && errno == EINTR) {
        }
        close(syncPipe[0]);
        // Reap the intermediate double-fork child, which exits right after
        // forking the sidecar, so the target does not inherit a zombie.
        while (waitpid(child, nullptr, 0) == -1 && errno == EINTR) {
        }
        if (got != 1 || buf != 'x') {
            // The sidecar gave up before attaching (its service could not be
            // registered, or it died).
            return notLaunching("sidecar did not attach");
        }
        VERBOSE_LOG("parent: launching into program: %s\n", progArgv[0]);
        execv(progArgv[0], progArgv);
        fprintf(stdout, "parent: execv: %s\n", strerror(errno));
        return 1;
    }

    // CHILD: double-fork so the sidecar reparents to launchd.
    close(syncPipe[0]);
    // Only the original process may exec the target.
    g_unhookedArgv = nullptr;
    pid_t grandchild = fork();
    if (grandchild == -1) {
        fprintf(stdout, "fork (double-fork): %s\n", strerror(errno));
        _exit(1);
    }
    if (grandchild != 0) {
        _exit(0);  // intermediate child exits; grandchild reparented to launchd
    }

    // ── GRANDCHILD == the sidecar ───────────────────────────────────────────
    UnhookedNotice unhookedNotice;
    {
        // Publish a receive port under the pid-based name; the target looks it
        // up, hands over its task+thread control ports, and blocks for a reply.
        mach_port_t bootstrapPort = MACH_PORT_NULL;
        task_get_bootstrap_port(mach_task_self(), &bootstrapPort);
        mach_port_t servicePort = MACH_PORT_NULL;
        mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &servicePort);
        mach_port_insert_right(mach_task_self(), servicePort, servicePort, MACH_MSG_TYPE_MAKE_SEND);
        kern_return_t kr =
            bootstrap_register2(bootstrapPort, const_cast<char*>(coopName.c_str()), servicePort, 0);
        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "[rosettax87] bootstrap_register2(%s) failed: 0x%x\n", coopName.c_str(),
                    kr);
            return 1;
        }
        VERBOSE_LOG("[rosettax87] cooperative service registered: %s\n", coopName.c_str());
        write(syncPipe[1], "x", 1);
        close(syncPipe[1]);
        unhookedNotice.prog = progArgv[0];  // the target is launched from here on
        unhookedNotice.pid = parentPid;

        x87_coop_request_rcv_t rcv{};
        kr = mach_msg(&rcv.req.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(rcv), servicePort,
                      30000, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "[rosettax87] cooperative handshake receive failed: 0x%x (%s)\n", kr,
                    mach_error_string(kr));
            return 1;
        }
        unhookedNotice.handshook = true;
        task_t traceeTask = rcv.req.task_port.name;
        thread_t traceeThread = rcv.req.thread_port.name;
        coopReplyPort = rcv.req.header.msgh_remote_port;
        VERBOSE_LOG("[rosettax87] cooperative attach: task=0x%x thread=0x%x reply=0x%x\n",
                    traceeTask, traceeThread, coopReplyPort);
        if (!dbg.adopt(parentPid, traceeTask, traceeThread)) {
            return 1;
        }
    }

    // Code ranges the M2 install patched, for the tracee to i-cache-invalidate
    // (see x87_coop_reply_t). Populated during M2.
    uint64_t coopIcacheAddr[2] = {0, 0};
    uint64_t coopIcacheLen[2] = {0, 0};

    // Release the tracee once the hook is installed: reply to the handshake,
    // carrying the patched code ranges so the tracee flushes its own i-cache,
    // then unblock it.
    auto releaseTracee = [&]() {
        x87_coop_reply_t reply{};
        reply.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
        reply.header.msgh_size = sizeof(reply);
        reply.header.msgh_remote_port = coopReplyPort;
        reply.header.msgh_local_port = MACH_PORT_NULL;
        reply.header.msgh_id = X87_COOP_MSGH_ID + 1;
        reply.icache_addr[0] = coopIcacheAddr[0];
        reply.icache_addr[1] = coopIcacheAddr[1];
        reply.icache_len[0] = coopIcacheLen[0];
        reply.icache_len[1] = coopIcacheLen[1];
        kern_return_t kr = mach_msg(&reply.header, MACH_SEND_MSG, sizeof(reply), 0,
                                    MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "[rosettax87] cooperative reply failed: 0x%x (%s)\n", kr,
                    mach_error_string(kr));
        }
    };

    static const uint8_t kExportsFetchPat[8] = {0x62, 0x06, 0x40, 0xF9, 0x63, 0x12, 0x40, 0xB9};
    const auto runtimeBase = dbg.findRuntimeMatching(offsetFinder.offsetExportsFetch_,
                                                     kExportsFetchPat, sizeof(kExportsFetchPat));

    VERBOSE_LOG("Rosetta runtime base: 0x%lx\n", runtimeBase);

    if (runtimeBase == 0) {
        fprintf(stdout, "Failed to find Rosetta runtime by signature\n");
        return 1;
    }
    // NOTE: Apple's AOT/interpreter path is already disabled — libRosettaRuntime
    // read ROSETTA_DISABLE_AOT=1 (set unconditionally before fork) at init and
    // set the g_disable_aot global itself, before deciding AOT-vs-JIT. That is
    // the single mechanism: it works pre-init regardless of when we attach, so
    // we do not poke the global from here.

    // Locate libRosettaRuntime in the tracee by content: the image whose
    // bytes at translate_insn's on-disk offset are translate_insn's prologue.
    // Valid because the post-init handshake guarantees oah is mapped. Every hook address is then
    // image base + on-disk offset; a live prologue scan remains the fallback.
    const uint64_t libBase = dbg.findRuntimeMatching(offsetFinder.offsetTranslateInsn_,
                                                     offsetFinder.translateInsnPrologue_.data(),
                                                     offsetFinder.translateInsnPrologue_.size());
    uint64_t translateInsnAddr = 0;
    if (libBase != 0) {
        VERBOSE_LOG("libRosettaRuntime base: 0x%llx\n", libBase);
        translateInsnAddr = libBase + offsetFinder.offsetTranslateInsn_;
    } else {
        fprintf(stdout, "libRosettaRuntime not found by content; scanning for translate_insn\n");
        translateInsnAddr = dbg.scanForPattern(OffsetFinder::kTranslateInsnPattern.data(),
                                               OffsetFinder::kTranslateInsnPattern.size());
    }
    if (translateInsnAddr == 0) {
        fprintf(stdout, "Failed to locate translate_insn\n");
        return 1;
    }
    VERBOSE_LOG("translate_insn (scanned) = 0x%llx\n", translateInsnAddr);

    // ── M2: Inline IPC stub install ─────────────────────────────────────────
    //
    // 1. Find libRosettaRuntime's __TEXT trailing alignment padding (zero
    //    bytes between last function and segment-end page boundary). That's
    //    where OUR_HANDLER + STASH + STASH_JUMP will live.
    // 2. Allocate a Mach receive port in this process and plant a SEND
    //    right under a fresh name in the parent's port namespace.
    // 3. Assemble stub bytes (entry @ translate_insn[0..16] + handler blob
    //    @ trailing padding) referencing that port name.
    // 4. COW + mach_vm_write the bytes; restore RX.
    // 5. After detach, run the receive loop alongside kqueue parent-exit.
    // NOTE_EXIT watch on the tracee, armed inside the install scope before the
    // release and waited on after it.
    int exitWatch = -1;
    {
        // translate_insn's live address came from the signature scan above.
        // No live Mach-O header walk, no Exports/init_library math, and no
        // TR-size patch: x87_cache no longer lives in the tracee's TR (the
        // sidecar keeps it in its own per-thread map), so stock's TR allocation
        // size is left untouched — which is what lets this install work
        // post-init in cooperative mode.

        // Snapshot the original prologue.
        uint8_t origPrologue[16];
        if (!dbg.readMemory(translateInsnAddr, origPrologue, sizeof(origPrologue))) {
            fprintf(stdout, "M2: failed to read translate_insn prologue at 0x%llx\n",
                    translateInsnAddr);
            return 1;
        }
        if (memcmp(origPrologue, offsetFinder.translateInsnPrologue_.data(),
                   sizeof(origPrologue)) != 0) {
            fprintf(stdout,
                    "translate_insn bytes at 0x%llx differ from the on-disk image (already "
                    "patched?); refusing to hook\n",
                    translateInsnAddr);
            return 1;
        }
        VERBOSE_LOG(
            "M2: translate_insn prologue: %02x%02x%02x%02x %02x%02x%02x%02x "
            "%02x%02x%02x%02x %02x%02x%02x%02x\n",
            origPrologue[0], origPrologue[1], origPrologue[2], origPrologue[3], origPrologue[4],
            origPrologue[5], origPrologue[6], origPrologue[7], origPrologue[8], origPrologue[9],
            origPrologue[10], origPrologue[11], origPrologue[12], origPrologue[13],
            origPrologue[14], origPrologue[15]);

        // ── Find the live executable region containing translate_insn ───────
        // dyld_shared_cache may split libRosettaRuntime segments. The
        // mach_header at runtimeBase doesn't necessarily live in the same
        // region as translate_insn's code. Use mach_vm_region to find the
        // bounds of the contiguous executable mapping that holds it.
        mach_vm_address_t regAddr = translateInsnAddr;
        mach_vm_size_t regSize = 0;
        vm_region_basic_info_data_64_t regInfo{};
        mach_msg_type_number_t regCount = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t regObj = MACH_PORT_NULL;
        if (mach_vm_region(dbg.taskPort(), &regAddr, &regSize, VM_REGION_BASIC_INFO_64,
                           reinterpret_cast<vm_region_info_t>(&regInfo), &regCount,
                           &regObj) != KERN_SUCCESS) {
            fprintf(stdout, "M2: mach_vm_region(translate_insn=0x%llx) failed\n",
                    translateInsnAddr);
            return 1;
        }
        if (translateInsnAddr < regAddr || translateInsnAddr >= regAddr + regSize) {
            fprintf(stdout,
                    "M2: translate_insn (0x%llx) not in region [0x%llx, "
                    "0x%llx)\n",
                    translateInsnAddr, static_cast<uint64_t>(regAddr), (regAddr + regSize));
            return 1;
        }
        const auto codeRegStart = static_cast<uint64_t>(regAddr);
        const auto codeRegEnd = (regAddr + regSize);
        VERBOSE_LOG(
            "M2: code region containing translate_insn: [0x%llx, 0x%llx) "
            "size=0x%llx prot=0x%x\n",
            codeRegStart, codeRegEnd, codeRegEnd - codeRegStart, regInfo.protection);

        // ── Find trailing alignment padding inside that region ──────────────
        // Read the last 64 KB and find the last non-zero byte, then bytes
        // after that are trailing padding.
        constexpr uint64_t kScanWindow = 0x10000;  // 64 KB
        uint64_t scanWindow = std::min(kScanWindow, codeRegEnd - codeRegStart);
        uint64_t scanStart = codeRegEnd - scanWindow;
        std::vector<uint8_t> tail(scanWindow, 0);
        if (!dbg.readMemory(scanStart, tail.data(), scanWindow)) {
            fprintf(stdout, "M2: failed to read code-region tail at 0x%llx\n", scanStart);
            return 1;
        }
        ssize_t lastNonZero = -1;
        for (ssize_t i = static_cast<ssize_t>(scanWindow) - 1; i >= 0; i--) {
            if (tail[i] != 0) {
                lastNonZero = i;
                break;
            }
        }
        if (lastNonZero < 0) {
            fprintf(stdout, "M2: code-region tail is all zeros, refusing\n");
            return 1;
        }
        uint64_t padStartOff =
            (static_cast<uint64_t>(lastNonZero + 1) + 3) & ~static_cast<uint64_t>(3);
        uint64_t padStartAddr = scanStart + padStartOff;
        // Zero bytes after the last non-zero one may still belong to a section
        // (__chain_starts ends in zeros). With the image base known and the
        // live region ending where the on-disk __TEXT does, start after the
        // last section; otherwise keep a margin past the last non-zero byte.
        if (libBase != 0 && offsetFinder.offsetTextSectionsEnd_ != 0 &&
            codeRegEnd == libBase + offsetFinder.offsetTextSegmentEnd_) {
            const uint64_t sectionsEnd = (libBase + offsetFinder.offsetTextSectionsEnd_ + 15) &
                                         ~static_cast<uint64_t>(15);
            padStartAddr = std::max(padStartAddr, sectionsEnd);
        } else {
            fprintf(stdout,
                    "M2: __TEXT layout unknown (base 0x%llx); keeping 64 bytes past the last "
                    "non-zero byte\n",
                    libBase);
            padStartAddr += 64;
        }
        if (padStartAddr >= codeRegEnd) {
            fprintf(stdout, "M2: no __TEXT padding left, refusing\n");
            return 1;
        }
        uint64_t padBytes = codeRegEnd - padStartAddr;
        VERBOSE_LOG("M2: __TEXT trailing padding starts at 0x%llx, %llu bytes free\n", padStartAddr,
                    padBytes);

        // ── Install Mach service port in parent ─────────────────────────────
        mach_port_t servicePort = MACH_PORT_NULL;
        uint32_t parentReqName = 0;
        uint32_t parentReplyName = 0;
        if (!sidecar::installPortInParent(dbg.taskPort(), &servicePort, &parentReqName,
                                          &parentReplyName)) {
            fprintf(stdout, "M2: sidecar::installPortInParent failed\n");
            return 1;
        }
        VERBOSE_LOG("M2: parent req port 0x%x  reply port 0x%x  local service 0x%x\n",
                    parentReqName, parentReplyName, servicePort);

        // ── Assemble stub bytes ─────────────────────────────────────────────
        // OUR_HANDLER + STASH + STASH_JUMP go to padStartAddr.
        // ENTRY (16-byte abs-jump to OUR_HANDLER) goes to translate_insn[0..16].
        auto blobs = stub_asm::build(padStartAddr, translateInsnAddr, origPrologue, parentReqName,
                                     parentReplyName);
        if (blobs.entry.size() != 16) {
            fprintf(stdout, "M2: stub_asm::build returned wrong entry size %zu\n",
                    blobs.entry.size());
            return 1;
        }
        if (blobs.handler.size() > padBytes) {
            fprintf(stdout,
                    "M2: handler blob (%zu bytes) doesn't fit in trailing "
                    "padding (%llu bytes)\n",
                    blobs.handler.size(), padBytes);
            return 1;
        }
        VERBOSE_LOG("M2: handler blob = %zu bytes (fits in %llu padding)\n", blobs.handler.size(),
                    padBytes);

        // Write into the tracee's executable text. The pad and the patch
        // sites share pages with libRosettaRuntime's own code, so once a page
        // has been made writable it is put back to read+execute whether or
        // not the write succeeded; a page left without execute would crash a
        // tracee that later runs without the hook.
        auto writeText = [&](uint64_t addr, const void* data, size_t len) -> bool {
            if (!dbg.adjustMemoryProtection(addr, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY,
                                            len)) {
                return false;
            }
            const bool wrote = dbg.writeMemory(addr, data, len);
            const bool executable =
                dbg.adjustMemoryProtection(addr, VM_PROT_READ | VM_PROT_EXECUTE, len);
            return wrote && executable;
        };

        // ── Write OUR_HANDLER + STASH + STASH_JUMP into trailing padding ───
        if (!writeText(padStartAddr, blobs.handler.data(), blobs.handler.size())) {
            fprintf(stdout, "M2: failed to write handler blob\n");
            return 1;
        }
        VERBOSE_LOG("M2: handler installed at 0x%llx\n", padStartAddr);

        // ── Install transcendental polynomial constants ─────────────────────
        // 8-byte aligned, right after the stub (transcendental_constants.hpp).
        const uint64_t constsAddr =
            (padStartAddr + blobs.handler.size() + 0x7) & ~static_cast<uint64_t>(0x7);
        const uint64_t constsEnd = constsAddr + sizeof(kTransConstants);
        if (constsEnd > padStartAddr + padBytes) {
            fprintf(stdout,
                    "M2: transcendental constants (%zu B) don't fit in "
                    "trailing padding (free %llu B after handler at 0x%llx)\n",
                    sizeof(kTransConstants), padStartAddr + padBytes - constsAddr, constsAddr);
            return 1;
        }
        // writeText restores RX, not just R: the trailing pad is
        // page-granular RX and the constants live on the same 4 KB page as
        // OUR_HANDLER. Stripping EXECUTE here would kill the handler.
        if (!writeText(constsAddr, &kTransConstants, sizeof(kTransConstants))) {
            fprintf(stdout, "M2: failed to write transcendental constants\n");
            return 1;
        }
        rosetta_core::set_transcendental_constants_addr(constsAddr);
        VERBOSE_LOG("M2: transcendental constants installed at 0x%llx (%zu B)\n", constsAddr,
                    sizeof(kTransConstants));

        // Once an entry is patched, the tracee branches into the stub on its
        // next translation. What is written before that lands in padding
        // that nothing executes. Leaving this scope between the first entry
        // write and the release, by a return or an exception, puts the
        // displaced bytes back and releases the tracee, which then runs
        // without the hook instead of paying a failed IPC per translation.
        struct PatchedEntry {
            uint64_t addr;
            uint8_t orig[16];
        };
        std::vector<PatchedEntry> patchedEntries;
        patchedEntries.reserve(1);  // push_back below must not throw
        // The entry written, for the tracee's i-cache invalidation.
        auto setEntryIcacheSpan = [&]() {
            coopIcacheAddr[0] = translateInsnAddr;
            coopIcacheLen[0] = sizeof(PatchedEntry::orig);
        };
        auto restoreEntry = [&](const PatchedEntry& e) -> bool {
            if (!writeText(e.addr, e.orig, sizeof(e.orig))) {
                fprintf(stdout, "M2: failed to restore the original bytes at 0x%llx\n", e.addr);
                return false;
            }
            return true;
        };
        ScopeExit rollback([&]() noexcept {
            size_t restored = 0;
            for (const auto& e : patchedEntries) {
                restored += restoreEntry(e) ? 1 : 0;
            }
            fprintf(stdout,
                    "M2: hook abandoned; restored %zu of %zu patched entries, releasing the "
                    "target without the hook\n",
                    restored, patchedEntries.size());
            if (restored != patchedEntries.size()) {
                fprintf(stdout,
                        "M2: a patched entry could not be restored; the target falls back to "
                        "stock on each translation\n");
            }
            fflush(stdout);
            // Release now rather than by exiting, so the tracee also
            // invalidates its i-cache over the restored entries.
            setEntryIcacheSpan();
            coopIcacheAddr[1] = 0;
            coopIcacheLen[1] = 0;
            releaseTracee();
        });

        // ── Patch translate_insn[0..16] with the abs-jump ENTRY ────────────
        PatchedEntry translateEntry{.addr = translateInsnAddr, .orig = {}};
        memcpy(translateEntry.orig, origPrologue, sizeof(translateEntry.orig));
        patchedEntries.push_back(translateEntry);
        if (!writeText(translateInsnAddr, blobs.entry.data(), blobs.entry.size())) {
            fprintf(stdout, "M2: failed to patch the translate_insn entry\n");
            return 1;
        }
        VERBOSE_LOG("M2: translate_insn entry patched (abs-jump to 0x%llx)\n", padStartAddr);
        // Record the patched code ranges so the tracee can invalidate its own
        // i-cache. The entry (hot in the i-cache post-init) is the one that
        // MUST be flushed; the handler lives in never-executed padding, but
        // flush it too for safety.
        //
        setEntryIcacheSpan();
        coopIcacheAddr[1] = padStartAddr;
        coopIcacheLen[1] = blobs.handler.size();

        // Capture the tracee's task port BEFORE releasing it. The send-right is
        // held in MuhDebugger::taskPort_ and stays valid past release (the
        // handshake reply); only ~MuhDebugger
        // releases it. The receive thread uses it for mach_vm_read on
        // TranslationResult / IRInstr structs.
        mach_port_t parentTaskPort = dbg.taskPort();

        // Spawn the Mach receive thread before the release, so a failure
        // can still undo the patch, and before the kqueue wait below so
        // any in-flight tickle messages from the parent get drained while
        // we sit on kqueue. Detached thread; cleaned up on process exit.
        if (!sidecar::spawnReceiveThread(servicePort, parentTaskPort)) {
            fprintf(stdout, "M2: failed to spawn receive thread\n");
            return 1;
        }
        exitWatch = armExitWatch(parentPid);
        releaseTracee();
        rollback.armed = false;
        unhookedNotice.prog = nullptr;
        VERBOSE_LOG("M2: receive thread running; entering kqueue wait\n");

        // Self-test: send a tickle message from this process to our own
        // service port. If the receive thread picks it up, the receive
        // plumbing is healthy (and the issue is on the parent side).
        {
            mach_msg_header_t selfMsg{};
            selfMsg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
            selfMsg.msgh_size = sizeof(selfMsg);
            selfMsg.msgh_remote_port = servicePort;
            selfMsg.msgh_local_port = MACH_PORT_NULL;
            selfMsg.msgh_id = 0xCAFEBABE;
            kern_return_t kr = mach_msg(&selfMsg, MACH_SEND_MSG, sizeof(selfMsg), 0, MACH_PORT_NULL,
                                        MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
            VERBOSE_LOG("M2: self-test mach_msg send → 0x%x (%s)\n", kr, mach_error_string(kr));
        }
    }

    // Block until the parent (wine) exits, on the watch armed before the
    // release above.
    waitExitWatch(exitWatch, parentPid);

    return 0;
} catch (const std::exception& e) {
    if (g_unhookedArgv != nullptr) {
        return runUnhooked(std::string("exception: ") + e.what());
    }
    fprintf(stderr, "rosettax87: %s\n", e.what());
    return 1;
}

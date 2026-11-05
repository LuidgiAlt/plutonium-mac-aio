#include "sidecar.hpp"

#include <mach/mach.h>
#include <mach/mach_port.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "rosetta_core/Fixup.h"
#include "rosetta_core/IRInstr.h"
#include "rosetta_core/IRModuleData.h"
#include "rosetta_core/Opcode.h"
#include "rosetta_core/ProfileRuntime.h"
#include "rosetta_core/ThreadContextOffsets.h"
#include "rosetta_core/TransactionalList.h"
#include "rosetta_core/TranslationResult.h"
#include "rosetta_core/Translator.h"
#include "rosetta_core/X87Cache.h"

namespace sidecar {

namespace {

// Largest message we expect to receive — header (24) + 5×8 args (40) +
// trailer + future payload. 4 KB is generous.
constexpr size_t kRecvBufferSize = 4096;

struct ThreadArgs {
    mach_port_t servicePort;
    mach_port_t parentTaskPort;
};

// Requests declined after an allocation failure, numbered in the log line.
std::atomic<uint64_t> g_statAllocDeclines{0};

// ── Cross-process marshalling helpers ───────────────────────────────────────
// Translator + its helpers grow `insn_buf` (mmap/calloc) and append to six
// fixup lists (`::operator new`). Both allocators land in *this* process, so
// we can't simply hand parent's pointers to Translator — the resulting
// pointers would be unreachable from the parent.
//
// Strategy:
//   1. Read parent's TR, ThreadContextOffsets, and IR array into locals.
//      sizeof(tr) bytes are read in full — the loader's M2 init patched
//      stock's TR allocator to allocate sizeof(TranslationResult) per TR
//      so parent's heap has the full extended struct (including our
//      appended x87_cache, OPT-1).
//   2. RESET TR's mutable buffers to empty (data=null, end=0, end_cap=0,
//      use_heap=1) and lists to nullptr. With use_heap=1 grow uses calloc
//      (no munmap of foreign pointers); with empty lists push_back_slow's
//      `delete old_begin` is `delete nullptr`, which is a no-op.
//   3. Run Translator on `tr`. Its growth/pushes allocate fresh sidecar-
//      local heap; the local TR's data/list pointers now name those.
//   4. APPEND the locally-produced bytes/fixups to parent's existing
//      buffers. If the append fits within parent's capacity we just
//      mach_vm_write the delta. If it doesn't, allocate a parent-side
//      replacement via `mach_vm_allocate`, copy parent's existing
//      contents over, then append the new tail. Update TR's pointers to
//      the parent VA.
//   5. mach_vm_write the patched TR back in full (sizeof(TranslationResult))
//      — including x87_cache so OPT-1's cross-instruction state persists.
//      Free our local allocations.
//
// Parent's old buffer (when we replace it on grow) becomes orphaned in
// parent's heap — we can't `free()` parent-side from here. The leak is
// per-grow only; capacity doubles each time so growths are logarithmic.

constexpr size_t kListCount = 6;

// ── Failed asserts decline the request ─────────────────────────────────────
// The translator keeps its asserts in release builds. A failed one used to
// abort the sidecar, which ended the acceleration for the rest of the run
// (and, before the stub learnt to fall back, killed the target). Instead
// assert() throws, the request handler frees what the translator allocated
// and replies None without writing the TR, and stock translates that one
// instruction. The x87 cache is dropped like on any other bypass.
// Defining __assert_rtn here takes the place of libSystem's for this binary.
struct TranslatorAssert {
    const char* expr;
    const char* file;
    int line;
};
std::atomic<uint64_t> g_statAsserts{0};

}  // namespace

extern "C" void __assert_rtn(const char* func, const char* file, int line,
                                          const char* expr) {
    (void)func;
    throw TranslatorAssert{.expr = expr, .file = file, .line = line};
}

namespace {

// ── OPT-1 cache storage (mode-independent) ──────────────────────────────────
// x87_cache is per-thread, cross-instruction Translator state. It used to be
// appended to the tracee's TranslationResult and round-tripped through the
// tracee's heap — which forced the M2 install to enlarge every TR (the TR-size
// MOVZ patch), and that patch is only safe before the first TR is allocated
// (pre-Rosetta-init). We now keep the cache HERE, keyed by the per-thread TR
// address the tracee passes in each request, so the tracee's TR stays
// stock-sized and no TR-size patch is needed. This is what lets the JIT-hook
// install work identically for default (task_for_pid+ptrace, stopped pre-init)
// and cooperative (task-port handshake, attached post-init) attach.
// Stock's real TranslationResult is 0x268 bytes — the size stock's own
// allocator MOVZ passes (offset_finder's translation_result_size_pattern; the
// AOT path patches this same MOVZ to 0x400 to make room for the appended
// x87_cache).  Our struct definition carries reverse-engineered tail fields
// past that (segments_*, field_280) up to offsetof(x87_cache)=0x288 — fields
// stock never allocates.  We read/write back exactly stock's real size: stock
// places each block's code buffer in the adjacent heap chunk (observed at
// tr+0x280), so writing even the extra bytes past 0x268 overran into and
// zeroed the block's first emitted ARM word — a 0x00000000 UDF that crashed
// WoW on nearly every fld block.  The translator only ever touches fields well
// below 0x268 (insn_buf, fixup lists, free_gpr/fpr masks, thread_context_
// offsets), so trimming to the real size loses no state.
// kStockTRSize lives in sidecar.hpp; the loader checks it against the
// immediate translator_translate hands its allocator at startup.
static_assert(kStockTRSize <= offsetof(TranslationResult, x87_cache),
              "stock TR size must not exceed our struct's pre-x87_cache prefix");
std::mutex g_x87CacheMu;
std::unordered_map<uint64_t, X87Cache> g_x87Cache;

// ThreadContextOffsets cache, keyed by the tracee-side pointer.  The struct
// describes stock's thread-context layout (process-lifetime constants that
// stock re-materialises on the calling thread's STACK, so the same pointer
// recurs per thread with the same contents).  Each distinct pointer is read
// from the tracee once; every miss cross-checks the new read against the
// first cached copy and logs loudly if the constants assumption ever breaks.
// Receive thread only, no lock.
std::unordered_map<uint64_t, ThreadContextOffsets> g_tcoCache;

// Per-block IR array cache, keyed by TR address (one in-flight block per TR,
// same keying rationale as g_x87Cache above).  The translator never mutates
// the tracee's IR array (TranslatorX87Fusion works on a local copy), so it is
// read-only cross-task and constant while stock walks the block.  But stock
// walks it with one request per x87 run, and re-reading the whole array
// (80 B × num_instrs) on every request was the single largest read on the
// translate path.  Reuse requires the identity triple to match, the walk to
// stay monotonic (a new translation pass restarts at a lower-or-equal index),
// and an 80-byte probe of the current IRInstr to compare equal.  Residual
// ABA needs recycled block/array pointers with a byte-identical current
// IRInstr (incl. guest pc) but different lookahead entries, i.e. guest
// self-modifying code re-decoded at the same pc: accepted.  Entry storage is reused across requests
// either way, which also drops the old per-request vector malloc.  Receive thread only.
struct IRCacheEntry {
    uint64_t block = 0;
    uint64_t instr_array = 0;
    uint64_t num_instrs = 0;
    uint64_t next_idx = 0;  // where the previous reply told stock to continue
    uint64_t hash = 0;      // profile::hash_ir_stream of ir, when hash_valid
    bool hash_valid = false;
    bool has_bcd = false;  // ir contains FBLD or FBSTP; set on every fill
    std::vector<IRInstr> ir;
};
std::unordered_map<uint64_t, IRCacheEntry> g_irCache;

// NOTE on the road not taken: mach_vm_remap(copy=FALSE) of TRACEE-owned pages
// into the sidecar (to turn the TR / insn-buf / fixup reads and writes below
// into memcpys) does NOT work.  The tracee is an x86_64-translated task with
// 4 KB VM pages; remapping its private anonymous memory into our 16 KB arm64
// map silently degrades to copy semantics, so the two views diverge in BOTH
// directions (verified empirically 2026-07: reads return remap-time bytes,
// writes through the local view never reach the tracee).  Sharing only works
// in the other direction, for objects WE allocate and remap INTO the tracee.
// Don't re-attempt.

struct TranslateRequest {
    uint64_t tr_addr;
    uint64_t block;  // opaque IRBlock* — Translator only compares as ptr
    uint64_t instr_array;
    uint64_t num_instrs;
    uint64_t insn_idx;
};

struct TranslateOutcome {
    bool reply_some;  // true → reply Some(value), else None.
    int64_t value;
};

bool readParent(mach_port_t task, uint64_t addr, void* dst, size_t size) {
    if (size == 0) {
        return true;
    }
    mach_vm_size_t got = 0;
    kern_return_t kr =
        mach_vm_read_overwrite(task, addr, size, reinterpret_cast<mach_vm_address_t>(dst), &got);
    return kr == KERN_SUCCESS && got == size;
}

bool writeParent(mach_port_t task, uint64_t addr, const void* src, size_t size) {
    if (size == 0) {
        return true;
    }
    return mach_vm_write(task, addr, reinterpret_cast<vm_offset_t>(const_cast<void*>(src)), size) ==
           KERN_SUCCESS;
}

// Allocate a parent-side replacement buffer of size `newCap`, copy parent's
// existing live bytes, then append `tailSize` bytes from `tail`. On success
// returns the parent VA of the new buffer. On any failure deallocates and
// returns 0.
mach_vm_address_t allocAndAppendInParent(mach_port_t parentTask, uint64_t origAddr,
                                         uint64_t origLive, uint64_t newCap, const void* tail,
                                         uint64_t tailSize) {
    // Round up to page granularity.
    newCap = (newCap + 0xFFF) & ~static_cast<uint64_t>(0xFFF);
    mach_vm_address_t parentNew = 0;
    if (mach_vm_allocate(parentTask, &parentNew, newCap, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        return 0;
    }
    if (origLive > 0) {
        std::vector<uint8_t> stash(origLive);
        if (!readParent(parentTask, origAddr, stash.data(), origLive) ||
            !writeParent(parentTask, parentNew, stash.data(), origLive)) {
            mach_vm_deallocate(parentTask, parentNew, newCap);
            return 0;
        }
    }
    if (tailSize > 0) {
        if (!writeParent(parentTask, parentNew + origLive, tail, tailSize)) {
            mach_vm_deallocate(parentTask, parentNew, newCap);
            return 0;
        }
    }
    return parentNew;
}

// Run Translator and write its output back to parent's TR. Returns Some(N)
// when translation produced a result and the write-back path completed;
// otherwise returns None (the stub falls through to stock translate_insn).
TranslateOutcome processTranslateRequest(mach_port_t parentTask, const TranslateRequest& req) {
    TranslateOutcome out{.reply_some = false, .value = 0};

    constexpr uint64_t kMaxNumInstrs = 0x10000;
    if (req.num_instrs == 0 || req.num_instrs > kMaxNumInstrs) {
        return out;
    }
    if (req.insn_idx >= req.num_instrs) {
        return out;
    }

    // Read parent's TR. Value-initialised local; we sterilise its list
    // pointers before scope end so `~TransactionalList` runs `::operator
    // delete(nullptr)` (a no-op) instead of freeing arbitrary parent VAs.
    // Value-init (not plain default-init) so the reverse-engineered tail past
    // stock's real 0x268 size — which we deliberately do NOT read from the
    // tracee — is deterministic zero rather than uninitialised.
    TranslationResult tr{};
    // Read only the stock-sized TR from the tracee; x87_cache lives in our own
    // per-thread map (keyed by TR address), not in the tracee's heap.
    if (!readParent(parentTask, req.tr_addr, &tr, kStockTRSize)) {
        return out;
    }
    {
        std::scoped_lock lk(g_x87CacheMu);
        tr.x87_cache = g_x87Cache[req.tr_addr];  // default-constructs on first use
    }

    // Any None reply produced WITHOUT running the translator (the early
    // failure returns below) must not leave the persisted per-TR X87Cache
    // trusting mid-run registers: stock translates the op itself and
    // clobbers the GPRs the cache thinks are holding TOP/base, so the next
    // sidecar-translated op would miscompile.  The translator's own None
    // paths invalidate in its default case; this guard covers every bypass
    // in one place.
    struct CacheBypassGuard {
        uint64_t tr_addr;
        bool ran_translator = false;
        ~CacheBypassGuard() {
            if (!ran_translator) {
                std::scoped_lock lk(g_x87CacheMu);
                auto it = g_x87Cache.find(tr_addr);
                if (it != g_x87Cache.end()) {
                    it->second.invalidate();
                    it->second.prev_block = nullptr;
                }
            }
        }
    } _bypass_guard{.tr_addr = req.tr_addr};

    TransactionalList<Fixup>* lists[kListCount] = {
        &tr.external_fixups, &tr.internal_fixups,  &tr._fixups,
        &tr.field_B0,        &tr.dyld_stub_fixups, &tr.field_1A8,
    };

    struct Sterilizer {
        TransactionalList<Fixup>** ls;
        ~Sterilizer() {
            for (size_t i = 0; i < kListCount; i++) {
                ls[i]->begin = ls[i]->end = ls[i]->end_cap = nullptr;
                ls[i]->_size = 0;
            }
        }
    } _sterilizer{lists};

    // Snapshot parent-side state we need for write-back.
    uint32_t* const origInsnData = tr.insn_buf.data;
    uint64_t const origInsnEnd = tr.insn_buf.end;
    uint64_t const origInsnCap = tr.insn_buf.end_cap;
    uint32_t const origInsnUseHeap = tr.insn_buf.use_heap;
    ThreadContextOffsets* const origTCO = tr.thread_context_offsets;

    struct ListBackup {
        Fixup* begin;
        Fixup* end;
        Fixup* end_cap;
        uint64_t _size;
    } origLists[kListCount];
    for (size_t i = 0; i < kListCount; i++) {
        origLists[i] = {.begin = lists[i]->begin,
                        .end = lists[i]->end,
                        .end_cap = lists[i]->end_cap,
                        ._size = lists[i]->_size};
    }

    // IR array: serve from the per-block cache when possible (see the
    // g_irCache comment for the reuse conditions and the ABA argument);
    // otherwise read it in full from the tracee.
    IRCacheEntry& irc = g_irCache[req.tr_addr];
    {
        // Reuse only for a request at or past the frontier the previous
        // reply set: stock walks a block front to back, so anything behind
        // it is a pass started over, whose IR may have been re-decoded into
        // the same array.
        bool reuse = irc.block == req.block && irc.instr_array == req.instr_array &&
                     irc.num_instrs == req.num_instrs && req.insn_idx >= irc.next_idx;
        if (reuse) {
            // Probe a window, not one instruction: the translator reads the
            // whole run's lookahead (fusions, the IR pipeline, bridging via
            // flag_liveness), and flag_liveness is recomputed from the
            // block's successors on every decode, so the cached tail can go
            // stale without the guest code changing.
            constexpr uint64_t kProbeWindow = 32;
            IRInstr probe[kProbeWindow];
            uint64_t win = req.num_instrs - req.insn_idx;
            if (win > kProbeWindow) {
                win = kProbeWindow;
            }
            if (!readParent(parentTask, req.instr_array + req.insn_idx * sizeof(IRInstr), probe,
                            win * sizeof(IRInstr))) {
                return out;
            }
            reuse = std::memcmp(probe, &irc.ir[req.insn_idx], win * sizeof(IRInstr)) == 0;
        }
        if (!reuse) {
            irc.ir.resize(req.num_instrs);
            if (!readParent(parentTask, req.instr_array, irc.ir.data(),
                            req.num_instrs * sizeof(IRInstr))) {
                // Don't leave a half-valid entry behind: the vector was
                // already resized, so stale keys could pair with the wrong
                // length and index out of bounds on a later request.
                irc = IRCacheEntry{};
                return out;
            }
            irc.block = req.block;
            irc.instr_array = req.instr_array;
            irc.num_instrs = req.num_instrs;
            irc.hash_valid = false;
            irc.has_bcd = std::ranges::any_of(irc.ir, [](const IRInstr& ins) {
                return ins.opcode() == kOpcodeName_fbld || ins.opcode() == kOpcodeName_fbstp;
            });
        }
        // Conservative until the reply is known; the Some path below moves it.
        irc.next_idx = req.insn_idx + 1;
    }
    IRInstr* const localIR = irc.ir.data();

    if (origTCO == nullptr) {
        return out;
    }
    ThreadContextOffsets localTCO{};
    {
        const uint64_t tcoAddr = reinterpret_cast<uint64_t>(origTCO);
        auto it = g_tcoCache.find(tcoAddr);
        if (it != g_tcoCache.end()) {
            localTCO = it->second;
        } else {
            if (!readParent(parentTask, tcoAddr, &localTCO, sizeof(localTCO))) {
                return out;
            }
            // Canary for the constants assumption: all TCOs in a process
            // must describe the same layout.
            if (!g_tcoCache.empty() &&
                std::memcmp(&g_tcoCache.begin()->second, &localTCO, sizeof(localTCO)) != 0) {
                fprintf(stdout,
                        "[rosettax87] WARNING: ThreadContextOffsets at 0x%llx differs from "
                        "cached layout; TCO cache assumption broken\n",
                        static_cast<unsigned long long>(tcoAddr));
                fflush(stdout);
            }
            g_tcoCache.emplace(tcoAddr, localTCO);
        }
    }

    // x87_cache (OPT-1) was loaded from g_x87Cache above, not from the tracee.
    // On the first request for a TR address it default-constructs; Translator's
    // cache.invalidate() converges it on the first block mismatch.

    // Set up local insn_buf with capacity ≥ parent's. Critical: end starts at
    // origInsnEnd so Translator's emit/fixup offsets count in the SAME
    // coordinate space the parent uses (`data + insn_offset`). If we started
    // end at 0, fixups referencing emitted bytes would patch into parent's
    // pre-existing prologue bytes when stock later applies them — corruption
    // that crashes parent with EXC_BAD_INSTRUCTION.
    //
    // use_heap=1 ensures grow() picks calloc and skips its munmap-of-old-
    // pointer branch (the "old" pointer would otherwise be foreign memory).
    std::vector<uint8_t> localInsnVec(std::max<uint64_t>(origInsnCap, 0x4000));
    tr.insn_buf.data = reinterpret_cast<uint32_t*>(localInsnVec.data());
    tr.insn_buf.end = origInsnEnd;
    tr.insn_buf.end_cap = localInsnVec.size();
    tr.insn_buf.use_heap = 1;
    for (auto& list : lists) {
        list->begin = list->end = list->end_cap = nullptr;
        list->_size = 0;
    }
    tr.thread_context_offsets = &localTCO;

    // A block containing FBLD or FBSTP goes to stock whole.  An 18-digit BCD integer needs up to 60
    // significand bits, which stock's 80-bit registers hold and the binary64 register file does
    // not, and the value usually reaches FBSTP through other instructions of the same block (FILD
    // m64 in Delphi's CurrToStr, the power-of-ten scaling in FloatToDecimal) that have to stay in
    // 80 bits as well.  Such blocks are rare, and the inline FBSTP measured only about 1.7x faster
    // than stock's.  The scan is done once per IR-cache fill (irc.has_bcd).
    const bool stock_bcd_hit = irc.has_bcd;

    // On a stock hit ran_translator stays false and the bypass guard
    // invalidates the persisted X87Cache, which is what a None reply that
    // skipped the translator needs.
    std::optional<int64_t> result;
    if (!stock_bcd_hit) {
        _bypass_guard.ran_translator = true;
        try {
            result = Translator::translate_instruction(
                &tr, reinterpret_cast<IRBlock*>(req.block), localIR,
                static_cast<int64_t>(req.num_instrs), static_cast<int64_t>(req.insn_idx));
        } catch (const TranslatorAssert& a) {
            const uint64_t n = g_statAsserts.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 10) {
                const uint16_t op = localIR[req.insn_idx].opcode();
                fprintf(stdout,
                        "[rosettax87] assert failed (%s, %s:%d) translating pc=%08x op=%s; "
                        "left to stock (#%llu)\n",
                        a.expr, a.file, a.line, localIR[req.insn_idx].pc,
                        op < kOpcodeNames.size() ? kOpcodeNames[op] : "?",
                        static_cast<unsigned long long>(n));
                fflush(stdout);
            }
            // Free what the translator allocated; the Sterilizer then clears
            // the list pointers. The parent's TR is not written.
            if (reinterpret_cast<uint8_t*>(tr.insn_buf.data) != localInsnVec.data()) {
                free(tr.insn_buf.data);
            }
            for (auto* list : lists) {
                ::operator delete(list->begin);
            }
            _bypass_guard.ran_translator = false;  // the guard drops the x87 cache
            irc = IRCacheEntry{};
            return out;
        }
        if (result.has_value()) {
            irc.next_idx = result.value();
        }
    }

    // Capture growth state. If insn_buf grew, Translator's grow() abandoned
    // localInsnVec for a calloc'd buffer (we own that and must free it).
    auto* const localInsnData = reinterpret_cast<uint8_t*>(tr.insn_buf.data);
    bool const insnGrew = (localInsnData != localInsnVec.data());
    uint64_t const insnEmitted = tr.insn_buf.end - origInsnEnd;
    Fixup* localPushed[kListCount];
    uint64_t localPushedBytes[kListCount];
    for (size_t i = 0; i < kListCount; i++) {
        localPushed[i] = lists[i]->begin;
        localPushedBytes[i] = static_cast<uint64_t>(reinterpret_cast<uint8_t*>(lists[i]->end) -
                                                    reinterpret_cast<uint8_t*>(lists[i]->begin));
    }
    struct LocalCleanup {
        uint8_t* insn_buf;  // null if Translator never grew (vec owns)
        Fixup* lists[kListCount];
        ~LocalCleanup() {
            if (insn_buf) {
                free(insn_buf);
            }
            for (auto& list : lists) {
                if (list) {
                    ::operator delete(list);
                }
            }
        }
    } _cleanup{.insn_buf = insnGrew ? localInsnData : nullptr,
               .lists = {localPushed[0], localPushed[1], localPushed[2], localPushed[3],
                         localPushed[4], localPushed[5]}};

    // We always write the TR back, even on None — Translator's default case
    // (and other unhandled paths) calls cache.invalidate() and resets the
    // scratch register masks; if we don't propagate those, parent ends up
    // with stale `cache.gprs_valid=1` from the previous Some translation
    // while stock's now-running translate_insn (for the unhandled opcode)
    // happily clobbers the GPRs the cache claims are still holding TOP /
    // base. The next x87 op would then trust the cache and emit wrong code.
    // Restore parent VAs in TR before any conditional data writes below;
    // the data-write path will re-pivot insn_buf/list pointers if grow
    // happened.
    tr.insn_buf.data = origInsnData;
    tr.insn_buf.end = origInsnEnd;
    tr.insn_buf.end_cap = origInsnCap;
    tr.insn_buf.use_heap = origInsnUseHeap;
    tr.thread_context_offsets = origTCO;
    for (size_t i = 0; i < kListCount; i++) {
        lists[i]->begin = origLists[i].begin;
        lists[i]->end = origLists[i].end;
        lists[i]->end_cap = origLists[i].end_cap;
        lists[i]->_size = origLists[i]._size;
    }

    if (result.has_value()) {
        // Write insn_buf delta bytes (the region Translator emitted, at
        // offsets [origInsnEnd .. origInsnEnd+emitted]) back to parent.
        // Two cases:
        //  - No grow + fits in parent's cap → mach_vm_write the tail
        //    in place.
        //  - Grow OR doesn't fit → allocate a parent-side replacement,
        //    copy parent's existing [0..origInsnEnd] bytes over, then
        //    append our emitted slice, and pivot TR.insn_buf.data onto
        //    it.
        uint64_t finalInsnEnd = origInsnEnd + insnEmitted;
        uint32_t* finalInsnData = origInsnData;
        uint64_t finalInsnCap = origInsnCap;
        if (!insnGrew && finalInsnEnd <= origInsnCap) {
            if (insnEmitted > 0) {
                if (!writeParent(parentTask, reinterpret_cast<uint64_t>(origInsnData) + origInsnEnd,
                                 localInsnData + origInsnEnd, insnEmitted)) {
                    return out;
                }
            }
        } else {
            uint64_t newCap = std::max(origInsnCap * 2, finalInsnEnd);
            mach_vm_address_t parentNew = allocAndAppendInParent(
                parentTask, reinterpret_cast<uint64_t>(origInsnData), origInsnEnd, newCap,
                localInsnData + origInsnEnd, insnEmitted);
            if (parentNew == 0) {
                return out;
            }
            finalInsnData = reinterpret_cast<uint32_t*>(parentNew);
            finalInsnCap = (newCap + 0xFFF) & ~static_cast<uint64_t>(0xFFF);
        }
        tr.insn_buf.data = finalInsnData;
        tr.insn_buf.end = finalInsnEnd;
        tr.insn_buf.end_cap = finalInsnCap;

        // Append each list's pushed entries to parent — same fits-or-grow
        // split.
        for (size_t i = 0; i < kListCount; i++) {
            const auto& orig = origLists[i];
            uint64_t parentLive =
                reinterpret_cast<uint8_t*>(orig.end) - reinterpret_cast<uint8_t*>(orig.begin);
            uint64_t parentCap =
                reinterpret_cast<uint8_t*>(orig.end_cap) - reinterpret_cast<uint8_t*>(orig.begin);
            uint64_t added = localPushedBytes[i];
            uint64_t newLive = parentLive + added;

            if (newLive <= parentCap) {
                if (added > 0) {
                    if (!writeParent(parentTask, reinterpret_cast<uint64_t>(orig.end),
                                     localPushed[i], added)) {
                        return out;
                    }
                }
                lists[i]->end =
                    reinterpret_cast<Fixup*>(reinterpret_cast<uint8_t*>(orig.begin) + newLive);
            } else {
                uint64_t newCap = std::max(parentCap * 2, newLive);
                mach_vm_address_t parentNew =
                    allocAndAppendInParent(parentTask, reinterpret_cast<uint64_t>(orig.begin),
                                           parentLive, newCap, localPushed[i], added);
                if (parentNew == 0) {
                    return out;
                }
                uint64_t roundedCap = (newCap + 0xFFF) & ~static_cast<uint64_t>(0xFFF);
                lists[i]->begin = reinterpret_cast<Fixup*>(parentNew);
                lists[i]->end = reinterpret_cast<Fixup*>(parentNew + newLive);
                lists[i]->end_cap = reinterpret_cast<Fixup*>(parentNew + roundedCap);
            }
        }
    }

    // Persist OPT-1's cross-instruction cache in our own per-thread map (not the
    // tracee's TR). Always — even on None, Translator's default case calls
    // cache.invalidate() and resets scratch masks; dropping that would leave the
    // next call trusting stale gprs_valid state.
    {
        std::scoped_lock lk(g_x87CacheMu);
        g_x87Cache[req.tr_addr] = tr.x87_cache;
    }

    // Write back only the stock-sized TR (propagates scratch-mask updates and
    // any pivoted buffer pointers from the Some path above). x87_cache is NOT
    // written to the tracee — it lives in g_x87Cache — so the tracee's TR needs
    // no enlargement and the M2 install needs no TR-size patch.
    if (!writeParent(parentTask, req.tr_addr, &tr, kStockTRSize)) {
        return out;
    }

    if (result.has_value()) {
        out.reply_some = true;
        out.value = result.value();
    } else {
        // The stub's FILTER prologue routes only x87 opcodes to us, so
        // reaching nullopt means the dispatcher returned nullopt for an
        // x87 op.  Two cases:
        //   1. Deliberate fall-through (fxsave/fxrstor): explicit `case`
        //      in Translator.cpp; we don't inline because of the 8 × f80
        //      ST slots that would inherit frstor's eager-conversion
        //      regression.  Stock translates them via shared x22.
        //   2. Forgot-to-handle: an x87 op without a translate_* and
        //      without an entry in kKnownFallThrough below.  This is the
        //      discoverability signal — emit a loud UNHANDLED line so a
        //      future helper-using opcode (e.g. a new transcendental)
        //      doesn't silently compose with stock's {x22, w23} ABI and
        //      produce wrong code.
        static constexpr std::array<uint16_t, 6> kKnownFallThrough = {
            kOpcodeName_fclex,    // metadata-only; inline parity → no win
            kOpcodeName_finit,    // metadata-only; inline 0.95× → no win
            kOpcodeName_fldenv,   // metadata-only; inline parity → no win
            kOpcodeName_fstenv,   // metadata-only; inline 0.66× regression
            kOpcodeName_fxsave,   // SSE-era extended (8×f80 ST + 16×XMM)
            kOpcodeName_fxrstor,  // SSE-era extended
        };
        const uint16_t op = localIR[req.insn_idx].opcode();
        // The translator ran out of scratch registers and put the request
        // back as it found it (Translator::translate_instruction).  Stock
        // translates this instruction and the rest of the block, whose
        // requests come back declined without a line of their own.  One line
        // per failure, since each one is a pressure-model bug.
        if (tr.alloc_failure != nullptr) {
            if (!irc.hash_valid) {
                irc.hash = profile::hash_ir_stream(localIR, req.num_instrs);
                irc.hash_valid = true;
            }
            const uint64_t n = g_statAllocDeclines.fetch_add(1, std::memory_order_relaxed) + 1;
            const char* name = (op < kOpcodeNames.size()) ? kOpcodeNames[op] : "?";
            char line[224];
            snprintf(line, sizeof(line),
                     "[rosettax87] %s failed in block hash=0x%016llx at %s idx=%llu/%llu; "
                     "declined to stock with the rest of the block (#%llu)\n",
                     tr.alloc_failure, static_cast<unsigned long long>(irc.hash), name,
                     static_cast<unsigned long long>(req.insn_idx),
                     static_cast<unsigned long long>(req.num_instrs),
                     static_cast<unsigned long long>(n));
            fputs(line, stdout);
            fflush(stdout);
        }
        // stock_bcd_hit: the None reply is a deliberate whole-block
        // exclusion, not a missing handler.
        const bool declined_block =
            tr.x87_cache.declined_block == reinterpret_cast<const IRBlock*>(req.block);
        const bool deliberate = stock_bcd_hit || tr.alloc_failure != nullptr || declined_block ||
                                std::ranges::find(kKnownFallThrough, op) != kKnownFallThrough.end();
        if (!deliberate) {
            const char* name = (op < kOpcodeNames.size()) ? kOpcodeNames[op] : "?";
            fprintf(stdout,
                    "[rosettax87] UNHANDLED x87 opcode %s (0x%x) at "
                    "insn_idx=%lld; falling through to stock — add a "
                    "translate_* and dispatch case, or extend "
                    "kKnownFallThrough if this is a deliberate-stock op\n",
                    name, static_cast<unsigned>(op), static_cast<long long>(req.insn_idx));
            fflush(stdout);
        }
    }
    return out;
}

// True when `kr` is a receive-side mach_msg error (0x10004xxx, bit 14 set;
// send-side errors are 0x10000xxx).  A combined SEND|RCV call can fail on
// either half: a send-half failure means the reply was NOT queued (we still
// own the SEND_ONCE right) and the receive half never ran; a receive-half
// failure means the reply went out fine.
constexpr bool machMsgRcvError(kern_return_t kr) {
    return (kr & 0x00004000) != 0;
}

void runReceiveLoop(mach_port_t servicePort, mach_port_t parentTaskPort) {
    struct alignas(8) {
        uint8_t bytes[kRecvBufferSize];
    } buf;

    struct ReplyMsg {
        mach_msg_header_t hdr;
        uint64_t result;
        uint64_t some_flag;  // 1 = Some(result), 0 = None (fall through)
    };

    uint64_t send_failures = 0;
    bool replyPending = false;  // buf.bytes holds a ReplyMsg not yet sent
    for (;;) {
        auto* hdr = reinterpret_cast<mach_msg_header_t*>(buf.bytes);
        kern_return_t kr;
        if (replyPending) {
            // MIG-server shape: send the pending reply and block for the
            // next request in ONE trap (the received message reuses the
            // same buffer).  Halves the per-request mach_msg count vs the
            // old separate SEND + RCV calls.
            replyPending = false;
            kr = mach_msg(hdr, MACH_SEND_MSG | MACH_RCV_MSG, sizeof(ReplyMsg), sizeof(buf),
                          servicePort, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
            if (kr != KERN_SUCCESS && !machMsgRcvError(kr)) {
                if (++send_failures <= 5) {
                    fprintf(stdout, "sidecar: reply send failed 0x%x %s\n", kr,
                            mach_error_string(kr));
                }
                // mach_msg consumes the SEND_ONCE on success; on send
                // failure we must drop it ourselves, then retry as a
                // plain receive.
                mach_port_deallocate(mach_task_self(), hdr->msgh_remote_port);
                continue;
            }
        } else {
            hdr->msgh_local_port = servicePort;
            hdr->msgh_size = sizeof(buf);
            kr = mach_msg(hdr, MACH_RCV_MSG, 0, sizeof(buf), servicePort, MACH_MSG_TIMEOUT_NONE,
                          MACH_PORT_NULL);
        }
        if (kr != KERN_SUCCESS) {
            fprintf(stdout, "sidecar: mach_msg(RCV) returned 0x%x (%s)\n", kr,
                    mach_error_string(kr));
            return;
        }

        // M3 payload: header (24 B) + 5 × 8-byte args (40 B) = 64 B.
        // Args are TR*, IRBlock*, IRInstr*, num_instrs, insn_idx — the
        // five translate_insn parameters in register order (x0..x4).
        // Reply path: stub provided a SEND_ONCE on msgh_remote_port (via
        // MAKE_SEND_ONCE on the local-port disposition).  We echo the
        // request's msgh_id back so the stub can detect cross-talk
        // (Step 1b) and put the Some/None signal into a dedicated body
        // word (some_flag) so msgh_id is purely a transaction tag.
        //
        // Dispatch on the top-byte sentinel (0x10) rather than an exact
        // msgh_id match — the bottom 24 bits now carry per-call data.
        mach_port_t replyPort = hdr->msgh_remote_port;
        if (hdr->msgh_size >= 24 + 40 && (hdr->msgh_id & 0xFF000000U) == 0x10000000U &&
            replyPort != MACH_PORT_NULL) {
            const uint32_t reqId = hdr->msgh_id;
            TranslateRequest req{};
            std::memcpy(&req, buf.bytes + 24, sizeof(req));

            TranslateOutcome outcome = processTranslateRequest(parentTaskPort, req);

            // Build the reply in place over the request bytes; it goes out
            // fused with the next receive at the top of the loop.  Every
            // header field is set explicitly since the buffer holds the
            // stale request header.
            auto* reply = reinterpret_cast<ReplyMsg*>(buf.bytes);
            reply->hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
            reply->hdr.msgh_size = sizeof(ReplyMsg);
            reply->hdr.msgh_remote_port = replyPort;
            reply->hdr.msgh_local_port = MACH_PORT_NULL;
            reply->hdr.msgh_voucher_port = MACH_PORT_NULL;
            reply->hdr.msgh_id = reqId;  // echo for transaction match
            reply->result = outcome.reply_some ? static_cast<uint64_t>(outcome.value) : 0;
            reply->some_flag = outcome.reply_some ? 1U : 0U;
            replyPending = true;
        } else if (replyPort != MACH_PORT_NULL &&
                   (hdr->msgh_bits & MACH_MSGH_BITS_REMOTE_MASK) == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
            // Other / malformed message — discard the SEND_ONCE.
            mach_port_deallocate(mach_task_self(), replyPort);
        }
    }
}

void* threadEntry(void* arg) {
    auto* a = reinterpret_cast<ThreadArgs*>(arg);
    runReceiveLoop(a->servicePort, a->parentTaskPort);
    delete a;
    return nullptr;
}

}  // namespace

bool installPortInParent(mach_port_t parentTaskPort, mach_port_t* outServicePort,
                         uint32_t* outParentReqName, uint32_t* outParentReplyName) {
    // Allocate a fresh receive port in this process.
    mach_port_t servicePort = MACH_PORT_NULL;
    kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &servicePort);
    if (kr != KERN_SUCCESS) {
        fprintf(stdout, "sidecar: mach_port_allocate(RECEIVE) failed (0x%x %s)\n", kr,
                mach_error_string(kr));
        return false;
    }

    // Insert a send right (derived from our receive right) so we can
    // hand it across the task boundary.
    kr =
        mach_port_insert_right(mach_task_self(), servicePort, servicePort, MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        fprintf(stdout, "sidecar: mach_port_insert_right(SELF, MAKE_SEND) failed (0x%x %s)\n", kr,
                mach_error_string(kr));
        return false;
    }

    // Allocate a fresh name in the parent task's namespace, then plant
    // our send right under that name. The parent process can mach_msg
    // to that name and the kernel will route to our servicePort.
    mach_port_name_t parentName = MACH_PORT_NULL;
    kr = mach_port_allocate(parentTaskPort, MACH_PORT_RIGHT_DEAD_NAME, &parentName);
    if (kr != KERN_SUCCESS) {
        fprintf(stdout, "sidecar: mach_port_allocate(parent, DEAD_NAME) failed (0x%x %s)\n", kr,
                mach_error_string(kr));
        return false;
    }
    // Drop the placeholder so the name slot is free.
    kr = mach_port_deallocate(parentTaskPort, parentName);
    if (kr != KERN_SUCCESS) {
        fprintf(stdout, "sidecar: mach_port_deallocate(parent placeholder) failed (0x%x %s)\n", kr,
                mach_error_string(kr));
        return false;
    }

    // Plant our send right under that freed name. The kernel resolves
    // (mach_task_self(), servicePort) to the underlying port object and
    // installs a send right at `parentName` in `parentTaskPort`'s ns.
    kr = mach_port_insert_right(parentTaskPort, parentName, servicePort, MACH_MSG_TYPE_COPY_SEND);
    if (kr != KERN_SUCCESS) {
        fprintf(stdout,
                "sidecar: mach_port_insert_right(parent, COPY_SEND) failed "
                "(0x%x %s)\n",
                kr, mach_error_string(kr));
        return false;
    }

    // Verify what's actually at parentName in parent's namespace.
    mach_port_type_t parentType = 0;
    kr = mach_port_type(parentTaskPort, parentName, &parentType);
    if (kr != KERN_SUCCESS) {
        fprintf(stdout, "sidecar: mach_port_type(parent,0x%x) failed 0x%x %s\n", parentName, kr,
                mach_error_string(kr));
    }

    *outServicePort = servicePort;
    *outParentReqName = static_cast<uint32_t>(parentName);

    // Allocate the parent-owned reply port. Stub names it as
    // msgh_local_port + MAKE_SEND_ONCE, so the kernel hands the sidecar
    // a fresh SEND_ONCE per call. Sidecar replies via that send-once
    // and the reply lands here in parent's space; the stub's
    // mach_msg(RCV) drains it.
    mach_port_name_t parentReplyName = MACH_PORT_NULL;
    kr = mach_port_allocate(parentTaskPort, MACH_PORT_RIGHT_RECEIVE, &parentReplyName);
    if (kr != KERN_SUCCESS) {
        fprintf(stdout,
                "sidecar: mach_port_allocate(parent, RECEIVE) for reply "
                "failed (0x%x %s)\n",
                kr, mach_error_string(kr));
        return false;
    }
    *outParentReplyName = static_cast<uint32_t>(parentReplyName);
    return true;
}

bool spawnReceiveThread(mach_port_t servicePort, mach_port_t parentTaskPort) {
    pthread_t thr;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    auto* args = new ThreadArgs{.servicePort = servicePort, .parentTaskPort = parentTaskPort};
    int rc = pthread_create(&thr, &attr, threadEntry, args);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        delete args;
        fprintf(stdout, "sidecar: pthread_create failed (%d)\n", rc);
        return false;
    }

    return true;
}

}  // namespace sidecar

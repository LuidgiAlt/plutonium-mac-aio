#pragma once

#include <cstdint>

struct IRBlock;
struct IRInstr;

// The format of one x87 slot in the native layout (X87Cache::slot_fmt).
enum X87SlotFmt : uint8_t {
    kSlotNative = 0,      // an 80-bit value: Rosetta's, or copied as ten bytes
    kSlotF64 = 1,         // the binary64 working value in the slot's first 8 bytes
    kSlotDead = 2,        // popped or freed in this reply, holding binary64 bits
    kSlotDeadNative = 3,  // popped or freed in this reply, holding 80-bit bits
};

// The slot's bytes are an 80-bit image, live or not.
inline bool x87_slot_holds_f80(uint8_t fmt) {
    return fmt == kSlotNative || fmt == kSlotDeadNative;
}

// The format a slot takes when it is popped or freed.
inline uint8_t x87_slot_killed(uint8_t fmt) {
    return x87_slot_holds_f80(fmt) ? kSlotDeadNative : kSlotDead;
}

// OPT-1: Cross-instruction x87 base/TOP register cache.
//
// When consecutive x87 instructions appear in a block, the base address
// (X18 + x87_state_offset) and the TOP field never change between instructions
// except through our own push/pop (which update the register in-place).
// Caching these two values across instructions saves 3-4 emitted AArch64
// instructions per x87 opcode after the first in a run.
struct X87Cache {
    // Where the previous translate_instruction call in this block entered
    // (last_insn_idx) and where its reply told stock to continue
    // (last_next_idx; -1 after a None reply).  Stock walks a block front to
    // back, so the only continuation of an active run is a request at
    // exactly last_next_idx.  A request at or before last_insn_idx, or
    // anywhere else inside an active run, means stock started the block's
    // translation over: the emitted code that holds base/TOP in registers
    // is gone, and the cache is reset as on a block change.
    int32_t last_insn_idx = -1;
    int32_t last_next_idx = -1;
    int8_t base_gpr = 0;        // GPR holding X87State base
    int8_t top_gpr = 0;         // GPR holding TOP
    int16_t run_remaining = 0;  // Countdown; 0 = inactive
    int8_t st_base_gpr = 0;     // GPR holding &st[0] = Xbase + kX87RegFileOff
    int8_t top_dirty = 0;       // OPT-C: 1 = push skipped store_top, TOP in memory stale
    int8_t gprs_valid = 0;      // 1 = base/top GPR numbers are meaningful
    int8_t st_base_valid = 0;   // 1 = st_base_gpr is currently pinned & meaningful.
                                // Independent of gprs_valid so that X87IRLower's
                                // epilogue can free Xst_base before the tag-batch
                                // alloc (drops peak GPR by 1) while keeping
                                // base_gpr/top_gpr pinned for the run's tail.
    int8_t tag_push_pending =
        0;  // OPT-D: 1 = push's tag-valid update deferred (cancel on next pop)
    int8_t deferred_pop_count =
        0;  // OPT-D2: number of pop tag-set-empty updates deferred to run end
    // 1 while the current reply is a native transport reply (see
    // is_native_transport_run): its ops work on Rosetta's packed 80-bit
    // slots directly and the reply has no compact binary64 state at all.
    // Set and cleared by Translator::translate_instruction around one
    // reply; invalidate() and tick() leave it alone.
    int8_t native_transport = 0;
    // 1 while the current reply keeps its working binary64 values inside
    // Rosetta's native slots (+6, stride 10) instead of
    // the compact layout.  Set and cleared around one reply like
    // native_transport.
    int8_t native_slots = 0;
    // 1 when the run started at the current request
    // holds only fldcw and fnstcw, which never use TOP, so x87_begin does not
    // load it.  A native-slot reply is often short, and the status-word load
    // would read part of the status and tag word that the reply before may
    // have just stored as one 32-bit word (the single-op fast path), which
    // stalls: a lone fld m64 followed by Delphi's Trunc sequence (fnstcw;
    // fnstcw; fldcw; fistp; fldcw) ran 6 percent longer with the load.
    // Recomputed at every run start.
    int8_t top_unused = 0;
    // What each physical slot holds, indexed by its position relative to
    // TOP at the start of the reply, (top_rel + depth) & 7, where top_rel
    // is the static change of TOP since then.  Every TOP change inside a
    // reply is a constant except frstor, which starts the count again.
    int8_t top_rel = 0;
    uint8_t slot_fmt[8] = {};  // X87SlotFmt

    // OPT-G: Deferred FXCH — compile-time register renaming.
    // perm[i] maps logical stack depth i to physical depth offset.
    // Identity: perm[i] == i for all i.  FXCH ST(n) swaps perm[0] and perm[n].
    // Flushed at run end by emitting the minimal memory swaps (cycle decomposition).
    int8_t perm[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    int8_t perm_dirty = 0;  // 1 = permutation is non-identity

    // Run bridging: descriptor stashed at a fresh run start when
    // lookahead_bridged found a profitable region.  bridge_pending_idx is
    // the region's start instruction (the attempt fires only when the IR
    // dispatch reaches exactly that index with no deferred state);
    // bridge_pending_plain is the plain x87 run length to restore when the
    // all-or-nothing attempt falls back.
    int16_t bridge_pending_total = 0;
    int16_t bridge_pending_plain = 0;
    int16_t bridge_pending_idx = -1;

    IRBlock* prev_block = nullptr;

    // Block and instruction at which a request ran out of scratch registers.
    // Every later request in the block is declined too, until stock moves to
    // another block or starts this one over.  The flags do not require this:
    // stock and the sidecar keep the guest EFLAGS in the same NZCV layout
    // (TranslatorX87Helpers.hpp), so either can read what the other set.  It
    // is kept because a failed request is rare and the rule only costs speed
    // in that block.  Stock reuses IRBlock objects, so the block is also
    // identified by its length and the guest pc of its first instruction.
    const IRBlock* declined_block = nullptr;
    int32_t declined_idx = -1;
    int32_t declined_num_instrs = 0;
    uint32_t declined_start_pc = 0;

    // Stock's free_gpr_mask word as it arrived at the current x87 entry
    // (run start or isolated op).  Stock's translate_insn carries its
    // register-allocator state across a block in this TR field; every
    // exit path must write this captured word back (minus pinned_mask()
    // while a run is active) instead of a scratch-pool constant —
    // overwriting it with kGprScratchMask poisoned stock's allocator for
    // the rest of the block.  Defaults to the full pool so offline tools
    // that seed their TR with kGprScratchMask behave identically.
    uint32_t stock_free_gpr_mask = 0x3FC00000U;  // = kGprScratchMask

    // FNV-1a IR-content hash, stable across runs (PC zeroed before hashing).
    // Set on block transition; names the block in log lines.
    uint64_t profile_hash = 0;

    bool active() const;
    void invalidate();
    void set_run(int run_length);
    void tick();
    uint32_t pinned_mask() const;

    // OPT-G: permutation helpers
    void reset_perm();
    bool perm_is_identity() const;

    // Scan forward from insn_idx counting consecutive handled x87 instructions.
    static int lookahead(IRInstr* instr_array, int64_t num_instrs, int64_t insn_idx);

    // True when the run lookahead() would count from insn_idx is non-empty
    // and every op in it moves integers or the control word without
    // arithmetic: fild, fist, fistp, fisttp (any memory size), fldcw,
    // fnstcw and the no-ops fnop, fdisi, feni.  Such a reply can be emitted
    // against the native 80-bit slots exactly, with no binary64 conversion
    // of the register file on entry or exit.
    static bool is_native_transport_run(IRInstr* instr_array, int64_t num_instrs, int64_t insn_idx);

    // Run bridging: a region descriptor for one IR run that spans short gaps
    // of bridge instructions (X87Bridge.h) between x87 segments.
    //   total       instructions in the region (x87 + bridges); 0 = no
    //               profitable bridged region (fall back to plain dispatch)
    //   x87_count   x87 instructions within it
    //   bridges     bridge instructions within it (== total - x87_count)
    // The region always starts and ends on an x87 instruction (trailing
    // bridges are trimmed — they join nothing), contains at least one join,
    // and satisfies x87_count >= 3 so the merged run clears the IR gate.
    // A gap holds at most kBridgeMaxGap consecutive bridge instructions and
    // a region at most kBridgeMaxTotal.  Gap instructions may also be
    // flag-dead ALU (v2), accepted only when the block's flag_liveness field
    // is demonstrably populated (x87bridge::block_has_flag_liveness).
    static constexpr int kBridgeMaxGap = 2;
    static constexpr int kBridgeMaxTotal = 8;
    struct BridgedRun {
        int16_t total = 0;
        int16_t x87_count = 0;
        int16_t bridges = 0;
    };
    static BridgedRun lookahead_bridged(IRInstr* instr_array, int64_t num_instrs, int64_t insn_idx);

    // What one instruction does to the register stack, for the
    // Native slot tracker.  Depths are logical (before the
    // permutation of deferred FXCH) and taken before the instruction's own
    // pushes and pops.  A non-x87 instruction (a bridge) has no effect.
    enum EffectKind : uint8_t {
        kEffectPlain = 0,
        kEffectFxch,     // swaps logical depths 0 and arg
        kEffectFree,     // ffree: depth arg becomes empty
        kEffectTopInc,   // fincstp
        kEffectTopDec,   // fdecstp
        kEffectSave,     // fsave: reads all eight, then an empty stack at TOP 0
        kEffectRestore,  // frstor: all eight replaced, TOP from memory
    };
    struct SlotEffect {
        uint8_t reads = 0;   // depths read
        uint8_t soft = 0;    // of those, the ones the direct emitter reads
                             // exactly from an 80-bit slot (no conversion)
        uint8_t writes = 0;  // depths written without being read
        int8_t pushes = 0;   // the pushed value is written as binary64
        int8_t pops = 0;
        EffectKind kind = kEffectPlain;
        int8_t arg = 0;
    };
    static SlotEffect slot_effect(const IRInstr& insn);
};

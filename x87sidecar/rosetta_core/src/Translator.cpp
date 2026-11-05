#include "rosetta_core/Translator.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include "TranslatorX87Internal.hpp"
#include "rosetta_core/AssemblerHelpers.hpp"
#include "rosetta_core/IRInstr.h"
#include "rosetta_core/Opcode.h"
#include "rosetta_core/ProfileRuntime.h"
#include "rosetta_core/Register.h"
#include "rosetta_core/TranslationResult.h"
#include "rosetta_core/TranslatorHelpers.hpp"
#include "rosetta_core/TranslatorX87.h"
#include "rosetta_core/TranslatorX87F80.hpp"
#include "rosetta_core/TranslatorX87Fusion.h"
#include "rosetta_core/TranslatorX87Helpers.hpp"
#include "rosetta_core/X87Cache.h"
#include "rosetta_core/X87IR.h"

// True for opcode IDs in the two x87 ranges that Rosetta assigns to x87
// instructions.  Mirrors the JIT stub's FILTER prologue range check at
// rosetta_loader/src/stub_asm.cpp.  Used by the dispatch default case so
// we only complain about *x87* ops we forgot to handle.
static bool is_x87_opcode(uint16_t op) {
    return (op >= kOpcodeName_fcmovb && op <= kOpcodeName_fucomip) ||
           (op >= kOpcodeName_f2xm1 && op <= kOpcodeName_fyl2xp1);
}

// Real body: one x87 instruction (or one fusion / one IR run).  The public
// translate_instruction wraps it so the reply's frontier (last_next_idx) is
// recorded once, after every exit path, and so that a run is one reply.
static auto translate_instruction_impl(TranslationResult* translation_result, IRBlock* block,
                                       IRInstr* instr_array, int64_t num_instrs, int64_t insn_idx)
    -> std::optional<int64_t>;

// What a request can change in the result before it is known to succeed:
// the emitted words, the fixup lists, the register masks and the x87 cache.
// A request whose allocator ran dry is put back to this and declined.
struct RequestEntry {
    uint64_t buf_end;
    uint64_t fixup_count[6];
    uint32_t free_gpr_mask;
    uint32_t free_fpr_mask;
    uint32_t pinned_temporary_scalars;
    X87Cache cache;
};

static constexpr TransactionalList<Fixup> TranslationResult::* kFixupLists[6] = {
    &TranslationResult::external_fixups, &TranslationResult::internal_fixups,
    &TranslationResult::_fixups,         &TranslationResult::field_B0,
    &TranslationResult::dyld_stub_fixups, &TranslationResult::field_1A8,
};

static RequestEntry capture_request_entry(const TranslationResult& tr) {
    RequestEntry e{.buf_end = tr.insn_buf.end,
                   .fixup_count = {},
                   .free_gpr_mask = tr.free_gpr_mask,
                   .free_fpr_mask = tr.free_fpr_mask,
                   .pinned_temporary_scalars = tr._pinned_temporary_scalars,
                   .cache = tr.x87_cache};
    for (size_t i = 0; i < 6; i++) {
        e.fixup_count[i] = (tr.*kFixupLists[i]).element_count();
    }
    return e;
}

// Undo a request that ran out of scratch registers, so that stock, which
// translates the instruction instead, finds the result as the request found
// it.  This is sound at any point inside the request because a request is
// a whole run (see translate_instruction): it starts with the cache released
// and the x87 state complete in memory, and nothing it emitted is kept.  The
// cache goes back to its entry state with the run released, as after any
// declined request.  When the request had moved the cache onto a new block,
// the entry state is the previous block's, so the block is entered afresh
// the next time it is translated.  A request that found stock translating
// the block again is treated the same way, by forgetting the block.  The
// rest of the block goes to stock as well (X87Cache::declined_block).
static void decline_after_alloc_failure(TranslationResult& tr, const RequestEntry& entry,
                                        const IRBlock* block, const IRInstr* instr_array,
                                        int64_t num_instrs, int64_t insn_idx) {
    tr.insn_buf.end = entry.buf_end;
    for (size_t i = 0; i < 6; i++) {
        auto& list = tr.*kFixupLists[i];
        list.end = list.begin + entry.fixup_count[i];
    }
    tr.free_gpr_mask = entry.free_gpr_mask;
    tr.free_fpr_mask = entry.free_fpr_mask;
    tr._pinned_temporary_scalars = entry.pinned_temporary_scalars;

    auto& cache = tr.x87_cache;
    cache = entry.cache;
    cache.invalidate();
    cache.bridge_pending_total = 0;
    cache.bridge_pending_idx = -1;
    if (cache.prev_block == block && insn_idx <= cache.last_insn_idx) {
        cache.prev_block = nullptr;
    }
    const bool same_block = cache.prev_block == block;
    if (same_block) {
        cache.last_insn_idx = static_cast<int32_t>(insn_idx);
    }
    cache.last_next_idx = -1;
    cache.declined_block = block;
    cache.declined_idx = static_cast<int32_t>(insn_idx);
    cache.declined_num_instrs = static_cast<int32_t>(num_instrs);
    cache.declined_start_pc = instr_array[0].pc;
}

// Native slots: sets the native layout and the slot tracker for one
// reply and clears both on every way out.  The tracker starts with TOP where
// the reply finds it and every slot holding Rosetta's 80-bit value.
struct NativeLayoutScope {
    NativeLayoutScope(bool on, TranslationResult& tr) {
        g_x87_native_layout = on;
        g_x87_slot_tracker = on ? &tr : nullptr;
        if (on) {
            tr.x87_cache.top_rel = 0;
            for (auto& f : tr.x87_cache.slot_fmt) {
                f = kSlotNative;
            }
        }
    }
    ~NativeLayoutScope() {
        g_x87_native_layout = false;
        g_x87_slot_tracker = nullptr;
    }
    NativeLayoutScope(const NativeLayoutScope&) = delete;
    NativeLayoutScope& operator=(const NativeLayoutScope&) = delete;
};

// Native slots: what the next `count` instructions from idx need done to
// the slots before they are emitted, simulated from the tracker's current
// state with X87Cache::slot_effect.  Slot positions are relative to TOP at the
// start of the reply (X87Cache::slot_fmt).
//
//   convert         slots still holding an 80-bit value that an instruction
//                   reads as binary64 (a hard read); the dispatcher converts
//                   them in place first.
//   soft_f80        a soft read finds an 80-bit slot: fld st(i), fst and fstp
//                   to a register or to m32/m64, fist, fistp, fisttp, fxch and
//                   fsave read such a slot in their direct emitters without
//                   changing it (the integer stores exactly, or from a
//                   binary64 copy when short of registers), so the fusions
//                   and the single-op fast path, which read binary64, are
//                   skipped for this request.
//   push_read_hard  with first_push_native, the value the first instruction
//                   pushes would be read as binary64 later in the window,
//                   by arithmetic or by an fst or fstp to m32 or m64.
//
// A deferred FXCH permutes depths; flushing it (at every push, pop, fincstp,
// fdecstp and fsave, and at the end of a run when to_run_end) moves the
// slots as they are (emit_x87_perm_flush), so the formats move with them.
// The emitters convert nothing themselves.  Converting a slot the
// instruction ends up not reading, or an empty one, costs code but changes
// no result.
struct SlotPlan {
    uint8_t convert = 0;
    bool soft_f80 = false;
    bool push_read_hard = false;
};

static SlotPlan plan_slots(const X87Cache& c, const IRInstr* instrs, int64_t num, int64_t idx,
                           int count, bool to_run_end, bool first_push_native = false) {
    SlotPlan plan;
    int top = c.top_rel;
    int8_t perm[8];
    uint8_t fmt[8];
    for (int i = 0; i < 8; i++) {
        perm[i] = c.perm[i];
        fmt[i] = c.slot_fmt[i];
    }
    int pushed = -1;  // the slot first_push_native follows
    // to_f64: the read converts the value to binary64 even though it leaves
    // the slot as it is (fst and fstp to m32 or m64).
    const auto read = [&](int phys, bool soft, bool to_f64) {
        const int r = (top + phys) & 7;
        if (fmt[r] != kSlotNative) {
            return;
        }
        if (r == pushed && (!soft || to_f64)) {
            plan.push_read_hard = true;
        }
        if (soft) {
            plan.soft_f80 = true;
            return;
        }
        plan.convert = static_cast<uint8_t>(plan.convert | (1U << r));
        fmt[r] = kSlotF64;
    };
    const auto flush = [&] {
        uint8_t moved[8];
        for (int i = 0; i < 8; i++) {
            moved[(top + i) & 7] = fmt[(top + perm[i]) & 7];
        }
        if (pushed >= 0) {
            for (int i = 0; i < 8; i++) {
                if (((top + perm[i]) & 7) == pushed) {
                    pushed = (top + i) & 7;
                    break;
                }
            }
        }
        for (int i = 0; i < 8; i++) {
            fmt[i] = moved[i];
            perm[i] = static_cast<int8_t>(i);
        }
    };
    for (int k = 0; k < count && idx + k < num; k++) {
        const IRInstr& insn = instrs[idx + k];
        const auto e = X87Cache::slot_effect(insn);
        const uint8_t soft = e.soft;
        const uint16_t op = insn.opcode();
        const bool to_f64 = (op == kOpcodeName_fst || op == kOpcodeName_fstp) &&
                            insn.num_operands > 0 &&
                            insn.operands[0].kind != IROperandKind::Register &&
                            (insn.operands[0].mem.size == IROperandSize::S32 ||
                             insn.operands[0].mem.size == IROperandSize::S64);
        for (int d = 0; d < 8; d++) {
            if ((e.reads >> d) & 1U) {
                read(perm[d], ((soft >> d) & 1U) != 0, to_f64);
            }
            if ((e.writes >> d) & 1U) {
                fmt[(top + perm[d]) & 7] = kSlotF64;
            }
        }
        switch (e.kind) {
            case X87Cache::kEffectFxch:
                std::swap(perm[0], perm[e.arg]);
                break;
            case X87Cache::kEffectFree: {
                auto& f = fmt[(top + perm[e.arg]) & 7];
                f = x87_slot_killed(f);
                break;
            }
            case X87Cache::kEffectTopInc:
            case X87Cache::kEffectTopDec:
                flush();
                top = (top + (e.kind == X87Cache::kEffectTopInc ? 1 : -1)) & 7;
                break;
            case X87Cache::kEffectSave:
            case X87Cache::kEffectRestore:
                for (int i = 0; i < 8; i++) {
                    perm[i] = static_cast<int8_t>(i);
                    fmt[i] = e.kind == X87Cache::kEffectSave ? kSlotDead : kSlotNative;
                }
                top = 0;
                pushed = -1;
                break;
            case X87Cache::kEffectPlain:
                break;
        }
        if (e.pushes != 0 || e.pops != 0) {
            flush();
        }
        for (int p = 0; p < e.pops; p++) {
            fmt[top] = x87_slot_killed(fmt[top]);
            if (top == pushed) {
                pushed = -1;
            }
            top = (top + 1) & 7;
        }
        for (int p = 0; p < e.pushes; p++) {
            top = (top - 1) & 7;
            fmt[top] = kSlotF64;
            if (k == 0 && first_push_native) {
                fmt[top] = kSlotNative;
                pushed = top;
            }
        }
    }
    if (to_run_end) {
        flush();
    }
    return plan;
}

// Converts each slot in `mask` (positions as in plan_slots) from its
// 80-bit value to binary64, in place, and marks it binary64 in the tracker.
// Uses the run cache's base, TOP and Xst_base when it holds them, otherwise
// loads base and TOP into registers of its own; either way it leaves the
// allocator as it found it.  Four scratch GPRs on top of those, so a run
// leaves one register of the eight-register pool to stock; the slot address
// is computed again after the conversion, which needs all four.  No NZCV.
static void emit_slot_conversions(TranslationResult& tr, uint8_t mask) {
    if (mask == 0) {
        return;
    }
    auto& c = tr.x87_cache;
    AssemblerBuffer& buf = tr.insn_buf;
    int Xbase = c.base_gpr;
    int Wtop = c.top_gpr;
    int Xst = (c.gprs_valid && c.st_base_valid) ? c.st_base_gpr : -1;
    const bool own = c.gprs_valid == 0;
    if (own) {
        Xbase = alloc_free_gpr(tr);
        Wtop = alloc_free_gpr(tr);
        emit_x87_base(buf, tr, Xbase);
        emit_load_top(buf, tr, Xbase, Wtop);
        Xst = -1;
    }
    const int Xm = alloc_free_gpr(tr);
    const int Wx = alloc_free_gpr(tr);
    const int Xt1 = alloc_free_gpr(tr);
    const int Xt2 = alloc_free_gpr(tr);
    const int Xfrom = Xst >= 0 ? Xst : Xbase;
    const int16_t off = Xst >= 0 ? 0 : kX87NativeSlotOff;
    for (int r = 0; r < 8; r++) {
        if (((mask >> r) & 1U) == 0) {
            continue;
        }
        const int depth = (r - c.top_rel) & 7;
        emit_native_slot_key(buf, Wtop, depth, Xt1);
        emit_add_sub_shifted_reg(buf, 1, 0, 0, 0, Xt1, 0, Xfrom, Xt1);
        emit_ldur_stur(buf, 3, /*LDUR*/ 1, off, Xt1, Xm);
        emit_ldr_str_imm(buf, 1, 0, /*LDRH*/ 1, static_cast<int16_t>((off + 8) / 2), Xt1, Wx);
        TranslatorX87::emit_f80_bits_to_f64(buf, Xm, Wx, Xt1, Xt2);
        emit_native_slot_key(buf, Wtop, depth, Xt1);
        emit_add_sub_shifted_reg(buf, 1, 0, 0, 0, Xt1, 0, Xfrom, Xt1);
        emit_ldur_stur(buf, 3, /*STUR*/ 0, off, Xt1, Xm);
        c.slot_fmt[r] = kSlotF64;
    }
    free_gpr(tr, Xt2);
    free_gpr(tr, Xt1);
    free_gpr(tr, Wx);
    free_gpr(tr, Xm);
    if (own) {
        free_gpr(tr, Wtop);
        free_gpr(tr, Xbase);
    }
}

// Flush every deferred piece of x87 state to memory and release the run.
// Only for the path that must not happen: an instruction inside a run the
// lookahead counted as handled came back declined.  The ops already emitted
// stay, stock takes over at the declined one, and it must see a complete
// X87State when it does.
static void flush_run_and_release(TranslationResult& tr) {
    auto& cache = tr.x87_cache;
    AssemblerBuffer& buf = tr.insn_buf;
    cache.run_remaining = 1;
    auto [Xbase, Wd_top] = TranslatorX87::x87_begin(tr, buf);
    const int Wd_tmp = alloc_gpr(tr, 2);
    TranslatorX87::x87_end(tr, buf, Xbase, Wd_top, Wd_tmp, /*consumed=*/1);
    free_gpr(tr, Wd_tmp);
    cache.tick();
    tr.free_gpr_mask = cache.stock_free_gpr_mask;
}

auto Translator::translate_instruction(TranslationResult* translation_result, IRBlock* block,
                                       IRInstr* instr_array, int64_t num_instrs, int64_t insn_idx)
    -> std::optional<int64_t> {
    auto& cache = translation_result->x87_cache;
    const auto start = translation_result->insn_buf.end;
    const auto op = instr_array[insn_idx].opcode();
    translation_result->alloc_failure = nullptr;
    // The rest of a block in which a request ran out of registers goes to
    // stock (X87Cache::declined_block).
    if (cache.declined_block != nullptr) {
        if (cache.declined_block != block || insn_idx <= cache.declined_idx ||
            num_instrs != cache.declined_num_instrs ||
            instr_array[0].pc != cache.declined_start_pc) {
            cache.declined_block = nullptr;
        } else {
            cache.last_next_idx = -1;
            return std::nullopt;
        }
    }
    // Metadata-only ops and FXSAVE/FXRSTOR go to stock, which consumes
    // native state directly. Every handled x87 reply uses compact state
    // internally and must export native f80 after its cache is flushed.
    const bool native_boundary = is_x87_opcode(op) && op != kOpcodeName_fclex &&
                                 op != kOpcodeName_finit && op != kOpcodeName_fldenv &&
                                 op != kOpcodeName_fstenv && op != kOpcodeName_fxsave &&
                                 op != kOpcodeName_fxrstor;
    // A reply made only of integer loads and stores, control word moves and
    // no-ops does no arithmetic, so it can work on the native f80 slots
    // directly and needs no conversion at either end.  That keeps an int64
    // held on the x87 stack across replies exact, which the binary64 round
    // trip of the boundary cannot (Delphi's System.Move holds fild values
    // across its branches).  The run lookahead decides it, so the predicate
    // covers every op the reply below can consume: bridging is off for such
    // a run and an IR run never extends past it.
    const bool transport =
        native_boundary && X87Cache::is_native_transport_run(instr_array, num_instrs, insn_idx);
    // Every other x87 reply keeps its working binary64 values inside the
    // native slots (+6, stride 10).  A native transport reply needs no
    // binary64 values at all and keeps its own emitters.
    const bool slots = native_boundary && !transport;
    // Captured before native_transport and native_slots are set, so a
    // declined request leaves both clear, as every request that ends does.
    const RequestEntry entry = capture_request_entry(*translation_result);
    cache.native_transport = transport ? 1 : 0;
    cache.native_slots = slots ? 1 : 0;
    const NativeLayoutScope layout(slots, *translation_result);
    // Nothing is converted on entry: each slot is converted where the reply
    // first reads it (emit_slot_conversions).
    auto ret = translation_result->alloc_failure != nullptr
                   ? std::nullopt
                   : translate_instruction_impl(translation_result, block, instr_array,
                                                num_instrs, insn_idx);

    // A run is one reply.  Stock records one instruction-map entry per
    // translate_insn reply, and when an asynchronous signal lands in a
    // translation Rosetta's runtime steps the ARM code forward to the next
    // entry, takes the guest state from the thread context there, and resumes
    // from that state after the handler.  Inside a run the architectural TOP
    // lives in a register, tag updates are pending and FXCH is a compile-time
    // permutation, none of which is in memory at an intermediate entry, so
    // each such entry was a point where a signal silently shifted the x87
    // stack (issue #23).  With the whole run in one reply the only entries
    // are the run's start and its end, where x87_end has flushed everything.
    while (ret.has_value() && cache.active() && *ret < num_instrs &&
           translation_result->alloc_failure == nullptr) {
        cache.last_next_idx = static_cast<int32_t>(*ret);
        const auto more =
            translate_instruction_impl(translation_result, block, instr_array, num_instrs, *ret);
        if (!more.has_value() && translation_result->alloc_failure != nullptr) {
            break;
        }
        if (!more.has_value()) {
            static int declined_count = 0;
            if (++declined_count <= 10) {
                std::fprintf(stdout,
                             "[rosettax87] x87 run: instruction %lld of block hash=0x%016llx "
                             "declined mid-run, flushing (#%d)\n",
                             static_cast<long long>(*ret),
                             static_cast<unsigned long long>(cache.profile_hash), declined_count);
                std::fflush(stdout);
            }
            flush_run_and_release(*translation_result);
            break;
        }
        ret = more;
    }
    cache.native_transport = 0;
    cache.native_slots = 0;
    if (native_boundary && translation_result->alloc_failure == nullptr) {
        if (ret) {
            if (slots) {
                TranslatorX87::emit_native_slots_export(*translation_result);
            }
        } else {
            // Declined: drop what the reply emitted, including any fixup
            // an address computation recorded.
            translation_result->insn_buf.end = start;
            for (size_t i = 0; i < 6; i++) {
                auto& list = translation_result->*kFixupLists[i];
                list.end = list.begin + entry.fixup_count[i];
            }
        }
    }
    if (translation_result->alloc_failure != nullptr) {
        decline_after_alloc_failure(*translation_result, entry, block, instr_array, num_instrs,
                                    insn_idx);
        return std::nullopt;
    }
    cache.last_next_idx = ret.has_value() ? static_cast<int32_t>(*ret) : -1;
    return ret;
}

static auto translate_instruction_impl(TranslationResult* translation_result, IRBlock* block,
                                       IRInstr* instr_array, int64_t num_instrs, int64_t insn_idx)
    -> std::optional<int64_t> {
    auto* const cur_instr = &instr_array[insn_idx];
    const auto opcode = cur_instr->opcode();
    auto& cache = translation_result->x87_cache;

    // ── OPT-1: x87 cross-instruction cache management ───────────────────────
    // Invalidate the cache if we've moved to a different block (between blocks,
    // branches may have executed non-x87 code that clobbers scratch GPRs).
    // Then, if no cache is active, scan ahead to find the length of the current
    // consecutive x87 run.  x87_cache_set_run only activates if run >= 2.
    {
        // Same block, but not a continuation: stock started the block's
        // translation over.  Either it asks at or before an index we already
        // handled, or a run is active and it asks anywhere but the frontier
        // the previous reply set (a re-ask inside a span a fusion or IR run
        // consumed).  In both cases the emitted code that holds base/TOP in
        // registers belongs to a pass stock discarded, so the cache is reset
        // as on a block change and this op is emitted from memory state.
        // Neither has been observed in a real run so far; the line below is
        // there so a run that does hit one says so.
        const bool midspan_reask = block == cache.prev_block && cache.active() &&
                                   cache.last_next_idx >= 0 &&
                                   insn_idx != static_cast<int64_t>(cache.last_next_idx);
        const bool restart_same_block =
            (block == cache.prev_block && insn_idx <= cache.last_insn_idx) || midspan_reask;
        if (restart_same_block) {
            static int restart_count = 0;
            ++restart_count;
            if (restart_count <= 10 || restart_count % 1000 == 0) {
                std::fprintf(stdout,
                             "[rosettax87] x87 cache: block hash=0x%016llx translated again "
                             "(ask=%lld last_in=%d frontier=%d run=%d), resetting cache (#%d)\n",
                             static_cast<unsigned long long>(cache.profile_hash),
                             static_cast<long long>(insn_idx), cache.last_insn_idx,
                             cache.last_next_idx, static_cast<int>(cache.run_remaining),
                             restart_count);
                std::fflush(stdout);
            }
        }
        if (block != cache.prev_block || restart_same_block) {
            cache.invalidate();
            cache.prev_block = block;
            // Stock carries its register-allocator state across the block
            // in free_gpr_mask (TR+0x210) — capture the incoming word (the
            // exit paths below write it back) and restrict our working
            // mask to the scratch pool.  Never overwrite the field with
            // kGprScratchMask: stock would then reuse regs it still holds
            // live (miscompiles), and our emits would clobber stock-live
            // values at runtime.
            cache.stock_free_gpr_mask = translation_result->free_gpr_mask;
            translation_result->free_gpr_mask &= kGprScratchMask;
            cache.last_insn_idx = -1;
            cache.last_next_idx = -1;
            // The hash names the block in log lines.  It is FNV-1a over the
            // IR stream with each instr's `pc` zeroed, so it identifies the
            // block by content and is stable across launches.
            cache.profile_hash =
                profile::hash_ir_stream(instr_array, static_cast<size_t>(num_instrs));
        } else {
            // Same block, new x87 entry.  Off-run the incoming word is
            // stock's current allocator state (possibly advanced by stock
            // since our last writeback) — recapture it.  Mid-run it is our
            // own previous writeback (captured & ~pins); keep the earlier
            // capture, which still holds the pinned bits.  Either way the
            // working mask is pool-restricted.
            if (!cache.active()) {
                cache.stock_free_gpr_mask = translation_result->free_gpr_mask;
            }
            translation_result->free_gpr_mask &= kGprScratchMask;
        }
        cache.last_insn_idx = static_cast<int32_t>(insn_idx);
        if (!cache.active()) {
            const int run = X87Cache::lookahead(instr_array, num_instrs, insn_idx);
            // A run of only control-word moves skips the TOP load
            // (X87Cache::top_unused).
            bool only_cw = true;
            for (int k = 0; k < run && only_cw; k++) {
                const auto o = instr_array[insn_idx + k].opcode();
                only_cw = o == kOpcodeName_fnstcw || o == kOpcodeName_fldcw;
            }
            cache.top_unused = only_cw ? 1 : 0;
            cache.set_run(run);
            // Run bridging: at a fresh run start, check whether short gaps
            // join this run to following x87 segments.  Stash the
            // descriptor; the IR dispatch below fires the all-or-nothing
            // attempt when it reaches this index with no deferred state.
            cache.bridge_pending_total = 0;
            cache.bridge_pending_idx = -1;
            // A native transport run is not bridged: the bridged region
            // would be lowered by the IR into the compact layout.
            if (run >= 1 && !cache.native_transport) {
                const auto br = X87Cache::lookahead_bridged(instr_array, num_instrs, insn_idx);
                if (br.total > run) {
                    cache.bridge_pending_total = br.total;
                    cache.bridge_pending_plain = static_cast<int16_t>(run);
                    cache.bridge_pending_idx = static_cast<int16_t>(insn_idx);
                }
            }
        }
    }

    // ── IR pipeline: try whole-run optimization for runs of 3+ ─────────────
    // The IR fires once at the start of a fresh run (no deferred cache state).
    // It builds an SSA-like IR, runs optimization passes (DSE, FMA, FCOM+FSTSW
    // fusion), and lowers directly to AArch64.
    // ── Run bridging: all-or-nothing bridged IR attempt ─────────────────────
    // Fires exactly once per stashed descriptor, at the region's start
    // instruction, and only with no deferred cache state (a fresh run start
    // has none; the guard is belt-and-braces).  On success the whole region
    // — x87 segments plus the mov/lea bridges joining them — lowers as ONE
    // IR run, so the FP stack never round-trips through memory at the gaps.
    // On any failure nothing was emitted (build/gate failures precede
    // lower()); restore the plain run and fall through to the normal paths.
    {
        if (cache.bridge_pending_total > 0 &&
            cache.bridge_pending_idx == static_cast<int16_t>(insn_idx) && cache.top_dirty == 0 &&
            cache.tag_push_pending == 0 && cache.deferred_pop_count == 0 && cache.perm_dirty == 0) {
            const int total = cache.bridge_pending_total;
            const int plain_run = cache.bridge_pending_plain;
            cache.bridge_pending_total = 0;
            cache.bridge_pending_idx = -1;

            cache.set_run(total);  // pin GPRs across the whole region
            // No conversion is needed here: the IR converts the slots it
            // reads into registers (X87IRLower ReadSt), and with no deferred
            // FXCH there is no permutation to flush.
            const int consumed =
                X87IR::compile_run(translation_result, instr_array, num_instrs, insn_idx, total,
                                   /*bridged=*/true);
            if (consumed == total) {
                for (int i = 0; i < total; i++) {
                    cache.tick();
                }
                if (cache.active()) {
                    translation_result->free_gpr_mask =
                        cache.stock_free_gpr_mask & ~cache.pinned_mask();
                } else {
                    translation_result->free_gpr_mask = cache.stock_free_gpr_mask;
                }
                translation_result->free_fpr_mask =
                    translation_result->_unoccupied_temporary_fprs_for_xmm_scalars;
                translation_result->_pinned_temporary_scalars = 0;
                return insn_idx + total;
            }
            // Fallback: nothing emitted; restore the plain x87 run.  Direct
            // field write: set_run() ignores lengths < 2, which would leave
            // run_remaining at the bridged total and keep the cache active
            // across the gap's stock-translated instructions.
            cache.run_remaining = static_cast<int16_t>(plain_run >= 2 ? plain_run : 0);
        }
    }

    {
        // The IR lowers into the compact layout, which a native transport
        // reply does not have.
        if (cache.native_transport == 0 && cache.active()) {
            bool gate_refused = false;
            // Speculative-flush rollback state.  The gate-refusal cascade
            // may emit a small flush (3-14 ARM) and clear a deferred cache
            // field, then fall through to compile_run.  If compile_run
            // subsequently bails on kFprPressure / kGprPressure, the
            // flush ARM is wasted — the next dispatch path (peephole /
            // single-op) handles the deferred state itself.  Rollback
            // rewinds insn_buf.end and restores the cleared cache field.
            //
            // There is no standalone tag_push_pending refusal arm:
            // lower()'s prologue emits the tag clear when it runs, and
            // compile_run's FPR/GPR pressure check bails before lower(), so
            // cache.tag_push_pending is preserved on bail.  The cascade
            // absorbs the common (top_dirty, tag_push) co-set case at the
            // top_dirty arm; the rare tag_push-only case falls through with
            // no gate-side flush.
            const uint64_t saved_buf_end = translation_result->insn_buf.end;
            const int8_t saved_perm_dirty = cache.perm_dirty;
            int8_t saved_perm[8];
            for (int i = 0; i < 8; i++) {
                saved_perm[i] = cache.perm[i];
            }
            const int8_t saved_top_dirty = cache.top_dirty;
            const int8_t saved_deferred_pop_count = cache.deferred_pop_count;
            // The perm_dirty flush below moves slot formats with the slots;
            // a rewind takes them back with it.
            uint8_t saved_slot_fmt[8];
            std::copy(std::begin(cache.slot_fmt), std::end(cache.slot_fmt), saved_slot_fmt);
            // Which branch flushed, so the rollback knows what to restore.
            enum RollbackBranch : uint8_t {
                kRollbackNone = 0,
                kRollbackTopDirty,
                kRollbackDeferredPop,
                kRollbackPermDirty,
            } flushed_branch = kRollbackNone;
            // Every branch below runs with run_remaining >= 3.
            if (cache.run_remaining < 3) {
                gate_refused = true;
            } else if (cache.top_dirty != 0) {
                // FLUSH top_dirty only (3 ARM).  tag_push_pending is
                // commonly set alongside (x87_push set both); we leave it
                // untouched here.  If compile_run consumes the run and
                // lower() runs, lower's prologue emits the tag clear.  If
                // compile_run bails before lower (FPR / GPR pressure),
                // cache.tag_push_pending is still set; the next call hits
                // this branch again or the deferred_pop / perm_dirty arms.
                if (cache.gprs_valid) {
                    AssemblerBuffer& buf = translation_result->insn_buf;
                    const int Wd_tmp = alloc_free_gpr(*translation_result);
                    emit_store_top(buf, cache.base_gpr, cache.top_gpr, Wd_tmp);
                    free_gpr(*translation_result, Wd_tmp);
                    cache.top_dirty = 0;
                    flushed_branch = kRollbackTopDirty;
                    // gate_refused stays false — fall through to compile_run.
                } else {
                    gate_refused = true;
                }
            } else if (cache.deferred_pop_count != 0) {
                // FLUSH deferred_pop_count alone (2 + 6*count ARM).  Reaches
                // here when top_dirty=0 (the top_dirty branch fired earlier
                // in the run + IR partial-consumed leaving deferred_pop_count
                // behind).  Cap on count<=2 keeps the flush cost under
                // ~14 ARM; count>=3 is rare.
                if (cache.gprs_valid && cache.deferred_pop_count <= 2) {
                    AssemblerBuffer& buf = translation_result->insn_buf;
                    const int Wd_tmp = alloc_free_gpr(*translation_result);
                    const int Wd_tmp2 = alloc_free_gpr(*translation_result);
                    const int Wd_tagw = alloc_free_gpr(*translation_result);
                    emit_x87_tag_set_empty_batch(buf, cache.base_gpr, cache.top_gpr, Wd_tmp,
                                                 Wd_tmp2, Wd_tagw, cache.deferred_pop_count);
                    free_gpr(*translation_result, Wd_tagw);
                    free_gpr(*translation_result, Wd_tmp2);
                    free_gpr(*translation_result, Wd_tmp);
                    cache.deferred_pop_count = 0;
                    flushed_branch = kRollbackDeferredPop;
                } else {
                    gate_refused = true;
                }
            } else if (cache.perm_dirty) {
                // FLUSH perm_dirty alone (~6 ARM for the typical 2-cycle
                // FXCH-ST(1) case).  By design invariant (every push/pop
                // path calls perm_flush_before_stack_change first),
                // perm_dirty=1 implies all other deferred flags=0, so this
                // branch always fires alone in the cascade.
                if (cache.gprs_valid) {
                    AssemblerBuffer& buf = translation_result->insn_buf;
                    const int Wd_tmp = alloc_free_gpr(*translation_result);
                    TranslatorX87::x87_emit_perm_flush(buf, *translation_result, cache.base_gpr,
                                                       cache.top_gpr, Wd_tmp);
                    free_gpr(*translation_result, Wd_tmp);
                    flushed_branch = kRollbackPermDirty;
                } else {
                    gate_refused = true;
                }
            }
            if (!gate_refused) {
                // No conversion is needed before the IR: ReadSt converts an
                // 80-bit slot into its register, and lower()'s flush of a
                // leftover FXCH permutation moves slots as they are.
                const int ir_consumed = X87IR::compile_run(
                    translation_result, instr_array, num_instrs, insn_idx, cache.run_remaining);
                if (ir_consumed == 0 && flushed_branch != kRollbackNone) {
                    // Speculative-flush rollback: the gate-cascade flush
                    // emitted ARM and compile_run bailed, so the flush ARM is
                    // wasted — peephole / single-op handles the deferred
                    // state itself.  Rewind insn_buf.end and restore the
                    // cleared cache field to its pre-flush state.
                    translation_result->insn_buf.end = saved_buf_end;
                    std::copy(std::begin(saved_slot_fmt), std::end(saved_slot_fmt),
                              std::begin(cache.slot_fmt));
                    if (flushed_branch == kRollbackPermDirty) {
                        cache.perm_dirty = saved_perm_dirty;
                        for (int i = 0; i < 8; i++) {
                            cache.perm[i] = saved_perm[i];
                        }
                    }
                    if (flushed_branch == kRollbackTopDirty) {
                        cache.top_dirty = saved_top_dirty;
                    }
                    if (flushed_branch == kRollbackDeferredPop) {
                        cache.deferred_pop_count = saved_deferred_pop_count;
                    }
                }
                if (ir_consumed > 0) {
                    for (int i = 0; i < ir_consumed; i++) {
                        cache.tick();
                    }
                    if (cache.active()) {
                        translation_result->free_gpr_mask =
                            cache.stock_free_gpr_mask & ~cache.pinned_mask();
                    } else {
                        translation_result->free_gpr_mask = cache.stock_free_gpr_mask;
                    }
                    translation_result->free_fpr_mask =
                        translation_result->_unoccupied_temporary_fprs_for_xmm_scalars;
                    translation_result->_pinned_temporary_scalars = 0;
                    return insn_idx + ir_consumed;
                }
            }
        }
    }

    // ── Native slots: convert what the next emitter reads ───────────────────
    // A fusion consumes up to six instructions and finishes the run's
    // deferred FXCH when it reaches the run's end, so the window covers that
    // much.
    bool soft_f80 = false;
    bool exact_fild = false;
    bool int_store_soft = false;
    if (cache.native_slots) {
        const int run = cache.active() ? cache.run_remaining
                                       : X87Cache::lookahead(instr_array, num_instrs, insn_idx);
        const int window = std::clamp(run, 1, 6);
        const bool to_run_end = run <= 6;
        // emit_f80_slot_to_int takes five scratch GPRs besides what
        // x87_begin needs; with fewer the integer store takes its usual
        // emitter, which converts ST(0) into a register and stores from that
        // binary64 value, leaving the slot as it is.
        const int free_gprs = __builtin_popcount(translation_result->free_gpr_mask);
        int_store_soft = free_gprs >= TranslatorX87::x87_begin_gpr_need(*translation_result) + 5;
        const SlotPlan plan = plan_slots(cache, instr_array, num_instrs, insn_idx, window,
                                         to_run_end);
        emit_slot_conversions(*translation_result, plan.convert);
        soft_f80 = plan.soft_f80;
        // fild m64 writes the exact 80-bit value when nothing later in the
        // run reads it as binary64; then a value of 2^53 or more held into a
        // later reply, or stored by fistp m64 or fstp m80, keeps all its
        // bits.  When something does read it as binary64, arithmetic or an
        // fst or fstp to m32 or m64, SCVTF is cheaper than the exact load
        // and a conversion later.
        if (opcode == Opcode::kOpcodeName_fild &&
            cur_instr->operands[0].kind != IROperandKind::Register &&
            cur_instr->operands[0].mem.size == IROperandSize::S64) {
            exact_fild = !plan_slots(cache, instr_array, num_instrs, insn_idx, std::max(run, 1),
                                     /*to_run_end=*/true,
                                     /*first_push_native=*/true)
                              .push_read_hard;
        }
    }

    // ── Peephole: try 2-instruction fusion patterns ─────────────────────────
    // A native transport reply keeps only the fild m64 + fistp m64 copies,
    // which touch no slot; every other fusion works on compact slots.  So
    // does a request whose window reads an 80-bit slot exactly (soft_f80) or
    // starts with an exact fild: the fusions read binary64.
    uint64_t fusions_mask = 0;
    if (cache.native_transport || soft_f80 || exact_fild) {
        fusions_mask = ~(1ULL << static_cast<int>(FusionId::fild_fistp));
    }
    const auto fused = TranslatorX87::try_peephole(translation_result, instr_array, num_instrs,
                                                   insn_idx, fusions_mask);

    // ── Single-op fast path: fused emitters for isolated fld/fst/fstp ──────
    // Only when the cache is inactive (run==1): no pinned
    // GPRs and no deferred TOP/tag/perm state can exist then — x87_end flushes
    // everything at run end.  Its fst and fstp read an 80-bit
    // ST(0) without changing it, so a soft read (soft_f80) does not keep
    // them off this path.
    bool single_fast = false;
    if (!fused && !cache.active() && !cache.native_transport) {
        single_fast = TranslatorX87::try_translate_single_fast(translation_result, cur_instr);
    }

    // Native transport: the integer loads and stores work on the native
    // slots.  fldcw, fnstcw and the no-ops touch no slot and take their
    // usual emitters below.  In a native slots reply the same emitters take
    // an exact fild m64 and an integer store of an 80-bit ST(0).
    bool native = false;
    if (!fused && cache.native_slots) {
        const bool st0_f80 =
            int_store_soft &&
            x87_slot_holds_f80_at(TranslatorX87::resolve_depth(*translation_result, 0));
        native = true;
        switch (opcode) {
            case Opcode::kOpcodeName_fild:
                native = exact_fild;
                break;
            case Opcode::kOpcodeName_fist:
            case Opcode::kOpcodeName_fistp:
            case Opcode::kOpcodeName_fisttp:
                native = st0_f80;
                break;
            default:
                native = false;
                break;
        }
        if (native) {
            switch (opcode) {
                case Opcode::kOpcodeName_fild:
                    TranslatorX87::translate_fild_native(translation_result, cur_instr);
                    break;
                case Opcode::kOpcodeName_fist:
                    TranslatorX87::translate_fist_native(translation_result, cur_instr);
                    break;
                case Opcode::kOpcodeName_fistp:
                    TranslatorX87::translate_fistp_native(translation_result, cur_instr);
                    break;
                default:
                    TranslatorX87::translate_fisttp_native(translation_result, cur_instr);
                    break;
            }
        }
    }
    if (!fused && cache.native_transport) {
        native = true;
        switch (opcode) {
            case Opcode::kOpcodeName_fild:
                TranslatorX87::translate_fild_native(translation_result, cur_instr);
                break;
            case Opcode::kOpcodeName_fist:
                TranslatorX87::translate_fist_native(translation_result, cur_instr);
                break;
            case Opcode::kOpcodeName_fistp:
                TranslatorX87::translate_fistp_native(translation_result, cur_instr);
                break;
            case Opcode::kOpcodeName_fisttp:
                TranslatorX87::translate_fisttp_native(translation_result, cur_instr);
                break;
            default:
                native = false;
                break;
        }
    }

    if (!fused && !single_fast && !native) {
        switch (opcode) {
            case Opcode::kOpcodeName_fldz:
                TranslatorX87::translate_fldz(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fld1:
                TranslatorX87::translate_fld1(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fldl2e:
                TranslatorX87::translate_fldl2e(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fldl2t:
                TranslatorX87::translate_fldl2t(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fldlg2:
                TranslatorX87::translate_fldlg2(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fldln2:
                TranslatorX87::translate_fldln2(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fldpi:
                TranslatorX87::translate_fldpi(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fld:
                TranslatorX87::translate_fld(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fild:
                TranslatorX87::translate_fild(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fadd:
                TranslatorX87::translate_fadd(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_faddp:
                TranslatorX87::translate_faddp(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fiadd:
                TranslatorX87::translate_fiadd(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fsub:
            case Opcode::kOpcodeName_fsubr:
                TranslatorX87::translate_fsub(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fsubp:
            case Opcode::kOpcodeName_fsubrp:
                TranslatorX87::translate_fsubp(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fdiv:
            case Opcode::kOpcodeName_fdivr:
                TranslatorX87::translate_fdiv(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fdivp:
            case Opcode::kOpcodeName_fdivrp:
                TranslatorX87::translate_fdivp(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fmul:
            case Opcode::kOpcodeName_fmulp:
                TranslatorX87::translate_fmul(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fst:
            case Opcode::kOpcodeName_fst_stack:
            case Opcode::kOpcodeName_fstp:
            case Opcode::kOpcodeName_fstp_stack:
                TranslatorX87::translate_fst(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fstsw:
                TranslatorX87::translate_fstsw(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fxam:
                TranslatorX87::translate_fxam(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fdecstp:
                TranslatorX87::translate_fdecstp(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fincstp:
                TranslatorX87::translate_fincstp(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_ffree:
                TranslatorX87::translate_ffree(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fxtract:
                TranslatorX87::translate_fxtract(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fscale:
                TranslatorX87::translate_fscale(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_frstor:
                TranslatorX87::translate_frstor(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fsave:
                TranslatorX87::translate_fsave(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fcom:
            case Opcode::kOpcodeName_fcomp:
            case Opcode::kOpcodeName_fcompp:
            case Opcode::kOpcodeName_fucom:
            case Opcode::kOpcodeName_fucomp:
            case Opcode::kOpcodeName_fucompp:
                TranslatorX87::translate_fcom(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fxch:
                TranslatorX87::translate_fxch(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fchs:
                TranslatorX87::translate_fchs(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fabs:
                TranslatorX87::translate_fabs(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fsqrt:
                TranslatorX87::translate_fsqrt(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fistp:
                TranslatorX87::translate_fistp(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fisttp:
                TranslatorX87::translate_fisttp(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fidiv:
                TranslatorX87::translate_fidiv(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fimul:
                TranslatorX87::translate_fimul(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fisub:
                TranslatorX87::translate_fisub(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fidivr:
                TranslatorX87::translate_fidivr(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_frndint:
                TranslatorX87::translate_frndint(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fcomi:
            case Opcode::kOpcodeName_fcomip:
            case Opcode::kOpcodeName_fucomi:
            case Opcode::kOpcodeName_fucomip:
                TranslatorX87::translate_fcomi(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_ftst:
                TranslatorX87::translate_ftst(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fist:
                TranslatorX87::translate_fist(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fisubr:
                TranslatorX87::translate_fisubr(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_ficom:
            case Opcode::kOpcodeName_ficomp:
                TranslatorX87::translate_ficom(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fcmovb:
            case Opcode::kOpcodeName_fcmovbe:
            case Opcode::kOpcodeName_fcmove:
            case Opcode::kOpcodeName_fcmovnb:
            case Opcode::kOpcodeName_fcmovnbe:
            case Opcode::kOpcodeName_fcmovne:
            case Opcode::kOpcodeName_fcmovu:
            case Opcode::kOpcodeName_fcmovnu:
                TranslatorX87::translate_fcmov(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fldcw:
                TranslatorX87::translate_fldcw(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fnstcw:
                TranslatorX87::translate_fnstcw(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fnop:
            case Opcode::kOpcodeName_fdisi:  // 8087 FPU-int disable; NOP on 80287+
            case Opcode::kOpcodeName_feni:   // 8087 FPU-int enable; NOP on 80287+
                TranslatorX87::translate_fnop(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fsin:
                TranslatorX87::translate_fsin(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fcos:
                TranslatorX87::translate_fcos(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_f2xm1:
                TranslatorX87::translate_f2xm1(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fpatan:
                TranslatorX87::translate_fpatan(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fsincos:
                TranslatorX87::translate_fsincos(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fptan:
                TranslatorX87::translate_fptan(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fyl2x:
                TranslatorX87::translate_fyl2x(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fyl2xp1:
                TranslatorX87::translate_fyl2xp1(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fprem:
                TranslatorX87::translate_fprem(translation_result, cur_instr);
                break;

            case Opcode::kOpcodeName_fprem1:
                TranslatorX87::translate_fprem1(translation_result, cur_instr);
                break;

            // Identical body to default: is intentional — the explicit
            // case documents the "deliberate fall-through" set and pairs
            // with the sidecar's kKnownFallThrough allowlist that
            // suppresses the UNHANDLED warning for these opcodes only.
            // Don't merge with default:.
            //
            // Deliberate fall-through to stock:
            //   • fclex/finit/fldenv/fstenv: metadata-only ops with no
            //     stack-aware path; inlining buys nothing measurable
            //     (parity at best, fstenv was a 0.66× regression).
            //     Test coverage in test_*_compose.c confirms no
            //     m108-style internal-offset bug — see
            //     project_native_rosetta_lazy_f80.md.
            //   • fxsave/fxrstor: SSE-era extended save/restore
            //     (512 B incl. 8 ST f80 slots + 16 XMM/YMM + MXCSR).
            //     Inlining would inherit frstor's 0.38× eager-f80
            //     regression; we don't translate SSE.
            //
            // For all six: is_handled_x87 returns false, so the run
            // terminates before this point and the preceding handled
            // op's x87_end has already flushed deferred state to
            // memory — stock reads coherent X87State via x22.
            case Opcode::kOpcodeName_fclex:
            case Opcode::kOpcodeName_finit:
            case Opcode::kOpcodeName_fldenv:
            case Opcode::kOpcodeName_fstenv:
            case Opcode::kOpcodeName_fxsave:
            case Opcode::kOpcodeName_fxrstor:
                // The entry prologue pool-restricted the working mask —
                // hand stock back its own word before falling through.
                translation_result->free_gpr_mask = cache.stock_free_gpr_mask;
                return std::nullopt;

            default:
                // Returning nullopt means "I have no handler for this
                // opcode."  In the runtime/sidecar flow, the stub's FILTER
                // prologue routes only x87 opcodes to us, so reaching here
                // with an x87 opcode means an op we *forgot* to handle —
                // log a loud line so we notice.  Add a translate_* and
                // dispatch case, or list it as deliberate fall-through
                // above (and extend kKnownFallThrough on the sidecar side
                // for a new stock-only op like fxsave/fxrstor).
                //
                // Critical: hand stock back its own free_gpr_mask word
                // (the entry prologue pool-restricted the working copy).
                // Stock's translate_insn (run by the stub's STASH abs-jump)
                // carries its register-allocation state across the basic
                // block in that field — overwriting it with our scratch
                // values makes stock reuse registers it still holds live
                // (miscompiles), on top of redoing allocation as if every
                // instruction started a fresh block.
                if (is_x87_opcode(opcode)) {
                    const char* name = (opcode < kOpcodeNames.size()) ? kOpcodeNames[opcode] : "?";
                    fprintf(stdout,
                            "[rosettax87] unexpected x87 opcode %s (0x%x) reached "
                            "translator default — add a translate_* and dispatch "
                            "case, or list it as deliberate fall-through above\n",
                            name, static_cast<unsigned>(opcode));
                    fflush(stdout);
                }
                translation_result->free_gpr_mask = cache.stock_free_gpr_mask;
                return std::nullopt;
        }
    }

    // OPT-1: Tick the cache (decrements run counter; releases on expiry).
    // Then write stock's captured mask word back, excluding any GPRs still
    // pinned by the cache.  Fused pairs consumed 2 instructions — tick twice.
    const int consumed = fused.value_or(1);
    for (int i = 0; i < consumed; i++) {
        cache.tick();
    }

    if (cache.active()) {
        translation_result->free_gpr_mask = cache.stock_free_gpr_mask & ~cache.pinned_mask();
    } else {
        translation_result->free_gpr_mask = cache.stock_free_gpr_mask;
    }
    translation_result->free_fpr_mask =
        translation_result->_unoccupied_temporary_fprs_for_xmm_scalars;
    translation_result->_pinned_temporary_scalars = 0;

    return insn_idx + consumed;
}

#include <cstdint>

#include "rosetta_core/Features.h"
#include "rosetta_core/IROperand.h"
#include "rosetta_core/X87IR.h"

namespace X87IR {

// ── Pass 1: Dead Store Elimination ──────────────────────────────────────────
//
// Walk backward. A value node is dead if it has no live consumers (no other
// node references it in inputs[], and it's not in the final slot_val[]).
// Side-effect nodes (Store*, FCmp, FTst, FStsw) are never eliminated.

static void pass_dse(Context& ctx) {
    // Count uses for each node.
    int16_t use_count[kMaxNodes] = {};

    // Final stack values count as uses.
    for (short d : ctx.slot_val) {
        if (d >= 0 && d < ctx.num_nodes) {
            use_count[d]++;
        }
    }

    // Forward pass to count uses from non-dead nodes.
    for (int i = 0; i < ctx.num_nodes; i++) {
        auto& n = ctx.nodes[i];
        if (n.flags & kDead) {
            continue;
        }
        for (short input : n.inputs) {
            if (input >= 0) {
                use_count[input]++;
            }
        }
    }

    // Backward pass: mark dead nodes (pure-value nodes with zero uses).
    for (int i = ctx.num_nodes - 1; i >= 0; i--) {
        auto& n = ctx.nodes[i];
        if (n.flags & kDead) {
            continue;
        }

        // Side-effect nodes are never dead.
        switch (n.op) {
            case Op::StoreF64:
            case Op::StoreF32:
            case Op::StoreI16:
            case Op::StoreI32:
            case Op::StoreI64:
            case Op::FCmp:
            case Op::FTst:
            case Op::FStsw:
            case Op::FComI:
            case Op::StoreCW:
            case Op::LoadCW:
            case Op::BridgeMovRR:
            case Op::BridgeMovRI:
            case Op::BridgeLea:
            case Op::BridgeLoadG:
            case Op::BridgeStoreG:
            case Op::BridgeExt:
            case Op::BridgeAluRR:
            case Op::BridgeAluRI:
            case Op::BridgeAluRM:
            case Op::BridgeAluMR:
                continue;
            default:
                break;
        }

        if (use_count[i] == 0) {
            n.flags |= kDead;
            // Decrement use counts for this node's inputs.
            for (short input : n.inputs) {
                if (input >= 0) {
                    use_count[input]--;
                }
            }
        }
    }

    // Second backward pass to catch cascading dead nodes.
    for (int i = ctx.num_nodes - 1; i >= 0; i--) {
        auto& n = ctx.nodes[i];
        if (n.flags & kDead) {
            continue;
        }
        switch (n.op) {
            case Op::StoreF64:
            case Op::StoreF32:
            case Op::StoreI16:
            case Op::StoreI32:
            case Op::StoreI64:
            case Op::FCmp:
            case Op::FTst:
            case Op::FStsw:
            case Op::FComI:
            case Op::StoreCW:
            case Op::LoadCW:
            case Op::BridgeMovRR:
            case Op::BridgeMovRI:
            case Op::BridgeLea:
            case Op::BridgeLoadG:
            case Op::BridgeStoreG:
            case Op::BridgeExt:
            case Op::BridgeAluRR:
            case Op::BridgeAluRI:
            case Op::BridgeAluRM:
            case Op::BridgeAluMR:
                continue;
            default:
                break;
        }
        if (use_count[i] == 0) {
            n.flags |= kDead;
            for (short input : n.inputs) {
                if (input >= 0) {
                    use_count[input]--;
                }
            }
        }
    }
}

// ── Pass 2: FMA Detection ───────────────────────────────────────────────────
//
// Pattern: FMul(a, b) with exactly one use → FAdd(mul, c) or FAdd(c, mul)
//          → replace with FMAdd(a, b, c).
//
// Similarly:
//   FAdd(c, mul) where mul = FMul(a, b)  → FMAdd(a, b, c)  [c + a*b]
//   FSub(c, mul)                          → FMSub(a, b, c)  [c - a*b]
//   FSub(mul, c)                          → FNMSub(a, b, c) [a*b - c]

static void pass_fma(Context& ctx) {
    // Count uses first to find single-use FMul nodes.
    int16_t use_count[kMaxNodes] = {};
    for (short d : ctx.slot_val) {
        if (d >= 0 && d < ctx.num_nodes) {
            use_count[d]++;
        }
    }
    for (int i = 0; i < ctx.num_nodes; i++) {
        auto& n = ctx.nodes[i];
        if (n.flags & kDead) {
            continue;
        }
        for (short input : n.inputs) {
            if (input >= 0) {
                use_count[input]++;
            }
        }
    }

    for (int i = 0; i < ctx.num_nodes; i++) {
        auto& n = ctx.nodes[i];
        if (n.flags & kDead) {
            continue;
        }
        if (n.op != Op::FAdd && n.op != Op::FSub) {
            continue;
        }

        int16_t in0 = n.inputs[0];
        int16_t in1 = n.inputs[1];

        // Check if one input is a single-use FMul.
        auto try_fuse = [&](int mul_input_idx, int other_input_idx) -> bool {
            int16_t mul_id = (mul_input_idx == 0) ? in0 : in1;
            int16_t other_id = (other_input_idx == 0) ? in0 : in1;
            if (mul_id < 0 || mul_id >= ctx.num_nodes) {
                return false;
            }
            auto& mul_node = ctx.nodes[mul_id];
            if (mul_node.op != Op::FMul) {
                return false;
            }
            if (mul_node.flags & kDead) {
                return false;
            }
            if (use_count[mul_id] != 1) {
                return false;
            }

            // Determine FMA variant.
            Op fma_op;
            if (n.op == Op::FAdd) {
                fma_op = Op::FMAdd;  // c + a*b (or a*b + c, same thing)
            } else {
                // FSub
                if (mul_input_idx == 0) {
                    // FSub(mul, c) → a*b - c = FNMSub
                    fma_op = Op::FNMSub;
                } else {
                    // FSub(c, mul) → c - a*b = FMSub
                    fma_op = Op::FMSub;
                }
            }

            // Rewrite: n becomes FMA, mul becomes dead.
            n.op = fma_op;
            n.inputs[0] = mul_node.inputs[0];  // a
            n.inputs[1] = mul_node.inputs[1];  // b
            n.inputs[2] = other_id;            // c (addend)
            mul_node.flags |= kDead;
            use_count[mul_id] = 0;
            // Update use counts for the rewritten inputs.
            if (mul_node.inputs[0] >= 0) {
                use_count[mul_node.inputs[0]]++;
            }
            if (mul_node.inputs[1] >= 0) {
                use_count[mul_node.inputs[1]]++;
            }
            // other_id already had its use from the original node; no change needed.
            return true;
        };

        if (!try_fuse(0, 1)) {
            try_fuse(1, 0);
        }
    }
}

// ── Pass 3: FCOM + FSTSW Fusion ─────────────────────────────────────────────
//
// If a FCmp/FTst node is immediately followed by a FStsw with no intervening
// CC-modifying node, mark both with kFcomFused. The lowering keeps packed CC
// in a GPR between the two nodes, avoiding one LDRH in FStsw.

static void pass_fcom_fstsw_fusion(Context& ctx) {
    for (int i = 0; i < ctx.num_nodes; i++) {
        auto& n = ctx.nodes[i];
        if (n.flags & kDead) {
            continue;
        }
        if (n.op != Op::FStsw) {
            continue;
        }

        int16_t fcmp_id = n.inputs[0];
        if (fcmp_id < 0 || fcmp_id >= ctx.num_nodes) {
            continue;
        }
        auto& fcmp_node = ctx.nodes[fcmp_id];
        if (fcmp_node.op != Op::FCmp && fcmp_node.op != Op::FTst) {
            continue;
        }

        // Check no intervening CC-modifying instruction between fcmp and fstsw.
        bool clean = true;
        for (int j = fcmp_id + 1; j < i; j++) {
            auto& between = ctx.nodes[j];
            if (between.flags & kDead) {
                continue;
            }
            if (between.op == Op::FCmp || between.op == Op::FTst) {
                clean = false;
                break;
            }
        }
        if (clean) {
            fcmp_node.flags |= kFcomFused;
            n.flags |= kFcomFused;
        }
    }
}

// ── Pass 4: FMA Reduction Vectorisation ─────────────────────────────────────
//
// Recognise serial FMADD reduction chains of the shape:
//
//     A0 = <some node>            (initial accumulator: ReadSt, Const, …)
//     A1 = FMAdd(L0, W0, A0)
//     A2 = FMAdd(L1, W1, A1)      ← single-use chain on input[2]
//     ...
//     AN = FMAdd(L_{N-1}, W_{N-1}, A_{N-1})
//
// where every L_i and W_i is a single-use LoadF32 (or, chain-wide, LoadF64)
// with a simple base+disp MemRef, and each memory stream advances by its own
// fixed stride: disp(L_i) = disp(L_0) + stride_L*i, all sharing one base
// register (same for W_i, with an independent stride_W).  The stride is
// derived from the first step (i=1) and validated across the chain — the
// natural +element_size stride is the contiguous case, larger strides are
// the matrix-vector dot-product idiom (stride-8/16) that dominates the WoW
// workload (rank-1/2/10/12 in /tmp/epoch.prof).
//
// The pass tags the head FMAdd with kFmaReduceHead, and tags every FMAdd
// in the chain plus their LoadF32 input pair with kFmaReduceMember.  The
// lowering pass (X87IRLower::lower_fma_reduce) then emits the body as
// pair-loaded LDR D + FCVTL .2D + FMLA .2D iterations followed by a
// scalar FADDP horizontal sum and a final FADD with A_init.
//
// It only sees the FMAdd nodes pass_fma creates, so it runs only with
// kFmaContract.

static bool extract_simple_mem(const Node& n, uint8_t* base_reg, int64_t* disp) {
    if (n.mem_operand == nullptr) {
        return false;
    }
    const IROperand& op = *n.mem_operand;
    if (op.kind != IROperandKind::MemRef) {
        return false;
    }
    if ((op.mem.mem_flags & 1U) == 0) {  // no base register
        return false;
    }
    if ((op.mem.mem_flags & 2U) != 0) {  // index register present
        return false;
    }
    // Defensive: build() already ends a run at any segment-override operand.
    if (op.mem.seg_override != 0) {
        return false;
    }
    *base_reg = op.mem.base_reg;
    *disp = op.mem.disp;
    return true;
}

static void count_uses(const Context& ctx, int16_t* use_count) {
    for (int i = 0; i < kMaxNodes; i++) {
        use_count[i] = 0;
    }
    for (short d : ctx.slot_val) {
        if (d >= 0 && d < ctx.num_nodes) {
            use_count[d]++;
        }
    }
    for (int i = 0; i < ctx.num_nodes; i++) {
        const auto& n = ctx.nodes[i];
        if (n.flags & kDead) {
            continue;
        }
        for (short input : n.inputs) {
            if (input >= 0 && input < ctx.num_nodes) {
                use_count[input]++;
            }
        }
    }
}

static void pass_fma_reduce(Context& ctx) {
    // Bridged runs: the pass hoists absorbed loads to the chain head, which
    // is illegal across a bridge store (aliasing) and across a bridge
    // register write (a hoisted load's base register may change).  Fence the
    // whole pass off when any bridge node is present.
    for (int i = 0; i < ctx.num_nodes; i++) {
        switch (ctx.nodes[i].op) {
            case Op::BridgeMovRR:
            case Op::BridgeMovRI:
            case Op::BridgeLea:
            case Op::BridgeLoadG:
            case Op::BridgeStoreG:
            case Op::BridgeExt:
            case Op::BridgeAluRR:
            case Op::BridgeAluRI:
            case Op::BridgeAluRM:
            case Op::BridgeAluMR:
                return;
            default:
                break;
        }
    }
    int16_t use_count[kMaxNodes];
    count_uses(ctx, use_count);

    int16_t chain[kMaxNodes];

    for (int i = 0; i < ctx.num_nodes; i++) {
        const auto& head = ctx.nodes[i];
        if (head.flags & (kDead | kFmaReduceMember)) {
            continue;
        }
        if (head.op != Op::FMAdd) {
            continue;
        }
        // The candidate head's input[2] (= A_init) must NOT itself be an
        // FMAdd in a chain; otherwise this node would be a chain *member*
        // rather than the head.  We mark members lazily as we extend, so
        // this catches the case where a previously-tagged chain ended at
        // i-1 and i picks up an unrelated FMAdd with the same shape.
        if (head.inputs[2] >= 0 && head.inputs[2] < ctx.num_nodes) {
            const auto& a_init = ctx.nodes[head.inputs[2]];
            if (a_init.op == Op::FMAdd && (a_init.flags & kFmaReduceMember)) {
                continue;
            }
        }

        // Forward-walk: extend the chain as long as the next FMAdd in
        // node order uses our tail as input[2] AND the tail is single-use
        // (its only consumer is the next FMAdd's accumulator slot).
        int chain_len = 0;
        chain[chain_len++] = static_cast<int16_t>(i);
        auto tail = static_cast<int16_t>(i);

        while (chain_len < kMaxNodes) {
            int16_t next = -1;
            for (int j = tail + 1; j < ctx.num_nodes; j++) {
                const auto& m = ctx.nodes[j];
                if (m.flags & kDead) {
                    continue;
                }
                if (m.op != Op::FMAdd) {
                    continue;
                }
                if (m.inputs[2] != tail) {
                    continue;
                }
                next = static_cast<int16_t>(j);
                break;
            }
            if (next < 0) {
                break;
            }
            if (use_count[tail] != 1) {
                break;  // tail also used elsewhere — can't fold its FPR away
            }
            chain[chain_len++] = next;
            tail = next;
        }

        if (chain_len < 2) {
            continue;  // need ≥2 trios for the vector path to win
        }

        // Validate every member's LoadF32 inputs are single-use, simple-
        // mem, and that each stream is +4-contiguous from its first.
        bool ok = true;
        Op elem_op = Op::LoadF32;  // captured at k==0; all loads must match
        uint8_t base_l = 0;
        uint8_t base_w = 0;
        int64_t disp_l_first = 0;
        int64_t disp_w_first = 0;
        int64_t stride_l = 0;  // derived at k==1
        int64_t stride_w = 0;
        for (int k = 0; k < chain_len && ok; k++) {
            const auto& a = ctx.nodes[chain[k]];
            const int16_t l_id = a.inputs[0];
            const int16_t w_id = a.inputs[1];
            if (l_id < 0 || w_id < 0 || l_id >= ctx.num_nodes || w_id >= ctx.num_nodes) {
                ok = false;
                break;
            }
            const auto& ln = ctx.nodes[l_id];
            const auto& wn = ctx.nodes[w_id];
            // Both streams must be the same load width — all LoadF32 (matrix-
            // vector f32) or all LoadF64 (m64 doubles, e.g. TurtleWoW). The
            // lowerer vectorises a single element width per chain.
            if (k == 0) {
                if ((ln.op != Op::LoadF32 && ln.op != Op::LoadF64) || wn.op != ln.op) {
                    ok = false;
                    break;
                }
                elem_op = ln.op;
            } else if (ln.op != elem_op || wn.op != elem_op) {
                ok = false;
                break;
            }
            if ((ln.flags & kDead) || (wn.flags & kDead)) {
                ok = false;
                break;
            }
            if (use_count[l_id] != 1 || use_count[w_id] != 1) {
                ok = false;
                break;
            }
            if ((ln.flags & kFmaReduceMember) || (wn.flags & kFmaReduceMember)) {
                ok = false;  // already absorbed by another chain
                break;
            }
            uint8_t lb = 0;
            uint8_t wb = 0;
            int64_t ldisp = 0;
            int64_t wdisp = 0;
            if (!extract_simple_mem(ln, &lb, &ldisp)) {
                ok = false;
                break;
            }
            if (!extract_simple_mem(wn, &wb, &wdisp)) {
                ok = false;
                break;
            }
            if (k == 0) {
                base_l = lb;
                base_w = wb;
                disp_l_first = ldisp;
                disp_w_first = wdisp;
            } else {
                if (k == 1) {
                    // Derive each stream's constant stride from the first
                    // step. Generalises the original +4-contiguous rule to
                    // any fixed stride (matrix-vector idioms run stride-8/16)
                    // and to LoadF64. A zero stride would alias the same
                    // element into every lane — reject as ill-formed.
                    stride_l = ldisp - disp_l_first;
                    stride_w = wdisp - disp_w_first;
                    if (stride_l == 0 || stride_w == 0) {
                        ok = false;
                        break;
                    }
                }
                const int64_t expected_l = disp_l_first + stride_l * static_cast<int64_t>(k);
                const int64_t expected_w = disp_w_first + stride_w * static_cast<int64_t>(k);
                if (lb != base_l || ldisp != expected_l) {
                    ok = false;
                    break;
                }
                if (wb != base_w || wdisp != expected_w) {
                    ok = false;
                    break;
                }
            }
        }

        if (!ok) {
            continue;
        }

        // Tag.  Every chain FMAdd gets kFmaReduceMember; the first
        // additionally gets kFmaReduceHead.  Each absorbed LoadF32 gets
        // kFmaReduceMember so the lowerer skips its scalar LDR S + FCVT
        // emission and peak_live_fprs treats it as zero-cost.
        ctx.nodes[chain[0]].flags |= kFmaReduceHead;
        for (int k = 0; k < chain_len; k++) {
            auto& a = ctx.nodes[chain[k]];
            a.flags |= kFmaReduceMember;
            ctx.nodes[a.inputs[0]].flags |= kFmaReduceMember;
            ctx.nodes[a.inputs[1]].flags |= kFmaReduceMember;
        }
    }
}

// ── Pass 5: Exact int64 copy ────────────────────────────────────────────────
//
// Real x87 registers have a 64-bit mantissa, so FILD m64 followed by FISTP or
// FISTTP m64 copies any int64 unchanged.  Here the value goes through a
// binary64, which rounds every |v| >= 2^53: 0x5357452E46535745 ("EWSF.EWS")
// comes back as ...5800 and INT64_MAX as the integer indefinite.  Delphi's
// System.Move copies memory with exactly these pairs.
//
// A LoadI64 whose only consumers are StoreI64 nodes, and which is not left
// on the stack at the end of the run, is never observed as a number.  Retag
// it to LoadF64 and its stores to StoreF64: LDR D / STR D move the 64 bits
// with no conversion, rounding-mode dispatch or indefinite guard.  The nodes
// keep their positions, so memory order is unchanged, and
// remat_relieve_fpr_pressure never sinks a load past a store, fnstcw or
// bridge node.  Operands with a segment override are left alone so the GPR
// model's per-op cost for them does not drop.
//
// Runs last, after every pass that matches LoadF64 or StoreI64.

static bool has_seg_override(const Node& n) {
    return n.mem_operand != nullptr && n.mem_operand->mem.seg_override != 0;
}

static void pass_exact_i64_copy(Context& ctx) {
    int16_t store_uses[kMaxNodes] = {};
    bool other_use[kMaxNodes] = {};
    for (short d : ctx.slot_val) {
        if (d >= 0 && d < ctx.num_nodes) {
            other_use[d] = true;
        }
    }
    for (int i = 0; i < ctx.num_nodes; i++) {
        const auto& n = ctx.nodes[i];
        if (n.flags & kDead) {
            continue;
        }
        const int nrefs = node_ref_input_count(n);
        for (int k = 0; k < nrefs; k++) {
            const short in = n.inputs[k];
            if (in < 0 || in >= ctx.num_nodes) {
                continue;
            }
            if (n.op == Op::StoreI64 && k == 0 && !has_seg_override(n)) {
                store_uses[in]++;
            } else {
                other_use[in] = true;
            }
        }
    }

    for (int i = 0; i < ctx.num_nodes; i++) {
        auto& n = ctx.nodes[i];
        if (n.op != Op::LoadI64 || (n.flags & kDead) || other_use[i] || store_uses[i] == 0 ||
            has_seg_override(n)) {
            continue;
        }
        n.op = Op::LoadF64;
        for (int j = i + 1; j < ctx.num_nodes; j++) {
            auto& s = ctx.nodes[j];
            if (s.op == Op::StoreI64 && !(s.flags & kDead) && s.inputs[0] == i) {
                s.op = Op::StoreF64;
                s.flags &= static_cast<uint8_t>(~kTruncate);
            }
        }
    }
}

// ── Public API ──────────────────────────────────────────────────────────────

void optimize(Context& ctx) {
    pass_dse(ctx);
    // pass_fma_reduce only matches FMAdd nodes pass_fma creates.
    if (kFmaContract) {
        pass_fma(ctx);
        pass_fma_reduce(ctx);
    }
    pass_fcom_fstsw_fusion(ctx);
    pass_exact_i64_copy(ctx);
}

}  // namespace X87IR

#pragma once

#include <cstdint>
#include <optional>

struct TranslationResult;
struct IRInstr;

// ── x87 peephole-fusion identifiers ─────────────────────────────────────────
// Bit positions in try_peephole's `disabled_mask`.  See
// `TranslatorX87Fusion.cpp` for the actual fusion implementations.
enum class FusionId : int {
    fld_arithp = 0,    // FLD + FADDP / FSUBP / FDIVP / FMULP
    fld_fstp,          // FLD + FSTP
    fld_arith_fstp,    // FLD + ARITH + FSTP  (3-instruction)
    fld_fcomp_fstsw,   // FLD + FCOMP + FSTSW (3-instruction)
    fxch_arithp,       // FXCH + FADDP / FSUBP / etc.
    fxch_fstp,         // FXCH + FSTP
    fxch_fcom_fstsw,   // FXCH ST(i) + FCOM/FCOMP* + FSTSW (3-instruction)
    fxch_fcom,         // FXCH ST(i) + FCOM/FCOMP* (2-instruction, no FSTSW)
    fcom_fstsw,        // FCOM/FCOMP/FUCOM/FUCOMP/FCOMPP/FUCOMPP + FSTSW
    fld_fcompp_fstsw,  // FLD + FCOMPP/FUCOMPP + FSTSW (3-instruction, net pop)
    fld_fld_fucompp,   // FLD + FLD + FCOMPP/FUCOMPP [+ FSTSW] (3- or 4-instruction)
    fld_fcomp,         // FLD + FCOMP/FUCOMP (2-instruction, no FSTSW)
    fld_arith_arithp,  // FLD + ARITH + ARITHp (3-instruction, push+pop cancel)
    fld_arith,         // FLD + non-popping ARITH mem (2-instruction, single net push)
    arithp_fstp,       // ARITHp ST(1) + FSTP mem (2-instruction, skip stack writeback)
    fstp_fld,          // FSTP + FLD/FILD/FLDZ/FLD1/FLDconst (2-instruction, pop+push cancel)
    arith_fstp,       // non-popping ARITH + FSTP mem (2-instruction, skip intermediate stack store)
    arith_faddp,      // FMUL + FADDP/FSUBP/FSUBRP → FMADD/FMSUB/FNMSUB (FMA fusion)
    fstp_arith_fstp,  // FSTP mem + non-popping ARITH mem + FSTP mem (3-instruction, batched 2-pop)
    fild_fistp,       // FILD m64 + FISTP/FISTTP m64 as an exact 64-bit copy (the peephole,
                      // also nested up to three pairs deep, and the IR pass_exact_i64_copy)
    fld_fstp_m80,     // FLD m80 + FSTP m80 (2-instruction, raw ten-byte copy)
    kCount
};

namespace TranslatorX87 {

// Single entry point for all peephole fusion patterns.
// Returns total instructions consumed (≥2 for any fusion),
// or std::nullopt if no fusion matched (caller translates the current instruction alone).
// disabled_mask: bitmask of FusionId bits to skip (0 = all enabled).
auto try_peephole(TranslationResult* tr, IRInstr* instrs, int64_t num, int64_t idx,
                  uint64_t disabled_mask = 0) -> std::optional<int>;

}  // namespace TranslatorX87

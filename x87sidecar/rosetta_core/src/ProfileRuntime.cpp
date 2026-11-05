#include "rosetta_core/ProfileRuntime.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "rosetta_core/IRInstr.h"
#include "rosetta_core/OpcodeCompatibility.h"

namespace profile {

uint64_t hash_ir_stream(const IRInstr* instrs, size_t num_instrs) {
    // 64-bit FNV-1a over a CANONICALIZED copy of each IRInstr, so the same
    // logical IR hashes identically across launches, re-decodes AND host
    // Rosetta versions:
    //   - `opcode_` mapped to the internal id (opcode_host_to_internal):
    //     26.4 hosts and 26.5+ hosts encode the same instruction with
    //     different raw ids; hashing the internal id keeps hash
    //     values portable across macOS versions.  On modern hosts the map
    //     is identity, so existing modern-host hashes are unchanged.
    //   - `pc` zeroed: codegen may place the block at a different PC each run.
    //   - `flag_liveness` zeroed: recomputed from the block's successors on
    //     every decode; changes with no code change.
    //   - `_pad08` zeroed.
    //   - operand slots >= num_operands zeroed entirely: the decoder leaves
    //     stack garbage (per-run pointer values) in them.
    //   - used operands rebuilt field-by-field per kind: the union's dead
    //     value fields (IROperandRegister::_unused et al.) and pad bytes
    //     carry the same per-run garbage.
    //   - absolute addresses (AbsMem, which is what rip-relative lea/mov
    //     decode to, and fixup-carrying Immediates) keep only their page
    //     offset: the image slides by whole pages between launches, so the
    //     low 12 bits are the part of the address that survives ASLR.
    // Zeroing only `pc` (the old behaviour) left that garbage in the hash
    // input, so the hash was NOT stable across launches — which silently
    // broke every cross-run use of the hash.  Verified empirically on the same binary run
    // twice: every block hashed differently until dead bytes were masked.
    constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
    constexpr uint64_t kFnvPrime = 1099511628211ULL;
    uint64_t h = kFnvOffset;
    for (size_t i = 0; i < num_instrs; ++i) {
        IRInstr tmp = instrs[i];
        tmp.opcode_ = opcode_host_to_internal(tmp.opcode_);
        tmp.pc = 0;
        tmp._pad08 = 0;
        tmp.flag_liveness = 0;
        const int nops = tmp.num_operands > 4 ? 4 : tmp.num_operands;
        for (int o = 0; o < 4; ++o) {
            const IROperand src = tmp.operands[o];
            IROperand canon{};
            std::memset(&canon, 0, sizeof(canon));
            if (o < nops) {
                switch (src.kind) {
                    case IROperandKind::Register:
                        canon.reg.kind = src.reg.kind;
                        canon.reg.size = src.reg.size;
                        canon.reg.reg = src.reg.reg;
                        canon.reg.seg_override = src.reg.seg_override;
                        break;
                    case IROperandKind::MemRef:
                        canon.mem = src.mem;  // every byte is semantic
                        break;
                    case IROperandKind::AbsMem:
                        canon.abs_mem.kind = src.abs_mem.kind;
                        canon.abs_mem.size = src.abs_mem.size;
                        canon.abs_mem.addr_size = src.abs_mem.addr_size;
                        canon.abs_mem.value = src.abs_mem.value & 0xfff;
                        break;
                    case IROperandKind::Immediate:
                        canon.imm.kind = src.imm.kind;
                        canon.imm.size = src.imm.size;
                        canon.imm.addr_size = src.imm.addr_size;
                        canon.imm.mem_flags = src.imm.mem_flags;
                        canon.imm.value =
                            src.imm.mem_flags != 0 ? (src.imm.value & 0xfff) : src.imm.value;
                        break;
                    case IROperandKind::BranchOffset:
                        canon.branch.kind = src.branch.kind;
                        canon.branch.value = src.branch.value;
                        break;
                    case IROperandKind::ConditionCode:
                        canon.cc.kind = src.cc.kind;
                        canon.cc.cc = src.cc.cc;
                        break;
                    case IROperandKind::SegmentRegister:
                        canon.seg.kind = src.seg.kind;
                        canon.seg.seg_idx = src.seg.seg_idx;
                        break;
                    default:
                        canon = src;  // unknown kind: keep raw, don't guess
                        break;
                }
            }
            tmp.operands[o] = canon;
        }
        const auto* bytes = reinterpret_cast<const uint8_t*>(&tmp);
        for (size_t b = 0; b < sizeof(IRInstr); ++b) {
            h ^= bytes[b];
            h *= kFnvPrime;
        }
    }
    return h;
}

}  // namespace profile

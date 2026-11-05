#pragma once

#include <cstddef>
#include <cstdint>

struct IRInstr;

namespace profile {

// 64-bit FNV-1a hash over an IRInstr stream, with the per-instruction
// `pc` field zeroed for stable equivalence across runs.  Names a block in
// log lines.
uint64_t hash_ir_stream(const IRInstr* instrs, size_t num_instrs);

}  // namespace profile

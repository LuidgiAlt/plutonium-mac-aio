#include "rosetta_core/OpcodeCompatibility.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "rosetta_core/Opcode.h"

namespace {
bool g_table_active = false;
std::vector<uint16_t> g_host_to_internal;  // indexed by host id
std::vector<uint16_t> g_internal_to_host;  // indexed by kOpcodeName_*
}  // namespace

void opcode_set_host_table(const std::vector<std::string>& hostNames) {
    std::unordered_map<std::string, uint16_t> internalByName;
    for (uint16_t i = 0; i < kOpcodeNames.size(); ++i) {
        if (kOpcodeNames[i] != nullptr) {
            internalByName.emplace(kOpcodeNames[i], i);
        }
    }
    g_host_to_internal.assign(hostNames.size(), kOpcodeUnmapped);
    g_internal_to_host.assign(kOpcodeNames.size(), kOpcodeUnmapped);
    for (size_t h = 0; h < hostNames.size(); ++h) {
        const auto it = internalByName.find(hostNames[h]);
        if (it == internalByName.end()) {
            continue;
        }
        g_host_to_internal[h] = it->second;
        if (g_internal_to_host[it->second] == kOpcodeUnmapped) {
            g_internal_to_host[it->second] = static_cast<uint16_t>(h);
        }
    }
    g_table_active = true;
}

bool opcode_host_table_active() {
    return g_table_active;
}

void opcode_clear_host_table() {
    g_table_active = false;
    g_host_to_internal.clear();
    g_internal_to_host.clear();
}

auto opcode_first_unmapped_required() -> uint16_t {
    if (!g_table_active) {
        return kOpcodeUnmapped;
    }
    // The two ranges the stub filter claims and the translator handles.
    static constexpr uint16_t kRanges[][2] = {
        {kOpcodeName_fcmovb, kOpcodeName_fucomip},
        {kOpcodeName_f2xm1, kOpcodeName_fyl2xp1},
    };
    for (const auto& r : kRanges) {
        for (uint16_t op = r[0]; op <= r[1]; ++op) {
            if (g_internal_to_host[op] == kOpcodeUnmapped) {
                return op;
            }
        }
    }
    // What run bridging reads between x87 instructions (X87Bridge.h,
    // X87IRBuild.cpp). Keep in step with those.
    static constexpr uint16_t kBridged[] = {
        kOpcodeName_wait,   kOpcodeName_mov, kOpcodeName_lea, kOpcodeName_movzx, kOpcodeName_movsx,
        kOpcodeName_movsxd, kOpcodeName_add, kOpcodeName_sub, kOpcodeName_and,   kOpcodeName_or,
        kOpcodeName_xor,    kOpcodeName_inc, kOpcodeName_dec,
    };
    for (const uint16_t op : kBridged) {
        if (g_internal_to_host[op] == kOpcodeUnmapped) {
            return op;
        }
    }
    return kOpcodeUnmapped;
}

// Host ids are mapped by name from the runtime's own mnemonic table; the
// loader refuses to hook a runtime whose table it could not read.
auto opcode_host_to_internal(uint16_t opcode) -> uint16_t {
    return opcode < g_host_to_internal.size() ? g_host_to_internal[opcode] : kOpcodeUnmapped;
}

auto opcode_internal_to_host(uint16_t opcode) -> uint16_t {
    return opcode < g_internal_to_host.size() ? g_internal_to_host[opcode] : kOpcodeUnmapped;
}

// Host-side test of the name-derived opcode maps (opcode_set_host_table).
//
// Runs natively; no Rosetta involved. It feeds the table three name lists and
// compares the result with what the pinned tables say:
//   - the internal (26.5+) names, where both maps must be the identity;
//   - the 26.4 names, where both maps must equal the pinned 26.4 table;
//   - a list shaped like macOS 15.8.1's runtime, which is the 26.4 list
//     without the four XSAVE-family opcodes.
// An id past each list must stay unmapped.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "rosetta_core/Opcode.h"
#include "rosetta_core/OpcodeCompatibility.h"
#include "rosetta_core/RosettaCore.h"

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                 \
    do {                                 \
        if (!(cond)) {                   \
            ++g_failures;                \
            std::printf("      FAIL  "); \
            std::printf(__VA_ARGS__);    \
            std::printf("\n");           \
        }                                \
    } while (0)

// Rosetta's own opcodes: everything in the internal enum.
constexpr uint16_t kRealOpcodes = kOpcodeNames.size();

bool isXsaveFamily(uint16_t internal) {
    return internal == kOpcodeName_xgetbv || internal == kOpcodeName_xrstor ||
           internal == kOpcodeName_xsave || internal == kOpcodeName_xsetbv;
}

void checkRangesContiguous(const char* what) {
    static constexpr uint16_t kRanges[][2] = {
        {kOpcodeName_fcmovb, kOpcodeName_fucomip},
        {kOpcodeName_f2xm1, kOpcodeName_fyl2xp1},
    };
    for (const auto& r : kRanges) {
        const uint16_t base = opcode_internal_to_host(r[0]);
        for (uint16_t op = r[0]; op <= r[1]; ++op) {
            CHECK(opcode_internal_to_host(op) == base + (op - r[0]), "%s: %s breaks its x87 range",
                  what, kOpcodeNames[op]);
        }
    }
}

void testIdentity() {
    std::vector<std::string> names;
    for (uint16_t i = 0; i < kRealOpcodes; ++i) {
        names.emplace_back(kOpcodeNames[i]);
    }
    opcode_set_host_table(names);
    CHECK(opcode_host_table_active(), "identity: table not active");
    for (uint16_t i = 0; i < kRealOpcodes; ++i) {
        CHECK(opcode_internal_to_host(i) == i, "identity: %s -> host %u", kOpcodeNames[i],
              opcode_internal_to_host(i));
        CHECK(opcode_host_to_internal(i) == i, "identity: host %u -> %u", i,
              opcode_host_to_internal(i));
    }
    CHECK(opcode_host_to_internal(kRealOpcodes) == kOpcodeUnmapped,
          "identity: an id past the table is mapped");
    CHECK(opcode_first_unmapped_required() == kOpcodeUnmapped,
          "identity: a required opcode is unmapped");
    checkRangesContiguous("identity");
    opcode_clear_host_table();
}

// A runtime that lists the opcodes in another order: the mapping follows names.
// (The x87 ranges are not contiguous here, which the loader refuses separately.)
void testReordered() {
    std::vector<std::string> names;
    for (uint16_t i = kRealOpcodes; i-- > 0;) {
        names.emplace_back(kOpcodeNames[i]);
    }
    opcode_set_host_table(names);
    for (uint16_t i = 0; i < kRealOpcodes; ++i) {
        const auto host = static_cast<uint16_t>(kRealOpcodes - 1 - i);
        CHECK(opcode_internal_to_host(i) == host, "reversed: %s -> host %u, want %u",
              kOpcodeNames[i], opcode_internal_to_host(i), host);
        CHECK(opcode_host_to_internal(host) == i, "reversed: host %u -> %u, want %s", host,
              opcode_host_to_internal(host), kOpcodeNames[i]);
    }
    opcode_clear_host_table();
}

// macOS 15.8.1 style: xgetbv, xrstor, xsave and xsetbv absent; every later
// opcode moves down by the number of absent ones before it.
void testAbsentOpcodes() {
    std::vector<std::string> names;
    for (uint16_t i = 0; i < kRealOpcodes; ++i) {
        if (!isXsaveFamily(i)) {
            names.emplace_back(kOpcodeNames[i]);
        }
    }
    opcode_set_host_table(names);
    uint16_t gone = 0;
    for (uint16_t i = 0; i < kRealOpcodes; ++i) {
        if (isXsaveFamily(i)) {
            CHECK(opcode_internal_to_host(i) == kOpcodeUnmapped, "absent: %s is mapped",
                  kOpcodeNames[i]);
            ++gone;
            continue;
        }
        const auto want = static_cast<uint16_t>(i - gone);
        CHECK(opcode_internal_to_host(i) == want, "absent: %s -> host %u, want %u",
              kOpcodeNames[i], opcode_internal_to_host(i), want);
        CHECK(opcode_host_to_internal(want) == i, "absent: host %u -> %u, want %s", want,
              opcode_host_to_internal(want), kOpcodeNames[i]);
    }
    CHECK(opcode_host_to_internal(names.size()) == kOpcodeUnmapped,
          "absent: an id past the table is mapped");
    CHECK(opcode_first_unmapped_required() == kOpcodeUnmapped,
          "absent: a required opcode is unmapped");
    checkRangesContiguous("absent");
    opcode_clear_host_table();
}

// A list that lost an opcode the translator reads must be reported, whether the
// opcode is x87 or one that run bridging inspects.
void testMissingRequired() {
    for (const uint16_t gone :
         {static_cast<uint16_t>(kOpcodeName_fld), static_cast<uint16_t>(kOpcodeName_mov)}) {
        std::vector<std::string> names;
        for (uint16_t i = 0; i < kRealOpcodes; ++i) {
            if (i != gone) {
                names.emplace_back(kOpcodeNames[i]);
            }
        }
        opcode_set_host_table(names);
        CHECK(opcode_first_unmapped_required() == gone, "without %s: reported %u",
              kOpcodeNames[gone], opcode_first_unmapped_required());
        opcode_clear_host_table();
    }
}

// Without a table nothing is mapped (the loader then does not hook).
void testCleared() {
    CHECK(!opcode_host_table_active(), "clear: table still active");
    CHECK(opcode_first_unmapped_required() == kOpcodeUnmapped,
          "clear: required check fires without a table");
    for (uint16_t i = 0; i < kRealOpcodes; ++i) {
        CHECK(opcode_internal_to_host(i) == kOpcodeUnmapped, "clear: %s is mapped",
              kOpcodeNames[i]);
    }
}

}  // namespace

int main() {
    testIdentity();
    testReordered();
    testAbsentOpcodes();
    testMissingRequired();
    testCleared();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

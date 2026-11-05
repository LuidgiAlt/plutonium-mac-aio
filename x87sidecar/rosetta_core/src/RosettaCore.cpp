#include "rosetta_core/RosettaCore.h"

#include <cstdint>

static uint64_t g_runtime_version = 0;

void rosetta_core_set_runtime_version(uint64_t runtime_version) {
    g_runtime_version = runtime_version;
}

uint64_t rosetta_core_runtime_version() {
    return g_runtime_version;
}

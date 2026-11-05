#pragma once

#include <cstdint>

// Stores the host Rosetta runtime version for the OpcodeCompatibility layer.
void rosetta_core_set_runtime_version(uint64_t runtime_version);

uint64_t rosetta_core_runtime_version();

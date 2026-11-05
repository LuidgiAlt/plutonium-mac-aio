#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Everything the loader patches or reads inside Rosetta is located here, from
// the two on-disk images, before anything is launched:
//
//   /usr/libexec/rosetta/runtime                     (determineOffsets)
//   /Library/Apple/usr/libexec/oah/libRosettaRuntime (determineRuntimeOffsets)
//
// Both images are position independent with __TEXT at vmaddr 0, so a file
// offset is also the offset from the live image base.
//
// Functions are found by the anchors that survive a rebuild: the export table
// names `translator_translate`, every function names itself in the string it
// hands to Rosetta's assert helper, and the opcode mnemonic table is laid out
// in enum order. Byte-pattern signatures of prologues remain as the fallback
// and as a cross-check. Whatever is found is then validated structurally.
struct OffsetFinder {
    auto determineOffsets() -> bool;
    auto determineRuntimeOffsets() -> bool;

    // /usr/libexec/rosetta/runtime
    std::uint64_t offsetExportsFetch_ = 0;
    std::uint64_t offsetSvcCallEntry_ = 0;
    std::uint64_t offsetSvcCallRet_ = 0;
    std::uint64_t offsetDisableAot_ = 0;

    // libRosettaRuntime
    std::uint64_t offsetTransactionResultSize_ = 0;
    std::uint64_t offsetTranslateInsn_ = 0;
    std::uint64_t offsetInitLibrary_ = 0;
    std::uint64_t offsetTranslatorTranslate_ = 0;  // exported by name
    // End of the last section in __TEXT and the end of the segment (image
    // offsets): the bytes between them are alignment padding nothing uses,
    // where the stub is written. 0 when not found.
    std::uint64_t offsetTextSectionsEnd_ = 0;
    std::uint64_t offsetTextSegmentEnd_ = 0;
    // Size of the TranslationResult stock allocates, read from the immediate
    // translator_translate hands its allocator. 0 when not found.
    std::uint32_t translationResultSize_ = 0;
    // The first 16 bytes of the hooked function as they are on disk; the live
    // bytes must match before the hook is written over them.
    std::array<std::uint8_t, 16> translateInsnPrologue_{};
    // Host opcode id -> mnemonic, from the runtime's own table. Empty when the
    // table was not found. Trimmed to opcodeCount_ when that is known; otherwise
    // it runs on into whatever strings follow the table.
    std::vector<std::string> opcodeNames_;
    // How many opcodes the runtime has, from the bound in its opcode_to_string
    // assert ("index < N"). 0 when not found.
    std::uint32_t opcodeCount_ = 0;

    // Exports.version, read from the on-disk runtime by determineRuntimeOffsets.
    // Seeds the OpcodeCompatibility layer (26.4<->26.5) without needing the live
    // Exports struct (X19), so both attach modes get it the same way.
    std::uint64_t runtimeVersion_ = 0;

    // translate_insn's 36-byte prologue signature (stp/sub-sp/mov prologue).
    // Fallback locator, and what identifies the image in live memory.
    static constexpr std::array<std::uint8_t, 36> kTranslateInsnPattern = {
        0xFF, 0x43, 0x03, 0xD1, 0xFC, 0x6F, 0x07, 0xA9, 0xfa, 0x67, 0x08, 0xa9,
        0xF8, 0x5F, 0x09, 0xA9, 0xF6, 0x57, 0x0A, 0xA9, 0xF4, 0x4F, 0x0B, 0xA9,
        0xFD, 0x7B, 0x0C, 0xA9, 0xFD, 0x03, 0x03, 0x91, 0xF3, 0x03, 0x00, 0xAA};
};

// True when none of the AArch64 instructions in `bytes` (a multiple of 4)
// is pc-relative, so the sequence runs unchanged from another address. The
// hook displaces the first 16 bytes of its target into a stash and executes
// them there.
auto prologueIsRelocatable(const std::uint8_t* bytes, std::size_t len) -> bool;

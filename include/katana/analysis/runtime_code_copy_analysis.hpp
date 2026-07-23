#pragma once

#include "katana/analysis/evidence.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/sh4/disassembler.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace katana::analysis {

// A finite native-code candidate written into a literal slot of a code template before that
// template is copied.  The initial image value is useful for AOT coverage, but the live store is
// still authoritative at runtime; consequently these candidates never imply a static CFG edge.
struct RuntimeCodePatchCandidate {
    std::uint32_t store_instruction_address = 0u;
    std::uint32_t slot_address = 0u;
    std::uint32_t live_value = 0u;
    std::uint32_t target_address = 0u;
};

// Describes the statically bounded source side of a runtime code-copy loop.  The destination is
// deliberately represented as VBR plus a signed delta because VBR is guest state, not an analysis
// constant.  source_end_inclusive is the address of the final copied 32-bit word.
struct RuntimeCodeCopy {
    std::uint32_t setup_address = 0u;
    std::uint32_t loop_address = 0u;
    std::uint32_t source_begin = 0u;
    std::uint32_t source_end_inclusive = 0u;
    std::uint32_t source_byte_count = 0u;
    std::int32_t destination_vbr_delta = 0;
    std::vector<RuntimeCodePatchCandidate> patch_candidates;
    ControlFlowEvidence evidence = ControlFlowEvidence::GuardedPartial;
    bool aot_candidates_only = true;
    std::string reason;
};

struct RuntimeCodeCopyAnalysis {
    std::vector<RuntimeCodeCopy> copies;
};

// Recognizes only the bounded SH-4 form used by native runtime templates:
//
//   stc vbr,Rd; mov.l @(disp,pc),Rs; mov.w @(disp,pc),Rdelta
//   bra compare; add Rdelta,Rd
// loop:
//   mov.l @Rs+,Rtmp; mov.l Rtmp,@Rd; add #4,Rd
// compare:
//   mov.l @(disp,pc),Rend; cmp/hi Rend,Rs; bf loop
//
// Source bounds and delta are read from the committed initial snapshot.  Writable snapshots and
// pre-copy patch values remain guarded AOT candidates; the analysis does not claim their live
// values or synthesize control-flow edges.
[[nodiscard]] RuntimeCodeCopyAnalysis
analyze_runtime_code_copies(const katana::io::ExecutableImage& image,
                            std::span<const katana::sh4::DisassemblyLine> instructions);

} // namespace katana::analysis

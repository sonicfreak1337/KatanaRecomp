#pragma once

#include "katana/analysis/jump_table_analysis.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace katana::analysis::detail {

// Run-local provenance for a snapshot absolute table recognizer. This stays
// outside the installed analyzer ABI: persisted tables remain result data,
// while an identity-bound external declaration must re-prove the concrete
// producer instructions and PC literal against the current executable image.
struct SnapshotAbsoluteJumpTableProducerEvidence {
    std::uint32_t literal_address = 0u;
    // A displaced load selects one exact pointer cell even when the wider
    // snapshot scan records adjacent cells as conservative AOT candidates.
    // External one-entry declarations may match this cell, but never the
    // speculative width of the adjacent candidate scan.
    bool fixed_entry = false;
    std::uint32_t fixed_entry_address = 0u;
    std::uint32_t fixed_target = 0u;
    std::vector<std::uint32_t> instruction_addresses;
};

// Run-local proof that a relative BRAF/BSRF consumes an offset loaded from
// one concrete PC-relative table. The native recognizer derives the bounded
// entry count and complete target set; this evidence binds that count and
// every instruction in the non-clobbering producer slice to the current
// executable-image generation.
// Keeping it internal avoids adding transient provenance to persisted
// analyzer result layouts.
struct RelativeJumpTableProducerEvidence {
    std::uint32_t dispatch_address = 0u;
    std::uint32_t table_address = 0u;
    JumpTableDispatchKind dispatch_kind = JumpTableDispatchKind::Jump;
    JumpTableEncoding encoding = JumpTableEncoding::SignedRelative16;
    std::size_t bounded_entry_count = 0u;
    std::vector<std::uint32_t> instruction_addresses;
};

[[nodiscard]] std::optional<RelativeJumpTableProducerEvidence>
recognize_relative_jump_table_producer(
    const katana::io::ExecutableImage& image,
    std::span<const katana::sh4::DisassemblyLine> lines,
    std::size_t dispatch_index);

[[nodiscard]] std::optional<JumpTableAnalysis>
recognize_snapshot_absolute_jump_table_candidates_with_producer(
    const katana::io::ExecutableImage& image,
    std::span<const katana::sh4::DisassemblyLine> lines,
    std::size_t dispatch_index,
    SnapshotAbsoluteJumpTableProducerEvidence* producer_evidence);

} // namespace katana::analysis::detail

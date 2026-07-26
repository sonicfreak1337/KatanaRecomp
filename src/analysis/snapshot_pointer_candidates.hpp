#pragma once

#include "katana/analysis/jump_table_analysis.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace katana::analysis::detail {

struct SnapshotPointerCandidateScanPolicy {
    std::size_t minimum_entries = 1u;
    std::size_t maximum_scanned_slots = 1u;
    std::size_t maximum_skipped_slots = 0u;
    std::size_t maximum_consecutive_skipped_slots = 0u;
    bool treat_null_as_reserved = false;
    bool reject_truncated_scan = true;
};

[[nodiscard]] std::optional<JumpTableAnalysis>
analyze_snapshot_pointer_candidates(
    const katana::io::ExecutableImage& image,
    std::uint32_t evidence_address,
    std::uint32_t table_address,
    JumpTableDispatchKind dispatch_kind,
    const SnapshotPointerCandidateScanPolicy& policy);

} // namespace katana::analysis::detail

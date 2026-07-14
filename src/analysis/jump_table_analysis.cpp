#include "katana/analysis/jump_table_analysis.hpp"

#include <limits>
#include <stdexcept>

namespace katana::analysis {
namespace {

constexpr std::size_t maximum_jump_table_entries = 4096u;

bool is_executable_code_target(
    const katana::io::ExecutableImage& image,
    const std::uint32_t target
) {
    const auto* segment = image.find_segment(target, 2u);
    return (target & 1u) == 0u && segment != nullptr
        && segment->kind == katana::io::SegmentKind::Code
        && segment->permissions.executable;
}

}

JumpTableAnalysis analyze_jump_table(
    const katana::io::ExecutableImage& image,
    const std::uint32_t dispatch_address,
    const std::uint32_t table_address,
    const std::size_t entry_count
) {
    JumpTableAnalysis analysis;
    analysis.dispatch_address = dispatch_address;
    analysis.table_address = table_address;
    analysis.requested_entries = entry_count;

    if (entry_count == 0u || entry_count > maximum_jump_table_entries) {
        analysis.reason = "entry-count-out-of-range";
        return analysis;
    }
    if ((table_address & 3u) != 0u
        || entry_count > (std::numeric_limits<std::uint32_t>::max() - table_address) / 4u) {
        analysis.reason = "table-range-invalid";
        return analysis;
    }

    analysis.entries.reserve(entry_count);
    for (std::size_t index = 0u; index < entry_count; ++index) {
        const auto entry_address = table_address + static_cast<std::uint32_t>(index * 4u);
        JumpTableEntry entry;
        entry.index = index;
        entry.entry_address = entry_address;
        try {
            entry.target = image.read_u32_le(entry_address);
        } catch (const std::out_of_range&) {
            entry.reason = "entry-not-committed";
            analysis.entries.push_back(std::move(entry));
            analysis.reason = "table-entry-rejected";
            continue;
        }
        if (!is_executable_code_target(image, entry.target)) {
            entry.reason = "target-not-committed-executable-code";
            analysis.entries.push_back(std::move(entry));
            analysis.reason = "table-entry-rejected";
            continue;
        }
        entry.accepted = true;
        entry.reason = "bounded-absolute-target";
        analysis.entries.push_back(std::move(entry));
    }

    analysis.resolved = analysis.entries.size() == entry_count;
    for (const auto& entry : analysis.entries) {
        analysis.resolved = analysis.resolved && entry.accepted;
    }
    if (analysis.resolved) {
        analysis.reason = "bounded-table";
    } else if (analysis.reason.empty()) {
        analysis.reason = "table-entry-rejected";
    }
    return analysis;
}

}

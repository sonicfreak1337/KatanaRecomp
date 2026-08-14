#pragma once

#include "katana/analysis/function_value_analysis.hpp"
#include "katana/analysis/recursive_analysis.hpp"
#include "katana/io/executable_image.hpp"

#include <span>
#include <vector>

namespace katana::analysis::detail {

class GuardedNativeEntryShapeCache;

struct StaticCallbackSinkContract final {
    std::uint32_t function_address = 0u;
    // Bit 0..3 corresponds to the function's incoming r4..r7.
    std::uint8_t argument_mask = 0u;

    bool operator==(const StaticCallbackSinkContract&) const = default;
};

// ABI-light companion to the full FunctionValue analysis. It discovers
// executable constants which flow through direct, statically bound calls into
// persistent pointer stores. The result is guarded AOT inventory only: it
// never resolves an indirect transfer and therefore cannot make a dynamic
// target set complete.
[[nodiscard]] GuardedCodeInventory analyze_static_callback_inventory(
    const katana::io::ExecutableImage& image,
    std::span<const katana::sh4::DisassemblyLine> lines,
    std::span<const FunctionCandidate> function_candidates,
    std::span<const std::uint32_t> external_block_entries,
    std::span<const std::uint32_t> non_root_function_entry_hints,
    GuardedNativeEntryShapeCache& native_entry_shapes,
    std::vector<StaticCallbackSinkContract>* callback_sink_contracts =
        nullptr);

} // namespace katana::analysis::detail

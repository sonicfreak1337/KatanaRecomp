#pragma once

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/io/executable_image.hpp"

#include <span>
#include <cstdint>
#include <vector>

namespace katana::analysis::detail {

class GuardedNativeEntryShapeCache;

using StaticCallbackSinkContract =
    katana::analysis::StaticCallbackSinkContract;
using StaticCallbackFieldSinkContract =
    katana::analysis::StaticCallbackFieldSinkContract;

// Returns the sorted, unique record-field displacements which feed an
// actually decoded indirect call/jump in the supplied image.  These are
// positive ABI-shape diagnostics, not structure names or admission evidence.
// A displacement without receiver provenance must never create a callback
// root or complete an indirect target set.
[[nodiscard]] std::vector<std::int32_t>
discover_static_callback_field_offsets(
    std::span<const katana::sh4::DisassemblyLine> lines);

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
        nullptr,
    std::vector<StaticCallbackFieldSinkContract>*
        callback_field_sink_contracts = nullptr);

} // namespace katana::analysis::detail

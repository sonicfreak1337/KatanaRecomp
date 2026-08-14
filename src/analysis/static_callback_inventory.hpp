#pragma once

#include "katana/analysis/function_value_analysis.hpp"
#include "katana/analysis/recursive_analysis.hpp"
#include "katana/io/executable_image.hpp"

#include <span>
#include <cstdint>
#include <vector>

namespace katana::analysis::detail {

class GuardedNativeEntryShapeCache;

struct StaticCallbackSinkContract final {
    std::uint32_t function_address = 0u;
    // Bit 0..3 corresponds to the function's incoming r4..r7.
    std::uint8_t argument_mask = 0u;

    bool operator==(const StaticCallbackSinkContract&) const = default;
};

// Exact primary-image record-field load which feeds an indirect call/jump.
// Receiver identity remains an analyzer-local proof domain; this exported
// shape is used only to retain guarded, identity-checked latent AOT entries
// written to the same field. It never completes the indirect target set.
struct StaticCallbackFieldSinkContract final {
    std::uint32_t function_address = 0u;
    std::uint32_t call_instruction_address = 0u;
    std::uint32_t load_instruction_address = 0u;
    std::int32_t displacement = 0;
    std::uint8_t width = 0u;
    bool call = false;

    bool operator==(const StaticCallbackFieldSinkContract&) const = default;
};

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

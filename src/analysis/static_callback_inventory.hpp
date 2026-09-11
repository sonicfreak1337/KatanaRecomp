#pragma once

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/io/executable_image.hpp"

#include <span>
#include <cstdint>
#include <memory>
#include <vector>

namespace katana::analysis::detail {

class GuardedNativeEntryShapeCache;

struct StaticSourceConstantCall final {
    std::uint32_t function_address = 0u;
    std::uint32_t call_instruction_address = 0u;
    std::uint32_t callee_address = 0u;
    std::array<std::optional<std::uint32_t>, 4> arguments;
};

// Conditional, intraprocedural source flow under SH-C callee-save semantics.
// Only immediate/PC-literal/register expressions survive; memory and spills
// supply no constant argument authority. All visits to a callsite must agree.
// The caller supplies the current, source-validated CFG including resolved
// table edges. No graph/selector completeness, file load or entry is proved.
[[nodiscard]] std::vector<StaticSourceConstantCall>
discover_source_constant_calls(
    const katana::io::ExecutableImage& image,
    std::span<const BasicBlock> blocks,
    std::span<const FunctionInfo> functions,
    std::span<const std::uint32_t> recognized_callees);

struct StaticExternalLiteralTransferCandidate final {
    std::uint32_t call_instruction_address = 0u;
    std::uint32_t literal_address = 0u;
    std::uint32_t target_address = 0u;
    bool call = false;

    bool operator==(const StaticExternalLiteralTransferCandidate&) const = default;
};

struct StaticExternalLiteralTransferBlock final {
    std::uint32_t address = 0u;
    std::uint32_t byte_size = 0u;
};

// Positive source-bound inventory from decoded resident instructions. The
// literal must feed the branch register before its delay slot on one bounded,
// contiguous path. No runtime target set, executable owner or ABI is proven.
[[nodiscard]] std::vector<StaticExternalLiteralTransferCandidate>
discover_external_literal_transfer_candidates(
    const katana::io::ExecutableImage& image,
    std::span<const katana::sh4::DisassemblyLine> lines,
    std::span<const StaticExternalLiteralTransferBlock> blocks,
    std::uint32_t primary_begin, std::uint64_t primary_size);

struct StaticCodePointerVectorInventory final {
    std::vector<StoredCodeAddressCandidate> candidates;
    bool truncated = false;
};

// Reuses the ordinary four-entry / repeated ordered-pair vector classifier.
// Anchors name immutable carrier references in this exact image view; they
// restrict the search but never establish callback semantics by themselves.
[[nodiscard]] StaticCodePointerVectorInventory
discover_anchored_static_code_pointer_vectors(
    const katana::io::ExecutableImage& image,
    const katana::io::ImageSegment& active_source,
    std::span<const std::uint32_t> anchors,
    GuardedNativeEntryShapeCache& native_entry_shapes);

using StaticCallbackSinkContract =
    katana::analysis::StaticCallbackSinkContract;
using StaticPersistentPointerSinkContract =
    katana::analysis::StaticPersistentPointerSinkContract;
using StaticCallbackFieldSinkContract =
    katana::analysis::StaticCallbackFieldSinkContract;
using StaticCallbackRecordTableContract =
    katana::analysis::StaticCallbackRecordTableContract;

// Retains only per-function semantic models whose complete CFG/input binding
// is byte-for-byte unchanged.  The session is an analysis accelerator, never
// authority: an image/proof binding change clears it and a missing or changed
// function is evaluated cold before it can contribute callback inventory.
class StaticCallbackInventorySession final {
  public:
    StaticCallbackInventorySession();
    ~StaticCallbackInventorySession();
    StaticCallbackInventorySession(StaticCallbackInventorySession&&) noexcept;
    StaticCallbackInventorySession&
    operator=(StaticCallbackInventorySession&&) noexcept;

    StaticCallbackInventorySession(const StaticCallbackInventorySession&) =
        delete;
    StaticCallbackInventorySession& operator=(
        const StaticCallbackInventorySession&) = delete;

    void clear() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend GuardedCodeInventory analyze_static_callback_inventory(
        const katana::io::ExecutableImage&,
        std::span<const katana::sh4::DisassemblyLine>,
        std::span<const FunctionCandidate>,
        std::span<const std::uint32_t>,
        std::span<const std::uint32_t>, GuardedNativeEntryShapeCache&,
        std::vector<StaticCallbackSinkContract>*,
        std::vector<StaticPersistentPointerSinkContract>*,
        std::vector<StaticCallbackFieldSinkContract>*,
        std::vector<StaticCallbackRecordTableContract>*,
        StaticCallbackInventorySession*,
        std::vector<PersistentFieldCopyContract>*);
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
    std::vector<StaticPersistentPointerSinkContract>*
        persistent_pointer_sink_contracts = nullptr,
    std::vector<StaticCallbackFieldSinkContract>*
        callback_field_sink_contracts = nullptr,
    std::vector<StaticCallbackRecordTableContract>*
        callback_record_table_contracts = nullptr,
    StaticCallbackInventorySession* session = nullptr,
    std::vector<PersistentFieldCopyContract>* persistent_field_copies = nullptr);

} // namespace katana::analysis::detail

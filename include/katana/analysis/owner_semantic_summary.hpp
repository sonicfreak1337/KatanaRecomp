#pragma once

#include "katana/analysis/hardware_audit.hpp"
#include "katana/ir/ir.hpp"
#include "katana/runtime/native_port_semantics.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::analysis {

// This is an analyzer-side identity contract.  The summary never invents an
// owner boundary or a byte identity: callers must provide both from an
// independently authenticated image/function-map source.
struct OwnerSemanticBoundary final {
    std::uint32_t entry_address = 0u;
    std::uint32_t size = 0u;
    std::string identity;
    bool exact = false;
    bool identity_bound = false;

    [[nodiscard]] bool operator==(const OwnerSemanticBoundary&) const = default;
};

// One scalar PC-relative literal whose bytes were independently bound to the
// current input identity.  Owner summaries consume values only through this
// explicit ledger; an effective address in IR is not itself byte authority.
struct OwnerSemanticLiteralEvidence final {
    std::uint32_t instruction_address = 0u;
    std::uint32_t literal_address = 0u;
    std::uint32_t bits = 0u;
    std::uint8_t width_bytes = 0u;
    bool signed_value = false;
    std::string identity;

    [[nodiscard]] bool operator==(
        const OwnerSemanticLiteralEvidence&) const = default;
};

enum class OwnerSemanticSummaryStatus : std::uint8_t {
    Incomplete,
    Complete,
    Truncated
};

enum class OwnerSemanticAuthority : std::uint8_t {
    Unbound,
    IdentityBound,
    Invalidated
};

enum class OwnerSemanticEffectKind : std::uint8_t {
    MemoryRead,
    MemoryWrite,
    HardwareRead,
    HardwareWrite,
    HardwarePrefetch,
    CpuStatusRead,
    CpuStatusWrite,
    CpuAccumulatorRead,
    CpuAccumulatorWrite,
    CpuSpecialRegisterRead,
    CpuSpecialRegisterWrite,
    DirectCall,
    Return
};

// Effects are ordered by block/instruction order in OwnerSemanticSummary.  A
// missing address is intentional: it means the IR did not prove an exact
// memory address and must never be interpreted as a provider/resource match.
struct OwnerSemanticEffect final {
    // Dense order is the comparison key used by provider contracts.  The
    // path token identifies the owner block/SCC that produced the effect.
    std::uint16_t order = 0u;
    std::uint32_t instruction_address = 0u;
    OwnerSemanticEffectKind kind = OwnerSemanticEffectKind::CpuStatusRead;
    runtime::NativePortProviderOperation provider_operation =
        runtime::NativePortProviderOperation::Read;
    runtime::NativePortProviderResourceKind provider_resource_kind =
        runtime::NativePortProviderResourceKind::ProviderState;
    bool canonical_address_known = false;
    std::uint32_t canonical_address = 0u;
    std::uint32_t write_mask = 0u;
    std::uint32_t clear_mask = 0u;
    std::string region;
    std::string register_name;
    std::string resource;
    std::string address_expression;
    std::string value_expression;
    std::string result_expression;
    std::string path_identity;
    std::optional<std::uint32_t> exact_address;
    std::uint8_t width_bytes = 0u;
    std::uint8_t destination_register = 0u;
    std::uint8_t source_register = 0u;
    ir::SpecialRegister special_register = ir::SpecialRegister::None;
    std::optional<HardwareAccessReference> hardware_reference;
};

struct OwnerSemanticGuard final {
    std::uint16_t order = 0u;
    std::string expression;
    std::string path_identity;
};

struct OwnerSemanticResultProjection final {
    runtime::NativePortProviderReturnAction action =
        runtime::NativePortProviderReturnAction::Return;
    std::uint32_t gpr_write_mask = 0u;
    std::uint32_t special_write_mask = 0u;
    std::uint8_t status_write_mask = 0u;
    std::string target_expression;
    std::string error_expression;
    std::string cpu_state_expression;
    std::string title_state_expression;
    // This is deliberately independent from OwnerSemanticSummaryStatus:
    // structural owner closure can be complete while a native provider's
    // result/state equivalence still requires an explicit contract.
    bool complete = false;
};

struct OwnerSemanticBlockSummary final {
    std::uint32_t start_address = 0u;
    std::vector<std::uint32_t> successor_addresses;
    std::vector<std::uint32_t> predecessor_addresses;
    std::vector<std::size_t> effect_indices;
    std::size_t scc_index = 0u;
    bool reachable = false;
    bool has_indirect_successor = false;
    bool open_edge = false;
};

struct OwnerSemanticSccSummary final {
    std::vector<std::size_t> block_indices;
    std::vector<std::size_t> successor_scc_indices;
    std::vector<std::size_t> effect_indices;
    std::size_t fixed_point_iterations = 0u;
    bool cyclic = false;
    bool structurally_stable = false;
    bool open_edge = false;
};

struct OwnerSemanticSummaryOptions final {
    // There is deliberately no block or instruction cap.  The only output
    // budget is on retained effects/reasons, so a large owner is summarized
    // compositionally instead of being silently reduced to a slice.
    std::size_t maximum_effects = 4096u;
    std::size_t maximum_reasons = 64u;
    std::size_t maximum_loop_iterations = 64u;
    std::size_t maximum_text_bytes = 1024u;
    std::size_t maximum_guards = 256u;
};

struct OwnerSemanticSummary final {
    OwnerSemanticBoundary boundary;
    OwnerSemanticSummaryStatus status = OwnerSemanticSummaryStatus::Incomplete;
    OwnerSemanticAuthority authority = OwnerSemanticAuthority::Unbound;

    bool boundary_valid = false;
    bool control_flow_closed = false;
    bool all_blocks_reachable = false;
    bool has_unknown_operations = false;
    bool has_open_edges = false;
    bool has_unstable_loops = false;
    bool provider_contract_required = false;
    bool has_contract_gaps = false;
    bool effects_truncated = false;
    bool guards_truncated = false;

    std::size_t instruction_count = 0u;
    std::size_t predecessor_edge_count = 0u;
    std::size_t truncated_effect_count = 0u;
    std::size_t truncated_guard_count = 0u;

    std::vector<OwnerSemanticBlockSummary> blocks;
    std::vector<OwnerSemanticSccSummary> sccs;
    std::vector<OwnerSemanticGuard> guards;
    std::vector<OwnerSemanticEffect> effects;
    OwnerSemanticResultProjection result;
    std::vector<std::string> incomplete_reasons;
    std::string digest;
};

// Builds a deterministic, pointer-free owner summary.  Hardware references
// are observations from the existing audit contract; they do not promote an
// unresolved IR operation or establish provider semantics.  A Complete result
// means the owner-side structural/effect summary is closed and identity-bound;
// callers must still validate a native provider's effect/state contract and
// require result.complete before admitting a replacement.
[[nodiscard]] OwnerSemanticSummary summarize_owner_semantics(
    const ir::Function& function,
    OwnerSemanticBoundary boundary,
    std::span<const HardwareAccessReference> hardware_references = {},
    std::span<const OwnerSemanticLiteralEvidence> literal_evidence = {},
    OwnerSemanticSummaryOptions options = {});

[[nodiscard]] const char*
owner_semantic_summary_status_name(OwnerSemanticSummaryStatus status) noexcept;
[[nodiscard]] const char*
owner_semantic_authority_name(OwnerSemanticAuthority authority) noexcept;
[[nodiscard]] const char*
owner_semantic_effect_kind_name(OwnerSemanticEffectKind kind) noexcept;

} // namespace katana::analysis

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace katana::runtime {

// This is a static declaration of the observable contract implemented by a
// native replacement.  It is deliberately separate from the callback ABI:
// the callback may mutate NativePortContext, while this bounded description
// is what export-time analysis can compare against the displaced guest owner.
inline constexpr std::uint32_t native_port_provider_semantics_contract_version =
    1u;
inline constexpr std::size_t native_port_provider_semantics_maximum_contracts =
    4'096u;
inline constexpr std::size_t native_port_provider_semantics_maximum_guards =
    256u;
inline constexpr std::size_t native_port_provider_semantics_maximum_effects =
    256u;
inline constexpr std::size_t native_port_provider_semantics_maximum_text =
    4'096u;

enum class NativePortProviderOperation : std::uint8_t {
    Read,
    Write,
    ReadModifyWrite,
    AckClear,
    Wait,
    Interrupt,
    Fifo
};

enum class NativePortProviderResourceKind : std::uint8_t {
    HardwareRegister,
    Memory,
    Status,
    Queue,
    Interrupt,
    ProviderState
};

// This intentionally mirrors the four terminal hook outcomes without making
// the semantics header depend on native_port.hpp's declaration order.
enum class NativePortProviderReturnAction : std::uint8_t {
    ContinueOriginal,
    Jump,
    Return,
    Abort
};

// DeclaredOnly is the migration-safe pilot mode: every declared contract is
// authoritative and must match, while hooks not yet migrated retain their
// existing structural admission.  RequiredForHardwareClosure is the final
// product gate and rejects every closing hardware hook without a contract.
enum class NativePortProviderSemanticCoverage : std::uint8_t {
    DeclaredOnly,
    RequiredForHardwareClosure
};

struct NativePortProviderGuard final {
    // Guards are ordered, bounded symbolic predicates.  Their text is a
    // canonical provider/analyzer token, never a source path or retail data.
    std::uint16_t order = 0u;
    std::string_view expression;
    // Exact owner block/SCC lane in which the predicate is observed.
    std::string_view path_identity;
};

struct NativePortProviderEffect final {
    // Effects are compared in this exact order; order must be dense from 0.
    std::uint16_t order = 0u;
    NativePortProviderOperation operation =
        NativePortProviderOperation::Read;
    NativePortProviderResourceKind resource_kind =
        NativePortProviderResourceKind::HardwareRegister;
    std::uint8_t width = 0u;
    bool canonical_address_known = false;
    std::uint32_t canonical_address = 0u;
    std::uint32_t write_mask = 0u;
    std::uint32_t clear_mask = 0u;
    std::string_view region;
    std::string_view register_name;
    std::string_view resource;
    std::string_view address_expression;
    std::string_view value_expression;
    std::string_view result_expression;
    // Exact owner block/SCC lane which observes this effect. Effects with
    // identical resources but different path identities are not equivalent:
    // preserving this correlation prevents a provider from moving a write
    // across a guest branch while matching the same flat effect list.
    std::string_view path_identity;
};

struct NativePortProviderResultProjection final {
    NativePortProviderReturnAction action =
        NativePortProviderReturnAction::Return;
    std::uint32_t gpr_write_mask = 0u;
    std::uint32_t special_write_mask = 0u;
    std::uint8_t status_write_mask = 0u;
    std::string_view target_expression;
    std::string_view error_expression;
    std::string_view cpu_state_expression;
    std::string_view title_state_expression;
};

struct NativePortProviderSemanticContract final {
    std::uint32_t contract_version =
        native_port_provider_semantics_contract_version;
    // The binding is resolved by exact guest address.  The validator then
    // requires provider_symbol to equal that binding's symbol.
    std::uint32_t hook_guest_address = 0u;
    bool authoritative = true;
    std::string_view provider_symbol;
    // SHA-256 identity of this semantic declaration, not of retail bytes.
    std::string_view semantic_identity;
    // The owner-summary and provider implementation identities are separate
    // domains.  They must never be collapsed into semantic_identity: the
    // former binds the analyzer result, the latter binds the native code
    // component that claims to implement it.
    std::string_view expected_owner_semantic_identity;
    std::string_view provider_implementation_identity;
    std::span<const NativePortProviderGuard> guards;
    std::span<const NativePortProviderEffect> effects;
    NativePortProviderResultProjection result;
};

} // namespace katana::runtime

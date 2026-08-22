#include "katana/analysis/native_provider_equivalence.hpp"

#include "katana/io/input_provenance.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>

namespace katana::analysis {
namespace {

using runtime::NativePortProviderEffect;
using runtime::NativePortProviderOperation;
using runtime::NativePortProviderResourceKind;
using runtime::NativePortProviderReturnAction;
using runtime::NativePortProviderSemanticContract;

constexpr std::size_t sha256_hex_digits = 64u;

void append_u64(std::string& output, const std::uint64_t value) {
    for (std::size_t index = 0u; index < sizeof(value); ++index)
        output.push_back(static_cast<char>((value >> (index * 8u)) & 0xFFu));
}

void append_text(std::string& output, const std::string_view value) {
    append_u64(output, value.size());
    if (!value.empty()) output.append(value.data(), value.size());
}

[[nodiscard]] bool bounded_text(const std::string_view value,
                                const bool required) noexcept {
    if (required && value.empty()) return false;
    if (value.size() > runtime::native_port_provider_semantics_maximum_text)
        return false;
    return std::all_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x21u && byte <= 0x7Eu;
    });
}

[[nodiscard]] bool canonical_sha256(const std::string_view value) noexcept {
    constexpr std::string_view prefix{"sha256:"};
    if (value.size() != prefix.size() + sha256_hex_digits ||
        !value.starts_with(prefix))
        return false;
    return std::all_of(value.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
                       value.end(), [](const char character) {
                           return (character >= '0' && character <= '9') ||
                                  (character >= 'a' && character <= 'f');
                       });
}

[[nodiscard]] bool valid_operation(const NativePortProviderOperation operation) noexcept {
    switch (operation) {
    case NativePortProviderOperation::Read:
    case NativePortProviderOperation::Write:
    case NativePortProviderOperation::ReadModifyWrite:
    case NativePortProviderOperation::AckClear:
    case NativePortProviderOperation::Wait:
    case NativePortProviderOperation::Interrupt:
    case NativePortProviderOperation::Fifo:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_resource(const NativePortProviderResourceKind resource) noexcept {
    switch (resource) {
    case NativePortProviderResourceKind::HardwareRegister:
    case NativePortProviderResourceKind::Memory:
    case NativePortProviderResourceKind::Status:
    case NativePortProviderResourceKind::Queue:
    case NativePortProviderResourceKind::Interrupt:
    case NativePortProviderResourceKind::ProviderState:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_action(const NativePortProviderReturnAction action) noexcept {
    switch (action) {
    case NativePortProviderReturnAction::ContinueOriginal:
    case NativePortProviderReturnAction::Jump:
    case NativePortProviderReturnAction::Return:
    case NativePortProviderReturnAction::Abort:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_width(const std::uint8_t width) noexcept {
    switch (width) {
    case 1u:
    case 2u:
    case 4u:
    case 8u:
    case 16u:
    case 32u:
    case 64u:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool unknown_token(const std::string_view value) noexcept {
    return value.empty() || value == "unknown" || value == "multiple-targets";
}

[[nodiscard]] bool valid_effect_shape(const NativePortProviderEffect& effect) noexcept {
    if (!valid_operation(effect.operation) || !valid_resource(effect.resource_kind) ||
        !valid_width(effect.width) || !bounded_text(effect.region, true) ||
        !bounded_text(effect.resource, true) || !bounded_text(effect.register_name, false) ||
        !bounded_text(effect.address_expression, !effect.canonical_address_known) ||
        !bounded_text(effect.value_expression, false) ||
        !bounded_text(effect.result_expression, false) ||
        !bounded_text(effect.path_identity, true) ||
        unknown_token(effect.region) || unknown_token(effect.resource) ||
        unknown_token(effect.path_identity) ||
        (!effect.address_expression.empty() && unknown_token(effect.address_expression)) ||
        (!effect.value_expression.empty() && unknown_token(effect.value_expression)) ||
        (!effect.result_expression.empty() && unknown_token(effect.result_expression)))
        return false;

    switch (effect.operation) {
    case NativePortProviderOperation::Read:
        return !effect.result_expression.empty() && effect.write_mask == 0u &&
               effect.clear_mask == 0u;
    case NativePortProviderOperation::Write:
        return !effect.value_expression.empty() && effect.clear_mask == 0u;
    case NativePortProviderOperation::ReadModifyWrite:
        return !effect.value_expression.empty() && !effect.result_expression.empty() &&
               effect.write_mask != 0u;
    case NativePortProviderOperation::AckClear:
        return effect.clear_mask != 0u || effect.write_mask != 0u;
    case NativePortProviderOperation::Wait:
        return !effect.result_expression.empty();
    case NativePortProviderOperation::Interrupt:
        return !effect.result_expression.empty();
    case NativePortProviderOperation::Fifo:
        return !effect.value_expression.empty() || !effect.result_expression.empty();
    }
    return false;
}

[[nodiscard]] bool valid_result_shape(
    const runtime::NativePortProviderResultProjection& result) noexcept {
    if (!valid_action(result.action) || result.gpr_write_mask > 0xFFFFu ||
        !bounded_text(result.target_expression, false) ||
        !bounded_text(result.error_expression, false) ||
        !bounded_text(result.cpu_state_expression, false) ||
        !bounded_text(result.title_state_expression, false) ||
        (!result.target_expression.empty() && unknown_token(result.target_expression)) ||
        (!result.error_expression.empty() && unknown_token(result.error_expression)) ||
        (!result.cpu_state_expression.empty() && unknown_token(result.cpu_state_expression)) ||
        (!result.title_state_expression.empty() && unknown_token(result.title_state_expression)))
        return false;

    switch (result.action) {
    case NativePortProviderReturnAction::ContinueOriginal:
    case NativePortProviderReturnAction::Return:
        return result.target_expression.empty() && result.error_expression.empty();
    case NativePortProviderReturnAction::Jump:
        return !result.target_expression.empty() && result.error_expression.empty();
    case NativePortProviderReturnAction::Abort:
        return !result.error_expression.empty();
    }
    return false;
}

[[nodiscard]] bool canonical_contract_material(
    const NativePortProviderSemanticContract& contract,
    std::string& material) {
    if (contract.contract_version !=
            runtime::native_port_provider_semantics_contract_version ||
        contract.hook_guest_address == 0u || (contract.hook_guest_address & 1u) != 0u ||
        !bounded_text(contract.provider_symbol, true) ||
        !canonical_sha256(contract.expected_owner_semantic_identity) ||
        !canonical_sha256(contract.provider_implementation_identity) ||
        contract.guards.size() > runtime::native_port_provider_semantics_maximum_guards ||
        contract.effects.empty() ||
        contract.effects.size() > runtime::native_port_provider_semantics_maximum_effects ||
        !valid_result_shape(contract.result))
        return false;

    material.clear();
    material.reserve(128u + contract.guards.size() * 16u +
                     contract.effects.size() * 96u);
    // Identity domain v1.  The provider implementation identity is kept out
    // of this stream by contract: changing the native implementation must be
    // a separate binding check, while behaviour identity remains stable.
    append_text(material, "katana-native-provider-semantics-v1");
    append_u64(material, contract.contract_version);
    append_u64(material, contract.hook_guest_address);
    append_u64(material, contract.authoritative);
    append_text(material, contract.provider_symbol);
    append_text(material, contract.expected_owner_semantic_identity);

    append_u64(material, contract.guards.size());
    for (std::size_t index = 0u; index < contract.guards.size(); ++index) {
        const auto& guard = contract.guards[index];
        if (guard.order != index || !bounded_text(guard.expression, true) ||
            !bounded_text(guard.path_identity, true) ||
            unknown_token(guard.expression) ||
            unknown_token(guard.path_identity))
            return false;
        append_u64(material, guard.order);
        append_text(material, guard.expression);
        append_text(material, guard.path_identity);
    }

    append_u64(material, contract.effects.size());
    for (std::size_t index = 0u; index < contract.effects.size(); ++index) {
        const auto& effect = contract.effects[index];
        if (effect.order != index || !valid_effect_shape(effect)) return false;
        append_u64(material, effect.order);
        append_u64(material, static_cast<std::uint8_t>(effect.operation));
        append_u64(material, static_cast<std::uint8_t>(effect.resource_kind));
        append_u64(material, effect.width);
        append_u64(material, effect.canonical_address_known);
        append_u64(material, effect.canonical_address);
        append_u64(material, effect.write_mask);
        append_u64(material, effect.clear_mask);
        append_text(material, effect.region);
        append_text(material, effect.register_name);
        append_text(material, effect.resource);
        append_text(material, effect.address_expression);
        append_text(material, effect.value_expression);
        append_text(material, effect.result_expression);
        append_text(material, effect.path_identity);
    }

    append_u64(material, static_cast<std::uint8_t>(contract.result.action));
    append_u64(material, contract.result.gpr_write_mask);
    append_u64(material, contract.result.special_write_mask);
    append_u64(material, contract.result.status_write_mask);
    append_text(material, contract.result.target_expression);
    append_text(material, contract.result.error_expression);
    append_text(material, contract.result.cpu_state_expression);
    append_text(material, contract.result.title_state_expression);
    return true;
}

[[nodiscard]] bool owner_effect_is_representable(
    const OwnerSemanticEffect& effect) noexcept {
    if (effect.width_bytes == 0u || effect.width_bytes > 64u ||
        unknown_token(effect.region) || unknown_token(effect.resource) ||
        (!effect.address_expression.empty() && unknown_token(effect.address_expression)) ||
        (!effect.value_expression.empty() && unknown_token(effect.value_expression)) ||
        (!effect.result_expression.empty() && unknown_token(effect.result_expression)))
        return false;
    if (effect.hardware_reference &&
        effect.hardware_reference->runtime_support != HardwareRuntimeSupport::Implemented)
        return false;
    // A prefetch is intentionally not collapsed into a read: provider
    // semantics have no prefetch operation, so this would lose an observable
    // contract distinction.
    if (effect.kind == OwnerSemanticEffectKind::HardwarePrefetch) return false;
    return valid_operation(effect.provider_operation) &&
           valid_resource(effect.provider_resource_kind);
}

[[nodiscard]] bool same_effect(const OwnerSemanticEffect& owner,
                               const NativePortProviderEffect& provider) noexcept {
    return owner.provider_operation == provider.operation &&
           owner.provider_resource_kind == provider.resource_kind &&
           owner.width_bytes == provider.width &&
           owner.canonical_address_known == provider.canonical_address_known &&
           owner.canonical_address == provider.canonical_address &&
           owner.write_mask == provider.write_mask && owner.clear_mask == provider.clear_mask &&
           owner.region == provider.region && owner.register_name == provider.register_name &&
           owner.resource == provider.resource &&
           owner.address_expression == provider.address_expression &&
           owner.value_expression == provider.value_expression &&
           owner.result_expression == provider.result_expression &&
           owner.path_identity == provider.path_identity;
}

[[nodiscard]] bool same_result(
    const OwnerSemanticResultProjection& owner,
    const runtime::NativePortProviderResultProjection& provider) noexcept {
    return owner.action == provider.action &&
           owner.gpr_write_mask == provider.gpr_write_mask &&
           owner.special_write_mask == provider.special_write_mask &&
           owner.status_write_mask == provider.status_write_mask &&
           owner.target_expression == provider.target_expression &&
           owner.error_expression == provider.error_expression &&
           owner.cpu_state_expression == provider.cpu_state_expression &&
           owner.title_state_expression == provider.title_state_expression;
}

} // namespace

std::string native_provider_semantic_identity(
    const NativePortProviderSemanticContract& contract) {
    std::string material;
    if (!canonical_contract_material(contract, material)) return {};
    return "sha256:" + io::sha256_bytes(material);
}

NativeProviderEquivalenceResult match_native_provider_semantics(
    const OwnerSemanticSummary& owner,
    const NativePortProviderSemanticContract& contract) noexcept {
    NativeProviderEquivalenceResult result;
    if (!contract.authoritative) {
        result.reason = NativeProviderEquivalenceReason::ContractNotAuthoritative;
        return result;
    }

    try {
        if (!canonical_sha256(contract.semantic_identity)) {
            result.reason = NativeProviderEquivalenceReason::ContractIdentityInvalid;
            return result;
        }
        if (!canonical_sha256(contract.provider_implementation_identity)) {
            result.reason =
                NativeProviderEquivalenceReason::ProviderImplementationIdentityInvalid;
            return result;
        }
        const auto computed_identity = native_provider_semantic_identity(contract);
        if (computed_identity.empty()) {
            result.reason = NativeProviderEquivalenceReason::ContractIdentityInvalid;
            return result;
        }
        if (computed_identity != contract.semantic_identity) {
            result.reason = NativeProviderEquivalenceReason::ContractIdentityMismatch;
            return result;
        }
    } catch (...) {
        result.reason = NativeProviderEquivalenceReason::ContractIdentityInvalid;
        return result;
    }

    if (!owner.boundary_valid || owner.authority != OwnerSemanticAuthority::IdentityBound ||
        !owner.boundary.exact || !owner.boundary.identity_bound) {
        result.reason = NativeProviderEquivalenceReason::OwnerSummaryNotIdentityBound;
        return result;
    }
    if (owner.status != OwnerSemanticSummaryStatus::Complete ||
        !owner.control_flow_closed || !owner.all_blocks_reachable ||
        owner.has_unknown_operations || owner.has_open_edges || owner.has_unstable_loops ||
        owner.has_contract_gaps || owner.effects_truncated || owner.guards_truncated) {
        result.reason = NativeProviderEquivalenceReason::OwnerSummaryIncomplete;
        return result;
    }
    if (!owner.result.complete) {
        result.reason = NativeProviderEquivalenceReason::ResultNotRepresentable;
        return result;
    }
    if (contract.expected_owner_semantic_identity != owner.digest) {
        result.reason = NativeProviderEquivalenceReason::OwnerIdentityMismatch;
        return result;
    }
    if (contract.hook_guest_address != owner.boundary.entry_address) {
        result.reason = NativeProviderEquivalenceReason::HookAddressMismatch;
        return result;
    }

    if (owner.guards.size() != contract.guards.size()) {
        result.reason = NativeProviderEquivalenceReason::GuardCountMismatch;
        return result;
    }
    for (std::size_t index = 0u; index < owner.guards.size(); ++index) {
        const auto& owner_guard = owner.guards[index];
        const auto& provider_guard = contract.guards[index];
        if (owner_guard.order != index || provider_guard.order != index ||
            owner_guard.expression.empty() || unknown_token(owner_guard.expression)) {
            result.reason = NativeProviderEquivalenceReason::GuardNotRepresentable;
            return result;
        }
        if (owner_guard.expression != provider_guard.expression ||
            owner_guard.path_identity != provider_guard.path_identity) {
            result.reason = NativeProviderEquivalenceReason::GuardMismatch;
            return result;
        }
    }

    if (owner.effects.size() != contract.effects.size()) {
        result.reason = NativeProviderEquivalenceReason::EffectCountMismatch;
        return result;
    }
    for (std::size_t index = 0u; index < owner.effects.size(); ++index) {
        const auto& owner_effect = owner.effects[index];
        const auto& provider_effect = contract.effects[index];
        if (owner_effect.order != index || provider_effect.order != index ||
            !owner_effect_is_representable(owner_effect) ||
            !valid_effect_shape(provider_effect)) {
            result.reason = NativeProviderEquivalenceReason::EffectNotRepresentable;
            return result;
        }
        if (!same_effect(owner_effect, provider_effect)) {
            result.reason = NativeProviderEquivalenceReason::EffectMismatch;
            return result;
        }
    }

    if (!valid_result_shape(contract.result)) {
        result.reason = NativeProviderEquivalenceReason::ResultNotRepresentable;
        return result;
    }
    if (!same_result(owner.result, contract.result)) {
        result.reason = NativeProviderEquivalenceReason::ResultMismatch;
        return result;
    }

    result.matched = true;
    result.reason = NativeProviderEquivalenceReason::Match;
    return result;
}

const char* native_provider_equivalence_reason_token(
    const NativeProviderEquivalenceReason reason) noexcept {
    switch (reason) {
    case NativeProviderEquivalenceReason::Match:
        return "match";
    case NativeProviderEquivalenceReason::ContractNotAuthoritative:
        return "contract-not-authoritative";
    case NativeProviderEquivalenceReason::ContractIdentityInvalid:
        return "contract-identity-invalid";
    case NativeProviderEquivalenceReason::ContractIdentityMismatch:
        return "contract-identity-mismatch";
    case NativeProviderEquivalenceReason::ProviderImplementationIdentityInvalid:
        return "provider-implementation-identity-invalid";
    case NativeProviderEquivalenceReason::OwnerSummaryNotIdentityBound:
        return "owner-summary-not-identity-bound";
    case NativeProviderEquivalenceReason::OwnerSummaryIncomplete:
        return "owner-summary-incomplete";
    case NativeProviderEquivalenceReason::HookAddressMismatch:
        return "hook-owner-address-mismatch";
    case NativeProviderEquivalenceReason::OwnerIdentityMismatch:
        return "owner-semantic-identity-mismatch";
    case NativeProviderEquivalenceReason::GuardCountMismatch:
        return "guard-count-mismatch";
    case NativeProviderEquivalenceReason::GuardNotRepresentable:
        return "guard-not-representable";
    case NativeProviderEquivalenceReason::GuardMismatch:
        return "guard-mismatch";
    case NativeProviderEquivalenceReason::EffectCountMismatch:
        return "effect-count-mismatch";
    case NativeProviderEquivalenceReason::EffectNotRepresentable:
        return "effect-not-representable";
    case NativeProviderEquivalenceReason::EffectMismatch:
        return "effect-mismatch";
    case NativeProviderEquivalenceReason::ResultNotRepresentable:
        return "result-not-representable";
    case NativeProviderEquivalenceReason::ResultMismatch:
        return "result-mismatch";
    }
    return "owner-summary-incomplete";
}

} // namespace katana::analysis

#pragma once

#include "katana/analysis/owner_semantic_summary.hpp"
#include "katana/runtime/native_port_semantics.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace katana::analysis {

// These reasons are intentionally a closed set.  They are suitable for
// bounded product diagnostics and never carry source paths, retail bytes, or
// arbitrary exception text.
enum class NativeProviderEquivalenceReason : std::uint8_t {
    Match,
    ContractNotAuthoritative,
    ContractIdentityInvalid,
    ContractIdentityMismatch,
    ProviderImplementationIdentityInvalid,
    OwnerSummaryNotIdentityBound,
    OwnerSummaryIncomplete,
    HookAddressMismatch,
    OwnerIdentityMismatch,
    GuardCountMismatch,
    GuardNotRepresentable,
    GuardMismatch,
    EffectCountMismatch,
    EffectNotRepresentable,
    EffectMismatch,
    ResultNotRepresentable,
    ResultMismatch
};

struct NativeProviderEquivalenceResult final {
    bool matched = false;
    NativeProviderEquivalenceReason reason =
        NativeProviderEquivalenceReason::OwnerSummaryIncomplete;
};

// Computes the canonical identity of the semantic contract.  The encoding is
// versioned and includes every semantic contract field; the self-referential
// semantic_identity and the separate provider_implementation_identity are
// deliberately excluded.  The latter is an independent binding to native
// code, not part of the guest/provider behaviour identity.  An empty result
// means that the bounded contract shape cannot be canonicalized.
[[nodiscard]] std::string native_provider_semantic_identity(
    const runtime::NativePortProviderSemanticContract& contract);

[[nodiscard]] NativeProviderEquivalenceResult match_native_provider_semantics(
    const OwnerSemanticSummary& owner,
    const runtime::NativePortProviderSemanticContract& contract) noexcept;

[[nodiscard]] const char* native_provider_equivalence_reason_token(
    NativeProviderEquivalenceReason reason) noexcept;

} // namespace katana::analysis

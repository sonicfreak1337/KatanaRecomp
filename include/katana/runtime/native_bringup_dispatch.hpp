#pragma once

#include "katana/runtime/block_table.hpp"
#include "katana/runtime/native_bringup_artifact.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace katana::runtime {

// This contract is deliberately independent of the historical indirect-
// dispatch runtime. It may be linked into a generated native port without
// pulling in a CPU dispatcher, materializer, interpreter or diagnostics.
enum class NativeBringupDispatchMiss : std::uint8_t {
    None,
    UnknownCompiledTarget,
    MissingIdentity,
    SourceIdentityMismatch,
    GenerationMismatch,
    InvalidEntry,
    UnmappedTarget,
    PhysicalIdentityMismatch
};

struct NativeBringupDispatchPackIdentity {
    std::uint32_t contract_version =
        native_bringup_evidence_contract_version;
    std::string_view authoring_artifact_identity;
    std::string_view project_id;
    std::string_view project_version;
    std::string_view analysis_identity;
    std::string_view aot_pack_identity;
    std::uint64_t aot_pack_generation = 0u;
};

struct NativeBringupDispatchStaticAotBinding {
    BlockAddress block;
    std::uint32_t size = 0u;
    BlockEndKind end_kind = BlockEndKind::Fallthrough;
    std::string_view block_code_identity;
};

struct NativeBringupDispatchEntry {
    // Candidate is execution-admitted only after exact export revalidation;
    // its AnalyzerReproof task and missing_proof remain open. Neither a hit nor
    // an observation can promote it to Proven.
    NativeBringupTargetEvidence admission;
    NativeBringupDispatchStaticAotBinding source;
    NativeBringupDispatchStaticAotBinding target;
};

struct NativeBringupDispatchPack {
    NativeBringupDispatchPackIdentity identity;
    std::span<const NativeBringupDispatchEntry> allowlist;
};

inline constexpr std::size_t native_bringup_dispatch_maximum_entries = 4096u;

struct NativeBringupDispatchObservation {
    bool executed = false;
    NativeBringupDispatchMiss miss = NativeBringupDispatchMiss::None;
    NativeBringupTransferKind transfer_kind =
        NativeBringupTransferKind::TailJumpRegister;
    std::uint32_t callsite = 0u;
    std::uint32_t target = 0u;
    std::uint64_t aot_pack_generation = 0u;
    std::uint64_t runtime_generation = 0u;
    std::uint64_t occurrences = 0u;

    [[nodiscard]] bool operator==(
        const NativeBringupDispatchObservation&) const noexcept = default;
};

inline constexpr std::size_t native_bringup_dispatch_observation_capacity =
    16u;

class NativeBringupDispatchObservations final {
  public:
    void record(bool executed,
                NativeBringupDispatchMiss miss,
                NativeBringupTransferKind transfer_kind,
                std::uint32_t callsite,
                std::uint32_t target,
                std::uint64_t aot_pack_generation,
                std::uint64_t runtime_generation) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::uint64_t total_occurrences() const noexcept;
    [[nodiscard]] std::uint64_t dropped_events() const noexcept;
    [[nodiscard]] std::span<const NativeBringupDispatchObservation> events()
        const noexcept;
    [[nodiscard]] std::string serialize_json() const;

  private:
    std::array<NativeBringupDispatchObservation,
               native_bringup_dispatch_observation_capacity>
        events_{};
    std::size_t event_count_ = 0u;
    std::uint64_t total_occurrences_ = 0u;
    std::uint64_t dropped_events_ = 0u;
};

class NativeBringupDispatchContext final {
  public:
    static constexpr bool release_eligible = false;

    // Pack/span/string backing must remain immutable and outlive this context.
    // Generated constexpr arrays and string literals are the intended owner.
    const NativeBringupDispatchPack& pack;
    std::uint64_t runtime_generation = 0u;
    NativeBringupDispatchObservations& observations;

    [[nodiscard]] bool validated_static_view_current(
        const RuntimeBlockTable& table) const noexcept;

  private:
    NativeBringupDispatchContext(
        const RuntimeBlockTable& validated_table,
        const NativeBringupDispatchPack& validated_pack,
        std::uint64_t active_runtime_generation,
        NativeBringupDispatchObservations& active_observations) noexcept;

    const NativeBringupDispatchEntry* validated_allowlist_data_ = nullptr;
    std::size_t validated_allowlist_size_ = 0u;
    NativeBringupDispatchPackIdentity validated_identity_;
    const RuntimeBlockTable* validated_table_ = nullptr;
    std::uint64_t validated_table_lifetime_ = 0u;
    std::uint64_t validated_table_generation_ = 0u;

    friend NativeBringupDispatchContext
    make_native_bringup_dispatch_context(
        const RuntimeBlockTable&,
        const NativeBringupDispatchPack&,
        std::uint64_t,
        NativeBringupDispatchObservations&);
};

[[nodiscard]] NativeBringupDispatchContext
make_native_bringup_dispatch_context(
    const RuntimeBlockTable& table,
    const NativeBringupDispatchPack& pack,
    std::uint64_t runtime_generation,
    NativeBringupDispatchObservations& observations);

struct NativeBringupDispatchPreflightRequest {
    NativeBringupTransferKind transfer_kind =
        NativeBringupTransferKind::TailJumpRegister;
    std::uint32_t callsite = 0u;
    std::uint32_t target = 0u;
    std::uint32_t continuation = 0u;
    BlockAddress source;
    BlockVariantKey variant;
};

struct NativeBringupDispatchPreflightResult {
    RuntimeBlockHandle block;
    ValidatedBlockExecution execution;
    std::uint32_t target = 0u;
    std::uint32_t physical_target = 0u;
};

class NativeBringupDispatchError final : public std::runtime_error {
  public:
    NativeBringupDispatchError(
        const NativeBringupDispatchPreflightRequest& request,
        NativeBringupDispatchMiss miss);

    [[nodiscard]] NativeBringupDispatchMiss miss() const noexcept;
    [[nodiscard]] NativeBringupTransferKind transfer_kind() const noexcept;
    [[nodiscard]] std::uint32_t callsite() const noexcept;
    [[nodiscard]] std::uint32_t target() const noexcept;
    [[nodiscard]] BlockAddress source() const noexcept;

  private:
    NativeBringupDispatchMiss miss_ = NativeBringupDispatchMiss::InvalidEntry;
    NativeBringupTransferKind transfer_kind_ =
        NativeBringupTransferKind::TailJumpRegister;
    std::uint32_t callsite_ = 0u;
    std::uint32_t target_ = 0u;
    BlockAddress source_;
};

[[nodiscard]] const char* native_bringup_dispatch_miss_name(
    NativeBringupDispatchMiss value) noexcept;

[[nodiscard]] NativeBringupDispatchPreflightResult
preflight_native_bringup_dispatch(
    const RuntimeBlockTable& table,
    const NativeBringupDispatchContext& context,
    const NativeBringupDispatchPreflightRequest& request);

} // namespace katana::runtime

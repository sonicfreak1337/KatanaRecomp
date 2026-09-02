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
#include <vector>

namespace katana::runtime {

class NativePortLoadedAotBinder;

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
    PhysicalIdentityMismatch,
    CoverageSourceMissing,
    CoverageTargetMissing,
    LoadedModuleInactive,
    LoadedModuleIdentityMismatch,
    HookReplacementConflict
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

// Coverage authority deliberately remains disjoint from the proof allowlist.
// It can compile and execution-admit an exact complete-disassembly entry in a
// NativeBringup product, but can never promote Evidence/Product closure.
inline constexpr std::uint32_t native_bringup_coverage_contract_version = 2u;

enum class NativeBringupCoverageSourceKind : std::uint8_t {
    StaticAot,
    LoadedAot,
};

struct NativeBringupCoveragePackIdentity final {
    std::uint32_t contract_version =
        native_bringup_coverage_contract_version;
    std::string_view authority_identity;
    std::string_view project_id;
    std::string_view project_version;
    std::string_view analysis_identity;
    std::string_view aot_pack_identity;
    std::uint64_t aot_pack_generation = 0u;
};

// Optional proof/fixed-placement accelerator for one unresolved register
// transfer whose source block is already compiled. The target is intentionally
// not part of this key. A missing record is not a dispatch veto: NativeBringup
// authenticates the exact source block through the sealed Static-AOT table or
// the active identity-/generation-bound Loaded-AOT module instance.
struct NativeBringupCoverageSourceTransfer final {
    NativeBringupTransferKind transfer_kind =
        NativeBringupTransferKind::TailJumpRegister;
    std::uint32_t callsite = 0u;
    std::uint32_t continuation = 0u;
    NativeBringupCoverageSourceKind source_kind =
        NativeBringupCoverageSourceKind::StaticAot;
    NativeBringupDispatchStaticAotBinding source;
    // LoadedAot only. StaticAot requires the empty/zero tuple.
    std::string_view source_module_identity;
    std::uint32_t source_runtime_start = 0u;
    std::uint32_t source_module_size = 0u;
    std::uint32_t source_module_offset = 0u;
};

// One exact compiled entry rooted either in the complete lossless disassembly
// or in the loaded-AOT analyzer's final immutable ingress bitmap. The latter
// contains only bounded explicit roots, complete embedded entry tables and
// positive guarded callback/vtable entries. runtime_start binds loader
// placement; source_start binds the generated AOT view. Runtime admission still
// requires the matching staged/active lifecycle generation and exact resident
// block bytes.
struct NativeBringupCoverageEntry final {
    std::string_view module_identity;
    std::uint32_t module_size = 0u;
    std::uint32_t source_start = 0u;
    std::uint32_t runtime_start = 0u;
    std::uint32_t module_relative_offset = 0u;
    NativeBringupDispatchStaticAotBinding target;
};

struct NativeBringupCoverageDispatchPack final {
    NativeBringupCoveragePackIdentity identity;
    // Both spans are optional accelerators. Executable authority is the sealed
    // Static-AOT table plus the generated Loaded-AOT module entry bitmap owned
    // by NativePortLoadedAotBinder, never membership in these diagnostic lists.
    std::span<const NativeBringupCoverageSourceTransfer> source_transfers;
    std::span<const NativeBringupCoverageEntry> entries;
};

inline constexpr std::size_t
    // Whole-game NativeBringup coverage aggregates unresolved source blocks
    // across every prepared Loaded-AOT module. This is therefore an aggregate
    // budget, not the historical per-module block ceiling.
    native_bringup_coverage_maximum_source_transfers = 262'144u;
inline constexpr std::size_t native_bringup_coverage_maximum_entries =
    16'384u;

struct NativeBringupCoverageObservation final {
    NativeBringupTransferKind transfer_kind =
        NativeBringupTransferKind::TailJumpRegister;
    std::uint32_t callsite = 0u;
    std::uint32_t target = 0u;
    NativeBringupCoverageSourceKind source_kind =
        NativeBringupCoverageSourceKind::StaticAot;
    std::string_view source_module_identity;
    std::string_view source_block_identity;
    std::uint32_t source_runtime_start = 0u;
    std::uint32_t source_module_offset = 0u;
    std::uint64_t source_lifecycle_generation = 0u;
    std::string_view target_module_identity;
    std::string_view target_block_identity;
    std::uint32_t target_runtime_start = 0u;
    std::uint32_t target_module_offset = 0u;
    std::uint64_t target_lifecycle_generation = 0u;
    std::uint64_t aot_pack_generation = 0u;
    std::uint64_t runtime_generation = 0u;
    std::uint64_t occurrences = 0u;
};

inline constexpr std::size_t native_bringup_coverage_observation_capacity =
    4096u;

// Separate non-proof journal for successful CoverageOnly execution admission.
// Event and open-addressed index storage are allocated once at product startup;
// record() never allocates and can therefore run after the atomic binder commit
// without adding a failure edge. Observations are later offline candidates.
class NativeBringupCoverageObservations final {
  public:
    using EventIndex = std::uint32_t;
    static constexpr EventIndex invalid_event_index = ~EventIndex{0u};

    NativeBringupCoverageObservations();

    // Returns a stable index until clear(). Callers that already authenticated
    // the same immutable observation may use record_cached() to avoid hashing
    // its SHA-256 identity strings on every hot dispatch.
    EventIndex record(
        const NativeBringupCoverageObservation& observation) noexcept;
    [[nodiscard]] bool record_cached(
        EventIndex index,
        const NativeBringupCoverageObservation& observation) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::uint64_t total_occurrences() const noexcept;
    [[nodiscard]] std::uint64_t dropped_events() const noexcept;
    [[nodiscard]] std::span<const NativeBringupCoverageObservation>
    events() const noexcept;
    [[nodiscard]] std::string serialize_json() const;

  private:
    std::vector<NativeBringupCoverageObservation> events_;
    std::vector<std::uint32_t> event_index_;
    std::uint64_t total_occurrences_ = 0u;
    std::uint64_t dropped_events_ = 0u;
};

class NativeBringupCoverageDispatchContext final {
  public:
    static constexpr bool release_eligible = false;

    // Pack/span/string backing and binder/table owners must outlive this view.
    const NativeBringupCoverageDispatchPack& pack;
    std::uint64_t runtime_generation = 0u;
    NativeBringupCoverageObservations& observations;

    [[nodiscard]] bool validated_view_current(
        const RuntimeBlockTable& table,
        const NativePortLoadedAotBinder& binder) const noexcept;

  private:
    NativeBringupCoverageDispatchContext(
        const RuntimeBlockTable& validated_table,
        NativePortLoadedAotBinder& validated_binder,
        const NativeBringupCoverageDispatchPack& validated_pack,
        std::uint64_t active_runtime_generation,
        NativeBringupCoverageObservations& active_observations) noexcept;

    const NativeBringupCoverageSourceTransfer* validated_sources_data_ =
        nullptr;
    std::size_t validated_sources_size_ = 0u;
    const NativeBringupCoverageEntry* validated_entries_data_ = nullptr;
    std::size_t validated_entries_size_ = 0u;
    NativeBringupCoveragePackIdentity validated_identity_;
    const RuntimeBlockTable* validated_table_ = nullptr;
    const NativePortLoadedAotBinder* validated_binder_ = nullptr;
    std::uint64_t validated_table_lifetime_ = 0u;
    std::uint64_t validated_table_generation_ = 0u;

    friend NativeBringupCoverageDispatchContext
    make_native_bringup_coverage_dispatch_context(
        const RuntimeBlockTable&,
        NativePortLoadedAotBinder&,
        const NativeBringupCoverageDispatchPack&,
        std::uint64_t,
        NativeBringupCoverageObservations&);
};

[[nodiscard]] NativeBringupCoverageDispatchContext
make_native_bringup_coverage_dispatch_context(
    const RuntimeBlockTable& table,
    NativePortLoadedAotBinder& binder,
    const NativeBringupCoverageDispatchPack& pack,
    std::uint64_t runtime_generation,
    NativeBringupCoverageObservations& observations);

struct NativeBringupCoveragePreflightRequest final {
    NativeBringupTransferKind transfer_kind =
        NativeBringupTransferKind::TailJumpRegister;
    std::uint32_t callsite = 0u;
    std::uint32_t target = 0u;
    std::uint32_t continuation = 0u;
    BlockAddress source;
    BlockVariantKey variant;
    enum class TargetHook : std::uint8_t {
        None,
        CallableFunctionEntry,
        ConflictingInstruction,
    };
    // This classification is emitted from the sealed native-hook table.  A
    // callable FunctionEntry is already the authoritative execution target;
    // an Instruction hook must never inherit call/jump admission merely
    // because its containing Static-AOT function is present.
    TargetHook target_hook = TargetHook::None;
};

struct NativeBringupCoveragePreflightResult final {
    RuntimeBlockHandle block;
    ValidatedBlockExecution execution;
    std::uint32_t target = 0u;
    std::uint32_t physical_target = 0u;
    std::uint64_t lifecycle_generation = 0u;
    bool cache_hit = false;
};

[[nodiscard]] NativeBringupCoveragePreflightResult
preflight_native_bringup_coverage_dispatch(
    const RuntimeBlockTable& table,
    NativePortLoadedAotBinder& binder,
    const NativeBringupCoverageDispatchContext& context,
    const NativeBringupCoveragePreflightRequest& request);

} // namespace katana::runtime

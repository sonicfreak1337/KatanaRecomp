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
class NativePortRuntimeImageBindings;

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
    RuntimeImageInactive,
    RuntimeImageIdentityMismatch,
    HookReplacementConflict,
    AmbiguousTargetOwner,
    TargetCapabilityMismatch
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
inline constexpr std::uint32_t native_bringup_coverage_contract_version = 6u;

enum class NativeBringupCoverageSourceKind : std::uint8_t {
    StaticAot,
    LoadedAot,
    RuntimeImage,
};

enum class NativeBringupCoverageOwnerKind : std::uint8_t {
    NativeFunctionEntry,
    PrimaryStatic,
    RuntimeImage,
    LoadedAot,
};

enum class NativeBringupCoverageTargetCapability : std::uint8_t {
    None = 0u,
    Callable = 1u << 0u,
    TailJumpEntry = 1u << 1u,
    InternalBlock = 1u << 2u,
    ResumeOnly = 1u << 3u,
};

[[nodiscard]] constexpr NativeBringupCoverageTargetCapability operator|(
    const NativeBringupCoverageTargetCapability left,
    const NativeBringupCoverageTargetCapability right) noexcept {
    return static_cast<NativeBringupCoverageTargetCapability>(
        static_cast<std::uint8_t>(left) |
        static_cast<std::uint8_t>(right));
}

[[nodiscard]] constexpr bool native_bringup_coverage_has_capability(
    const NativeBringupCoverageTargetCapability capabilities,
    const NativeBringupCoverageTargetCapability required) noexcept {
    return (static_cast<std::uint8_t>(capabilities) &
            static_cast<std::uint8_t>(required)) != 0u;
}

struct NativeBringupCoveragePackIdentity final {
    std::uint32_t contract_version =
        native_bringup_coverage_contract_version;
    std::string_view authority_identity;
    std::string_view project_id;
    std::string_view project_version;
    std::string_view analysis_identity;
    std::string_view aot_pack_identity;
    std::string_view module_universe_identity;
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
    // LoadedAot/RuntimeImage SHA-256. StaticAot requires the empty tuple.
    std::string_view source_module_identity;
    // RuntimeImage descriptor ID. Other source kinds require empty.
    std::string_view source_image_id;
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

// Movable executable ingress authority is distinct from the optional
// fixed-placement accelerator above. Every record names one externally
// admissible RuntimeImage/LoadedAot entry, or an optional narrowed
// PrimaryStatic capability. An exact sealed PrimaryStatic block with no
// declared authority at its source address is self-authenticating for the
// current CallRegister/TailJumpRegister transfer only. A zero runtime_start is
// legal only for a loader-placed LoadedAot module; its live runtime address is
// then supplied by the identity-/generation-bound binder.
struct NativeBringupCoverageTargetAuthority final {
    NativeBringupCoverageOwnerKind owner_kind =
        NativeBringupCoverageOwnerKind::PrimaryStatic;
    NativeBringupCoverageTargetCapability capabilities =
        NativeBringupCoverageTargetCapability::None;
    std::string_view module_identity;
    std::string_view image_id;
    std::uint32_t module_size = 0u;
    std::uint32_t source_start = 0u;
    std::uint32_t runtime_start = 0u;
    std::uint32_t module_relative_offset = 0u;
    NativeBringupDispatchStaticAotBinding target;
};

struct NativeBringupCoverageDispatchPack final {
    NativeBringupCoveragePackIdentity identity;
    // Both spans are optional lookup/proof accelerators. Neither grants
    // executable ingress authority.
    std::span<const NativeBringupCoverageSourceTransfer> source_transfers;
    std::span<const NativeBringupCoverageEntry> entries;
    // Optional narrowing evidence for movable and PrimaryStatic owners.
    // PrimaryStatic records must match the sealed RuntimeBlockTable. Movable
    // records instead match the complete generated dispatcher entry plus the
    // current exact lifecycle binding; requiring them in the smaller sealed
    // table would turn an export reachability subset into a second allowlist.
    // Missing records never make an otherwise exact active
    // RuntimeImage/Loaded-AOT block unavailable. Matching records can still
    // narrow capabilities, and conflicting active owners remain fail-closed.
    std::span<const NativeBringupCoverageTargetAuthority> target_authorities;
};

inline constexpr std::size_t
    // Whole-game NativeBringup coverage aggregates unresolved source blocks
    // across every prepared Loaded-AOT module. This is therefore an aggregate
    // budget, not the historical per-module block ceiling.
    native_bringup_coverage_maximum_source_transfers = 262'144u;
inline constexpr std::size_t native_bringup_coverage_maximum_entries =
    16'384u;
inline constexpr std::size_t
    native_bringup_coverage_maximum_target_authorities = 65'536u;

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
    std::string_view source_image_id;
    NativeBringupCoverageOwnerKind target_owner_kind =
        NativeBringupCoverageOwnerKind::PrimaryStatic;
    std::string_view target_image_id;
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

    // Coverage observations are diagnostic-only. Product runners may disable
    // their hot-path journal while retaining the exact same executable
    // admission/preflight semantics.
    void set_recording_enabled(bool enabled);
    [[nodiscard]] bool recording_enabled() const noexcept;

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
        const NativePortRuntimeImageBindings& runtime_images,
        const NativePortLoadedAotBinder& binder) const noexcept;

  private:
    NativeBringupCoverageDispatchContext(
        const RuntimeBlockTable& validated_table,
        NativePortRuntimeImageBindings& validated_runtime_images,
        NativePortLoadedAotBinder& validated_binder,
        const NativeBringupCoverageDispatchPack& validated_pack,
        std::uint64_t active_runtime_generation,
        NativeBringupCoverageObservations& active_observations) noexcept;

    const NativeBringupCoverageSourceTransfer* validated_sources_data_ =
        nullptr;
    std::size_t validated_sources_size_ = 0u;
    const NativeBringupCoverageEntry* validated_entries_data_ = nullptr;
    std::size_t validated_entries_size_ = 0u;
    const NativeBringupCoverageTargetAuthority*
        validated_target_authorities_data_ = nullptr;
    std::size_t validated_target_authorities_size_ = 0u;
    NativeBringupCoveragePackIdentity validated_identity_;
    const RuntimeBlockTable* validated_table_ = nullptr;
    const NativePortRuntimeImageBindings* validated_runtime_images_ = nullptr;
    const NativePortLoadedAotBinder* validated_binder_ = nullptr;
    std::uint64_t validated_table_lifetime_ = 0u;
    std::uint64_t validated_table_generation_ = 0u;

    friend NativeBringupCoverageDispatchContext
    make_native_bringup_coverage_dispatch_context(
        const RuntimeBlockTable&,
        NativePortRuntimeImageBindings&,
        NativePortLoadedAotBinder&,
        const NativeBringupCoverageDispatchPack&,
        std::uint64_t,
        NativeBringupCoverageObservations&);
};

[[nodiscard]] NativeBringupCoverageDispatchContext
make_native_bringup_coverage_dispatch_context(
    const RuntimeBlockTable& table,
    NativePortRuntimeImageBindings& runtime_images,
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
    NativeBringupCoverageOwnerKind owner_kind =
        NativeBringupCoverageOwnerKind::PrimaryStatic;
    NativeBringupCoverageTargetCapability capabilities =
        NativeBringupCoverageTargetCapability::None;
    std::string_view owner_identity;
    std::string_view owner_image_id;
    std::string_view block_identity;
    std::uint32_t dispatch_source = 0u;
    // Movable title code already has two exact authorities that do not depend
    // on a reachability export: the active RuntimeImage/Loaded-AOT lifecycle
    // owns the resident bytes, and the generated dispatcher owns an exact
    // source-address entry.  When that entry was not duplicated into the
    // much smaller sealed bring-up table, the generated dispatcher must bind
    // it directly after this preflight instead of treating the missing table
    // row as missing native code.
    bool generated_entry_required = false;
};

[[nodiscard]] NativeBringupCoveragePreflightResult
preflight_native_bringup_coverage_dispatch(
    const RuntimeBlockTable& table,
    NativePortRuntimeImageBindings& runtime_images,
    NativePortLoadedAotBinder& binder,
    const NativeBringupCoverageDispatchContext& context,
    const NativeBringupCoveragePreflightRequest& request);

} // namespace katana::runtime

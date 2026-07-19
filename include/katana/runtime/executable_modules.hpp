#pragma once

#include "katana/runtime/block_table.hpp"
#include "katana/runtime/code_invalidation.hpp"
#include "katana/runtime/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace katana::runtime {

class IndirectDispatchMetrics;

enum class ExecutableModuleKind : std::uint8_t { Module, Overlay };

inline constexpr std::uint32_t executable_module_relocation_module_base32 = 1u;

enum class ExecutableStorageRole : std::uint8_t {
    RuntimeMaterializable,
    ProvenData,
    ProvenPadding
};

struct ExecutableModuleRangeRole {
    std::uint32_t offset = 0u;
    std::uint32_t size = 0u;
    ExecutableStorageRole role = ExecutableStorageRole::RuntimeMaterializable;
};

struct ExecutableModuleRelocation {
    std::uint32_t offset = 0u;
    std::uint32_t type = 0u;
    std::int32_t addend = 0;
};

struct ExecutableModule {
    std::string id;
    std::string source_identity;
    std::uint32_t guest_start = 0u;
    std::vector<std::uint8_t> bytes;
    std::vector<ExecutableModuleRelocation> relocations;
    std::vector<ExecutableModuleRangeRole> range_roles;
    ExecutableModuleKind kind = ExecutableModuleKind::Module;
    std::uint64_t generation = 1u;
    std::uint64_t relocation_generation = 1u;
    bool executable_permission = true;
    bool writable = true;
    bool active = true;

    [[nodiscard]] std::uint64_t end_address() const noexcept;
    [[nodiscard]] bool contains(std::uint32_t address, std::size_t width = 1u) const noexcept;
    [[nodiscard]] bool materializable(std::uint32_t address, std::size_t width = 1u) const noexcept;
};

struct ExecutableModuleMetrics {
    std::uint64_t loads = 0u;
    std::uint64_t unloads = 0u;
    std::uint64_t replacements = 0u;
    std::uint64_t invalidated_blocks = 0u;
};

class ExecutableModuleCatalog final {
  public:
    void publish(ExecutableModule module);
    void
    replace(ExecutableModule module, RuntimeBlockTable& blocks, ExecutableCodeTracker& tracker);
    void unload(std::string_view id, RuntimeBlockTable& blocks, ExecutableCodeTracker& tracker);
    void update_relocations(std::string_view id,
                            std::vector<ExecutableModuleRelocation> relocations,
                            RuntimeBlockTable& blocks,
                            ExecutableCodeTracker& tracker);
    [[nodiscard]] const ExecutableModule* resolve(std::uint32_t address,
                                                  std::size_t width = 1u) const noexcept;
    [[nodiscard]] const ExecutableModule* find(std::string_view id) const noexcept;
    [[nodiscard]] bool
    validate_bytes(const Memory& memory, std::uint32_t address, std::size_t width) const;
    [[nodiscard]] const ExecutableModuleMetrics& metrics() const noexcept;

  private:
    std::vector<ExecutableModule> modules_;
    ExecutableModuleMetrics metrics_;
};

enum class MaterializationFailure : std::uint8_t {
    None,
    Disabled,
    Misaligned,
    Uncommitted,
    PermissionDenied,
    ProvenNonCode,
    UnknownSource,
    BudgetExhausted,
    RepeatedMissLimit,
    DecodeRejected,
    AnalysisIncomplete,
    IrVerificationFailed,
    CodeGenerationFailed,
    ByteIdentityMismatch,
    GenerationMismatch,
    ModuleUnloaded,
    RelocationMismatch,
    StaleHandle,
    InvalidBlock
};

struct BlockMaterializationPolicy {
    bool enabled = false;
    std::uint64_t max_blocks = 0u;
    std::uint64_t max_bytes = 0u;
    std::uint64_t max_guest_cycles = 1'000'000u;
    std::uint64_t max_instructions = 1'024u;
    std::uint64_t max_recursive_seeds = 64u;
    std::uint64_t max_analysis_time_ms = 1'000u;
    std::uint64_t max_memory_bytes = 16u * 1024u * 1024u;
    std::uint64_t max_materializations_per_run = 1'024u;
    std::uint64_t max_repeated_misses_per_target = 2u;
};

struct BlockMaterializationMetrics {
    std::uint64_t requests = 0u;
    std::uint64_t cache_hits = 0u;
    std::uint64_t materializations = 0u;
    std::uint64_t materialized_bytes = 0u;
    std::uint64_t misses = 0u;
    std::uint64_t budget_failures = 0u;
    std::uint64_t generation_revalidation_failures = 0u;
    std::uint64_t byte_identity_failures = 0u;
    std::uint64_t dispatch_validation_failures = 0u;
    MaterializationFailure first_failure = MaterializationFailure::None;
    std::uint32_t first_failure_target = 0u;
};

struct MaterializedBlockCandidate {
    RuntimeBlock block;
    bool decode_candidate_validated = false;
    bool bounded_analysis_complete = false;
    bool ir_verified = false;
    bool code_generated = false;
    std::uint64_t guest_cycles = 0u;
    std::uint64_t instructions = 0u;
    std::uint64_t recursive_seeds = 0u;
    std::uint64_t analysis_time_ms = 0u;
    std::uint64_t peak_memory_bytes = 0u;
};

struct BlockMaterializationEvent {
    std::uint64_t sequence = 0u;
    std::uint32_t target = 0u;
    MaterializationFailure failure = MaterializationFailure::None;
    bool success = false;

    bool operator==(const BlockMaterializationEvent&) const = default;
};

using BlockMaterializeCallback = std::function<MaterializedBlockCandidate(
    std::uint32_t, std::span<const std::uint8_t>, const BlockVariantKey&)>;

class DemandBlockMaterializer final {
  public:
    DemandBlockMaterializer(ExecutableModuleCatalog& modules,
                            RuntimeBlockTable& blocks,
                            ExecutableCodeTracker* tracker,
                            BlockMaterializationPolicy policy,
                            BlockMaterializeCallback callback);
    [[nodiscard]] std::optional<RuntimeBlockHandle> try_materialize(CpuState& cpu,
                                                                    std::uint32_t target,
                                                                    const BlockVariantKey& variant,
                                                                    std::uint32_t callsite);
    [[nodiscard]] const BlockMaterializationMetrics& metrics() const noexcept;
    [[nodiscard]] const std::vector<BlockMaterializationEvent>& events() const noexcept;
    [[nodiscard]] MaterializationFailure last_failure() const noexcept;
    [[nodiscard]] bool validate_for_dispatch(const CpuState& cpu,
                                             RuntimeBlockHandle handle,
                                             std::uint32_t target) noexcept;
    void record_invalidation(std::uint32_t address,
                             std::size_t size,
                             IndirectDispatchMetrics& dispatch_metrics);

  private:
    void fail(MaterializationFailure failure, std::uint32_t target) noexcept;
    void record_success(std::uint32_t target) noexcept;
    ExecutableModuleCatalog& modules_;
    RuntimeBlockTable& blocks_;
    ExecutableCodeTracker* tracker_ = nullptr;
    BlockMaterializationPolicy policy_;
    BlockMaterializeCallback callback_;
    BlockMaterializationMetrics metrics_;
    MaterializationFailure last_failure_ = MaterializationFailure::None;
    std::uint64_t next_event_sequence_ = 1u;
    std::vector<BlockMaterializationEvent> events_;
    std::map<std::uint32_t, std::uint64_t> misses_by_target_;
    struct MaterializedOrigin {
        std::uint32_t address = 0u;
        std::uint32_t size = 0u;
        std::uint32_t callsite = 0u;
        std::string module_id;
        std::string source_identity;
        std::uint64_t module_generation = 0u;
        std::uint64_t relocation_generation = 0u;
        RuntimeBlockHandle handle;
        std::vector<std::uint8_t> snapshot;
    };
    std::vector<MaterializedOrigin> origins_;
};

[[nodiscard]] const char* executable_module_kind_name(ExecutableModuleKind value) noexcept;
[[nodiscard]] const char* executable_storage_role_name(ExecutableStorageRole value) noexcept;
[[nodiscard]] const char* materialization_failure_name(MaterializationFailure value) noexcept;
[[nodiscard]] std::string
format_block_materialization_metrics_json(const BlockMaterializationMetrics& metrics,
                                          std::span<const BlockMaterializationEvent> events = {},
                                          bool include_local_details = false);

} // namespace katana::runtime

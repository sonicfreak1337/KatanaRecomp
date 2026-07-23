#pragma once

#include "katana/runtime/block_abi.hpp"

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace katana::runtime {

class ExecutableCodeTracker;

using BackendBlockFunction = BlockExit (*)(CpuState&, BlockExecutionContext&);

struct BlockVariantKey {
    std::uint64_t address_space_generation = 0u;
    std::uint64_t mmu_generation = 0u;
    std::uint64_t watchpoint_generation = 0u;
    std::uint32_t fpscr_mode = 0u;
    std::uint64_t runtime_generation = 0u;

    [[nodiscard]] auto operator<=>(const BlockVariantKey&) const noexcept = default;
};

struct RuntimeAotTemplateContract {
    CodeAddressMapping mapping;
    // Validation starts at the physical byte corresponding to mapping.runtime_start
    // (derived from the block's physical origin).  It may be larger than the mapped
    // executable range so writes to an adjacent literal pool invalidate every native
    // block backed by the template as well.
    std::uint32_t validation_extent = 0u;

    [[nodiscard]] bool operator==(const RuntimeAotTemplateContract&) const noexcept = default;
};

struct RuntimeBlock {
    std::uint32_t virtual_start = 0u;
    std::uint32_t physical_origin = 0u;
    std::uint32_t size = 0u;
    BlockEndKind end_kind = BlockEndKind::Fallthrough;
    BlockVariantKey variant;
    BackendBlockFunction function = nullptr;
    std::string provenance;
    bool runtime_registered = false;
    std::optional<RuntimeAotTemplateContract> aot_template;
};

struct RuntimeBlockHandle {
    std::uint64_t id = 0u;
    std::uint64_t generation = 0u;

    [[nodiscard]] explicit operator bool() const noexcept {
        return id != 0u;
    }
    [[nodiscard]] auto operator<=>(const RuntimeBlockHandle&) const noexcept = default;
};

enum class RuntimeBlockLookupMode : std::uint8_t { Direct, ReferenceTree };
enum class RuntimeBlockDispatchState : std::uint8_t {
    StaticCompiled,
    RuntimeMaterialized,
    Rejected
};

struct RuntimeBlockDispatchStatus {
    RuntimeBlockDispatchState state = RuntimeBlockDispatchState::Rejected;
    std::uint64_t generation = 0u;
    std::optional<RuntimeBlockHandle> handle;
};

struct RuntimeBlockLookupCounters {
    std::uint64_t direct_probes = 0u;
    std::uint64_t reference_probes = 0u;

    [[nodiscard]] bool operator==(const RuntimeBlockLookupCounters&) const = default;
};

struct RuntimeBlockRecordSnapshot {
    RuntimeBlockHandle handle;
    std::uint32_t virtual_start = 0u;
    std::uint32_t physical_origin = 0u;
    std::uint32_t size = 0u;
    BlockEndKind end_kind = BlockEndKind::Fallthrough;
    BlockVariantKey variant;
    std::string identity;
    std::string provenance;
    bool runtime_registered = false;
    bool active = false;
    bool static_block = false;
    std::optional<RuntimeAotTemplateContract> aot_template;

    [[nodiscard]] bool operator==(const RuntimeBlockRecordSnapshot&) const = default;
};

struct RuntimeBlockRejectionSnapshot {
    std::uint32_t virtual_address = 0u;
    BlockVariantKey variant;
    std::uint64_t generation = 0u;

    [[nodiscard]] bool operator==(const RuntimeBlockRejectionSnapshot&) const = default;
};

struct RuntimeBlockTableSnapshot {
    std::vector<RuntimeBlockRecordSnapshot> records;
    std::vector<RuntimeBlockRejectionSnapshot> rejected;
    std::uint64_t next_id = 1u;
    std::size_t active_count = 0u;
    bool static_sealed = false;
    bool code_tracker_bound = false;
    RuntimeBlockLookupMode lookup_mode = RuntimeBlockLookupMode::Direct;
    RuntimeBlockLookupCounters lookup_counters;

    [[nodiscard]] bool operator==(const RuntimeBlockTableSnapshot&) const = default;
};

[[nodiscard]] std::uint32_t canonical_physical_address(std::uint32_t address) noexcept;
[[nodiscard]] std::string stable_runtime_block_identity(const RuntimeBlock& block);
[[nodiscard]] BlockExit execute_runtime_block(const RuntimeBlock& block,
                                              CpuState& cpu,
                                              BlockExecutionContext& context);

class RuntimeBlockTable {
  public:
    [[nodiscard]] RuntimeBlockHandle register_static(RuntimeBlock block);
    [[nodiscard]] std::optional<RuntimeBlockHandle>
    register_static_variant(std::uint32_t virtual_address,
                            std::uint32_t physical_address,
                            const BlockVariantKey& source_variant,
                            const BlockVariantKey& target_variant);
    [[nodiscard]] std::vector<RuntimeBlockHandle>
    register_static_bulk(std::vector<RuntimeBlock> blocks);
    void seal_static() noexcept;
    [[nodiscard]] RuntimeBlockHandle register_bootstrap_static(RuntimeBlock block);
    [[nodiscard]] RuntimeBlockHandle register_runtime(RuntimeBlock block);
    [[nodiscard]] std::optional<RuntimeBlockHandle>
    lookup(std::uint32_t virtual_address, const BlockVariantKey& variant) const noexcept;
    [[nodiscard]] std::optional<RuntimeBlockHandle>
    lookup_physical(std::uint32_t physical_address, const BlockVariantKey& variant) const noexcept;
    [[nodiscard]] std::vector<RuntimeBlockHandle> aliases(std::uint32_t physical_origin) const;
    [[nodiscard]] std::optional<std::reference_wrapper<const RuntimeBlock>>
    resolve(RuntimeBlockHandle handle) const noexcept;
    [[nodiscard]] bool active(RuntimeBlockHandle handle) const noexcept;
    [[nodiscard]] RuntimeBlockDispatchStatus
    dispatch_status(std::uint32_t virtual_address, const BlockVariantKey& variant) const noexcept;
    void mark_rejected(std::uint32_t virtual_address,
                       const BlockVariantKey& variant) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] RuntimeBlockLookupMode lookup_mode() const noexcept;
    void set_lookup_mode(RuntimeBlockLookupMode mode) noexcept;
    [[nodiscard]] const RuntimeBlockLookupCounters& lookup_counters() const noexcept;
    [[nodiscard]] RuntimeBlockTableSnapshot snapshot() const;
    void reset_lookup_counters() const noexcept;
    [[nodiscard]] bool erase_identity(const std::string& block_identity) noexcept;
    [[nodiscard]] std::size_t erase_overlapping_physical(std::uint32_t physical_address,
                                                         std::size_t size) noexcept;
    void bind_code_tracker(const ExecutableCodeTracker* tracker) noexcept;
    void clear() noexcept;

  private:
    friend class ExecutableDiscLoadTransactionCoordinator;
    struct PreparedDiscLoadInvalidation {
        std::vector<std::uint64_t> ids;
    };
    [[nodiscard]] PreparedDiscLoadInvalidation
    prepare_disc_load_invalidation(std::uint32_t physical_address, std::size_t size) const;
    [[nodiscard]] std::size_t
    commit_disc_load_invalidation(PreparedDiscLoadInvalidation plan) noexcept;

    struct VariantAddressKey {
        BlockVariantKey variant;
        std::uint32_t address = 0u;
        [[nodiscard]] auto operator<=>(const VariantAddressKey&) const noexcept = default;
    };
    struct VariantAddressHash {
        [[nodiscard]] std::size_t operator()(const VariantAddressKey& key) const noexcept;
    };
    struct PhysicalLookupKey {
        BlockVariantKey variant;
        std::uint32_t physical = 0u;
        std::uint32_t virtual_start = 0u;
        [[nodiscard]] auto operator<=>(const PhysicalLookupKey&) const noexcept = default;
    };
    struct Record {
        RuntimeBlock block;
        std::string identity;
        std::uint64_t generation = 1u;
        bool active = true;
        bool static_block = false;
    };

    using VirtualIndex = std::map<VariantAddressKey, std::uint64_t>;
    using DirectVirtualIndex =
        std::unordered_map<VariantAddressKey, std::uint64_t, VariantAddressHash>;
    using PhysicalIndex = std::map<PhysicalLookupKey, std::uint64_t>;
    using AliasIndex = std::map<std::uint32_t, std::set<std::uint64_t>>;

    [[nodiscard]] RuntimeBlockHandle insert(RuntimeBlock block, bool runtime_registered);
    [[nodiscard]] bool dispatchable(const Record& record) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    overlapping_active_virtual(const RuntimeBlock& block,
                               std::uint64_t ignored_id = 0u) const noexcept;
    void index_active(std::uint64_t id, const Record& record);
    void deactivate(std::uint64_t id) noexcept;
    [[nodiscard]] std::optional<RuntimeBlockHandle>
    lookup_index(const VirtualIndex& index,
                 std::uint32_t virtual_address,
                 const BlockVariantKey& variant) const noexcept;
    [[nodiscard]] std::optional<RuntimeBlockHandle>
    lookup_direct_index(const DirectVirtualIndex& index,
                        std::uint32_t virtual_address,
                        const BlockVariantKey& variant) const noexcept;
    [[nodiscard]] std::optional<RuntimeBlockHandle>
    lookup_physical_index(const PhysicalIndex& index,
                          std::uint32_t physical_address,
                          const BlockVariantKey& variant) const noexcept;

    std::map<std::uint64_t, Record> records_;
    std::map<std::string, std::uint64_t> identities_;
    VirtualIndex active_virtual_ranges_;
    VirtualIndex static_virtual_index_;
    VirtualIndex dynamic_virtual_index_;
    DirectVirtualIndex static_direct_virtual_index_;
    DirectVirtualIndex dynamic_direct_virtual_index_;
    PhysicalIndex static_physical_index_;
    PhysicalIndex dynamic_physical_index_;
    AliasIndex static_alias_index_;
    AliasIndex dynamic_alias_index_;
    std::map<std::uint32_t, std::set<std::uint64_t>> active_physical_pages_;
    std::uint64_t next_id_ = 1u;
    std::size_t active_count_ = 0u;
    bool static_sealed_ = false;
    const ExecutableCodeTracker* code_tracker_ = nullptr;
    RuntimeBlockLookupMode lookup_mode_ = RuntimeBlockLookupMode::Direct;
    mutable RuntimeBlockLookupCounters lookup_counters_;
    mutable std::map<VariantAddressKey, std::uint64_t> rejected_generations_;
};

} // namespace katana::runtime

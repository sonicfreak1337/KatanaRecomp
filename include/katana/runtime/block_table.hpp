#pragma once

#include "katana/runtime/block_abi.hpp"

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
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

struct RuntimeBlock {
    std::uint32_t virtual_start = 0u;
    std::uint32_t physical_origin = 0u;
    std::uint32_t size = 0u;
    BlockEndKind end_kind = BlockEndKind::Fallthrough;
    BlockVariantKey variant;
    BackendBlockFunction function = nullptr;
    std::string provenance;
    bool runtime_registered = false;
};

struct RuntimeBlockHandle {
    std::uint64_t id = 0u;
    std::uint64_t generation = 0u;

    [[nodiscard]] explicit operator bool() const noexcept { return id != 0u; }
    [[nodiscard]] auto operator<=>(const RuntimeBlockHandle&) const noexcept = default;
};

[[nodiscard]] std::uint32_t canonical_physical_address(std::uint32_t address) noexcept;
[[nodiscard]] std::string stable_runtime_block_identity(const RuntimeBlock& block);

class RuntimeBlockTable {
  public:
    [[nodiscard]] RuntimeBlockHandle register_static(RuntimeBlock block);
    [[nodiscard]] std::vector<RuntimeBlockHandle>
    register_static_bulk(std::vector<RuntimeBlock> blocks);
    void seal_static() noexcept;
    [[nodiscard]] RuntimeBlockHandle register_runtime(RuntimeBlock block);
    [[nodiscard]] std::optional<RuntimeBlockHandle>
    lookup(std::uint32_t virtual_address, const BlockVariantKey& variant) const noexcept;
    [[nodiscard]] std::optional<RuntimeBlockHandle>
    lookup_physical(std::uint32_t physical_address,
                    const BlockVariantKey& variant) const noexcept;
    [[nodiscard]] std::vector<RuntimeBlockHandle>
    aliases(std::uint32_t physical_origin) const;
    [[nodiscard]] std::optional<std::reference_wrapper<const RuntimeBlock>>
    resolve(RuntimeBlockHandle handle) const noexcept;
    [[nodiscard]] bool active(RuntimeBlockHandle handle) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool erase_identity(const std::string& block_identity) noexcept;
    [[nodiscard]] std::size_t erase_overlapping_physical(std::uint32_t physical_address,
                                                         std::size_t size) noexcept;
    void bind_code_tracker(const ExecutableCodeTracker* tracker) noexcept;
    void clear() noexcept;

  private:
    struct VariantAddressKey {
        BlockVariantKey variant;
        std::uint32_t address = 0u;
        [[nodiscard]] auto operator<=>(const VariantAddressKey&) const noexcept = default;
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
    using PhysicalIndex = std::map<PhysicalLookupKey, std::uint64_t>;
    using AliasIndex = std::map<std::uint32_t, std::set<std::uint64_t>>;

    [[nodiscard]] RuntimeBlockHandle insert(RuntimeBlock block, bool runtime_registered);
    [[nodiscard]] bool dispatchable(const Record& record) const noexcept;
    [[nodiscard]] bool overlaps_active_virtual(const RuntimeBlock& block,
                                               std::uint64_t ignored_id = 0u) const noexcept;
    void index_active(std::uint64_t id, const Record& record);
    void deactivate(std::uint64_t id) noexcept;
    [[nodiscard]] std::optional<RuntimeBlockHandle>
    lookup_index(const VirtualIndex& index,
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
    PhysicalIndex static_physical_index_;
    PhysicalIndex dynamic_physical_index_;
    AliasIndex static_alias_index_;
    AliasIndex dynamic_alias_index_;
    std::map<std::uint32_t, std::set<std::uint64_t>> active_physical_pages_;
    std::uint64_t next_id_ = 1u;
    std::size_t active_count_ = 0u;
    bool static_sealed_ = false;
    const ExecutableCodeTracker* code_tracker_ = nullptr;
};

} // namespace katana::runtime

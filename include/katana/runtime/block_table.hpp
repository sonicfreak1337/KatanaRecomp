#pragma once

#include "katana/runtime/block_abi.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace katana::runtime {

using BackendBlockFunction = BlockExit (*)(CpuState&, BlockExecutionContext&);

struct BlockVariantKey {
    std::uint64_t address_space_generation = 0u;
    std::uint64_t mmu_generation = 0u;
    std::uint32_t fpscr_mode = 0u;
    std::uint64_t runtime_generation = 0u;

    [[nodiscard]] bool operator==(const BlockVariantKey&) const noexcept = default;
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

[[nodiscard]] std::uint32_t canonical_physical_address(std::uint32_t address) noexcept;
[[nodiscard]] std::string stable_runtime_block_identity(const RuntimeBlock& block);

class RuntimeBlockTable {
public:
    void register_static(RuntimeBlock block);
    void register_runtime(RuntimeBlock block);
    [[nodiscard]] const RuntimeBlock* lookup(
        std::uint32_t virtual_address,
        const BlockVariantKey& variant
    ) const noexcept;
    [[nodiscard]] std::vector<const RuntimeBlock*> aliases(
        std::uint32_t physical_origin
    ) const;
    [[nodiscard]] std::size_t size() const noexcept;
    void clear() noexcept;

private:
    void insert(RuntimeBlock block, bool runtime_registered);
    std::vector<RuntimeBlock> blocks_;
};

} // namespace katana::runtime

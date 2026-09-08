#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace katana::codegen::detail {

struct NativeDispatchShard {
    std::uint64_t id;
    std::size_t begin;
    std::size_t end;
};

// Sonic r318 added eleven dispatch rows and shifted 58 fixed-count shards.
// A binary address prefix names each shard independently of preceding rows.
// Only the affected prefix splits when it exceeds the existing row budget.
// Tree order also preserves the runtime's strictly sorted table contract.
template <typename Blocks>
[[nodiscard]] std::vector<NativeDispatchShard> native_dispatch_shards(
    const Blocks& blocks) {
    constexpr std::size_t maximum_rows = 8192u;
    std::vector<NativeDispatchShard> result;
    if (blocks.empty()) return result;
    for (std::size_t index = 1u; index < blocks.size(); ++index)
        if (blocks[index - 1u].address >= blocks[index].address)
            throw std::runtime_error("native-dispatch-shards-order");

    const auto append = [&](auto&& self, const std::size_t begin,
                            const std::size_t end, const std::uint64_t lower,
                            const std::uint64_t upper,
                            const std::uint64_t id) -> void {
        if (end - begin <= maximum_rows) {
            result.push_back({id, begin, end});
            return;
        }
        const auto middle = lower + (upper - lower) / 2u;
        const auto split = static_cast<std::size_t>(std::lower_bound(
            blocks.begin() + begin, blocks.begin() + end, middle,
            [](const auto& block, const std::uint64_t address) {
                return block.address < address;
            }) - blocks.begin());
        if (split != begin)
            self(self, begin, split, lower, middle, id * 2u);
        if (split != end)
            self(self, split, end, middle, upper, id * 2u + 1u);
    };
    append(append, 0u, blocks.size(), 0u, std::uint64_t{1u} << 32u, 1u);
    return result;
}

} // namespace katana::codegen::detail

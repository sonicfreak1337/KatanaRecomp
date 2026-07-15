#pragma once

#include "katana/runtime/memory.hpp"

#include <cstdint>
#include <memory>

namespace katana::runtime {

inline constexpr std::uint32_t sh4_cache_control_address = 0xFF00001Cu;

class Sh4CacheControl final {
public:
    static constexpr std::uint32_t instruction_invalidate = 0x00000800u;
    static constexpr std::uint32_t supported_write_mask = 0x000089AFu;

    [[nodiscard]] std::uint32_t value() const noexcept;
    [[nodiscard]] std::uint64_t instruction_invalidation_count() const noexcept;
    void write(std::uint32_t value);
    void reset() noexcept;

private:
    std::uint32_t value_ = 0u;
    std::uint64_t instruction_invalidations_ = 0u;
};

[[nodiscard]] std::shared_ptr<Sh4CacheControl> map_sh4_cache_control(
    Memory& memory
);

} // namespace katana::runtime

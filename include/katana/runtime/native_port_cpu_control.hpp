#pragma once

#include "katana/runtime/memory.hpp"

#include <cstdint>
#include <memory>

namespace katana::runtime {

class NativePortCpuControlDevice;

enum class NativePortOperandCacheOperation : std::uint8_t {
    Invalidate = 0u,
    Purge = 1u,
    WriteBack = 2u,
};

// Product-side SH-4 CPU-control compatibility.  This is deliberately not a
// cache, MMU or Dreamcast device model: statically recompiled code already
// executes from immutable host code and ordinary native RAM.  The state keeps
// only the guest-visible CCR configuration bits and turns cache-invalidation
// commands into host ordering barriers.  Cache arrays and on-chip RAM remain
// unmapped and therefore fail closed if reachable title code depends on them.
class NativePortCpuControl final {
  public:
    static constexpr std::uint32_t instruction_invalidate = 0x00000800u;
    static constexpr std::uint32_t operand_invalidate = 0x00000008u;
    static constexpr std::uint32_t command_mask =
        instruction_invalidate | operand_invalidate;
    static constexpr std::uint32_t supported_write_mask = 0x000089AFu;
    static constexpr std::uint32_t configuration_mask =
        supported_write_mask & ~command_mask;

    explicit NativePortCpuControl(std::uint32_t initial_value);

    [[nodiscard]] static constexpr bool
    valid_initial_value(const std::uint32_t value) noexcept {
        return (value & ~configuration_mask) == 0u;
    }

    [[nodiscard]] std::uint32_t value() const noexcept;
    [[nodiscard]] std::uint64_t
    instruction_invalidation_count() const noexcept;
    [[nodiscard]] std::uint64_t
    operand_invalidation_count() const noexcept;

    // Native AOT memory is host-coherent and executable code is immutable.
    // A guest SDK cache-range operation therefore has no cache-array state to
    // reproduce; it becomes one host ordering boundary after the title
    // provider has validated the affected guest range.
    void maintain_operand_range(NativePortOperandCacheOperation operation,
                                std::uint32_t address,
                                std::uint32_t size);

  private:
    friend class NativePortCpuControlDevice;
    friend std::shared_ptr<NativePortCpuControl>
    map_native_port_cpu_control(Memory&, std::uint32_t);

    void write(std::uint32_t value);

    std::uint32_t value_ = 0u;
    std::uint64_t instruction_invalidations_ = 0u;
    std::uint64_t operand_invalidations_ = 0u;
};

[[nodiscard]] std::shared_ptr<NativePortCpuControl>
map_native_port_cpu_control(Memory& memory, std::uint32_t initial_value);

} // namespace katana::runtime

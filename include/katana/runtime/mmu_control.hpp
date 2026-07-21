#pragma once

#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/memory.hpp"
#include "katana/runtime/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace katana::runtime {

inline constexpr std::uint32_t sh4_mmu_control_p4_base = 0xFF000000u;
inline constexpr std::uint32_t sh4_mmu_control_area7_base = 0x1F000000u;
inline constexpr std::size_t sh4_mmu_control_register_size = 0x14u;
inline constexpr std::uint32_t sh4_ptea_p4_address = 0xFF000034u;
inline constexpr std::uint32_t sh4_ptea_area7_address = 0x1F000034u;

class Sh4MmuControl final {
  public:
    Sh4MmuControl(CpuState& cpu, RuntimeAddressSpace& address_space) noexcept;
    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value);
    [[nodiscard]] std::uint32_t read_ptea() const noexcept;
    void write_ptea(std::uint32_t value) noexcept;
    void reset() noexcept;

  private:
    CpuState& cpu_;
    RuntimeAddressSpace& address_space_;
};

[[nodiscard]] std::shared_ptr<Sh4MmuControl>
map_sh4_mmu_control(Memory& memory, CpuState& cpu, RuntimeAddressSpace& address_space);

} // namespace katana::runtime

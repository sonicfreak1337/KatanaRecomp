#pragma once

#include "katana/runtime/memory.hpp"

#include <array>
#include <cstdint>
#include <memory>

namespace katana::runtime {

inline constexpr std::uint32_t sh4_cache_control_address = 0xFF00001Cu;
inline constexpr std::uint32_t sh4_instruction_cache_address_array = 0xF0000000u;
inline constexpr std::uint32_t sh4_instruction_cache_data_array = 0xF1000000u;
inline constexpr std::uint32_t sh4_operand_cache_address_array = 0xF4000000u;
inline constexpr std::uint32_t sh4_operand_cache_data_array = 0xF5000000u;
inline constexpr std::uint32_t sh4_cache_array_aperture_size = 0x01000000u;
inline constexpr std::uint32_t sh4_on_chip_ram_address = 0x7C000000u;
inline constexpr std::uint32_t sh4_on_chip_ram_aperture_size = 0x04000000u;
inline constexpr std::size_t sh4_on_chip_ram_size = 8u * 1024u;

class Sh4CacheControl final {
  public:
    static constexpr std::uint32_t instruction_invalidate = 0x00000800u;
    static constexpr std::uint32_t operand_ram_enable = 0x00000020u;
    static constexpr std::uint32_t operand_index_mode = 0x00000080u;
    static constexpr std::uint32_t supported_write_mask = 0x000089AFu;

    [[nodiscard]] std::uint32_t value() const noexcept;
    [[nodiscard]] std::uint64_t instruction_invalidation_count() const noexcept;
    [[nodiscard]] std::uint32_t read_instruction_address(std::uint32_t offset) const;
    [[nodiscard]] std::uint32_t read_operand_address(std::uint32_t offset) const;
    [[nodiscard]] std::uint32_t read_instruction_data(std::uint32_t offset) const;
    [[nodiscard]] std::uint32_t read_operand_data(std::uint32_t offset) const;
    [[nodiscard]] std::uint32_t
    read_on_chip_ram(std::uint32_t offset, MemoryAccessWidth width) const noexcept;
    void write_instruction_address(std::uint32_t offset, std::uint32_t value);
    void write_operand_address(std::uint32_t offset, std::uint32_t value);
    void write_instruction_data(std::uint32_t offset, std::uint32_t value);
    void write_operand_data(std::uint32_t offset, std::uint32_t value);
    void write_on_chip_ram(std::uint32_t offset,
                           std::uint32_t value,
                           MemoryAccessWidth width) noexcept;
    void write(std::uint32_t value);
    void reset() noexcept;

  private:
    std::uint32_t value_ = 0u;
    std::uint64_t instruction_invalidations_ = 0u;
    std::array<std::uint32_t, 256u> instruction_addresses_{};
    std::array<std::uint32_t, 512u> operand_addresses_{};
    std::array<std::uint8_t, 8u * 1024u> instruction_data_{};
    std::array<std::uint8_t, 16u * 1024u> operand_data_{};
    std::array<std::uint8_t, sh4_on_chip_ram_size> on_chip_ram_{};
};

[[nodiscard]] std::shared_ptr<Sh4CacheControl> map_sh4_cache_control(Memory& memory);

} // namespace katana::runtime

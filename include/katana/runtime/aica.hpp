#pragma once

#include "katana/runtime/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace katana::runtime {

inline constexpr std::uint32_t aica_register_physical_base = 0x00700000u;
inline constexpr std::size_t aica_register_size = 0x00008000u;
inline constexpr std::size_t aica_channel_count = 64u;
inline constexpr std::size_t aica_channel_register_stride = 0x80u;
inline constexpr std::uint32_t aica_common_register_base = 0x2800u;

class AicaRegisterFile final {
public:
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, MemoryAccessWidth width) const;
    void write(std::uint32_t offset, std::uint32_t value, MemoryAccessWidth width);
    void reset() noexcept;
    [[nodiscard]] std::uint64_t write_count() const noexcept;
private:
    [[nodiscard]] static std::size_t width_bytes(MemoryAccessWidth width) noexcept;
    void check(std::uint32_t offset, MemoryAccessWidth width) const;
    std::array<std::uint8_t, aica_register_size> registers_{};
    std::uint64_t writes_ = 0u;
};

[[nodiscard]] std::shared_ptr<AicaRegisterFile> map_aica_registers(Memory& memory);

} // namespace katana::runtime

#pragma once

#include "katana/runtime/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace katana::runtime {

inline constexpr std::uint32_t pvr_register_physical_base = 0x005F8000u;
inline constexpr std::size_t pvr_register_size = 0x200u;
inline constexpr std::uint32_t pvr_id = 0x17FD11DBu;
inline constexpr std::uint32_t pvr_revision = 0x00000011u;

namespace pvr_register {
inline constexpr std::uint32_t Id = 0x000u;
inline constexpr std::uint32_t Revision = 0x004u;
inline constexpr std::uint32_t SoftReset = 0x008u;
inline constexpr std::uint32_t StartRender = 0x014u;
inline constexpr std::uint32_t ParameterBase = 0x020u;
inline constexpr std::uint32_t RegionBase = 0x02Cu;
inline constexpr std::uint32_t BorderColor = 0x040u;
inline constexpr std::uint32_t FramebufferReadControl = 0x044u;
inline constexpr std::uint32_t FramebufferWriteControl = 0x048u;
inline constexpr std::uint32_t FramebufferReadSof1 = 0x050u;
inline constexpr std::uint32_t FramebufferReadSize = 0x05Cu;
inline constexpr std::uint32_t VideoControl = 0x0E8u;
}

class PvrRegisterFile final {
public:
    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value);
    void reset() noexcept;
    [[nodiscard]] std::uint64_t render_request_count() const noexcept;
    [[nodiscard]] std::uint64_t reset_count() const noexcept;
private:
    [[nodiscard]] static std::size_t index(std::uint32_t offset);
    std::array<std::uint32_t, pvr_register_size / 4u> registers_{};
    std::uint64_t render_requests_ = 0u;
    std::uint64_t resets_ = 0u;
};

[[nodiscard]] std::shared_ptr<PvrRegisterFile> map_pvr_registers(Memory& memory);

} // namespace katana::runtime

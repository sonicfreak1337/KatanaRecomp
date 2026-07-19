#pragma once

#include "katana/runtime/memory.hpp"
#include "katana/runtime/platform_interrupt.hpp"
#include "katana/runtime/scheduler.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace katana::runtime {

inline constexpr std::uint32_t system_asic_physical_base = 0x005F6900u;
inline constexpr std::uint32_t system_asic_register_size = 0x58u;
inline constexpr std::uint32_t system_bus_control_physical_base = 0x005F6800u;
inline constexpr std::uint32_t system_bus_control_register_size = 0xB0u;

namespace system_bus_register {
inline constexpr std::uint32_t Channel2Destination = 0x00u;
inline constexpr std::uint32_t Channel2Length = 0x04u;
inline constexpr std::uint32_t Channel2Start = 0x08u;
inline constexpr std::uint32_t SortStartAddress = 0x10u;
inline constexpr std::uint32_t SortBaseAddress = 0x14u;
inline constexpr std::uint32_t SortLinkWidth = 0x18u;
inline constexpr std::uint32_t SortAddressShift = 0x1Cu;
inline constexpr std::uint32_t SortStart = 0x20u;
inline constexpr std::uint32_t DbreqMask = 0x40u;
inline constexpr std::uint32_t BavlWaitCount = 0x44u;
inline constexpr std::uint32_t Channel2Priority = 0x48u;
inline constexpr std::uint32_t Channel2MaxBurst = 0x4Cu;
inline constexpr std::uint32_t SortDivider = 0x60u;
inline constexpr std::uint32_t TaFifoRemaining = 0x80u;
inline constexpr std::uint32_t TextureMemoryMode0 = 0x84u;
inline constexpr std::uint32_t TextureMemoryMode1 = 0x88u;
inline constexpr std::uint32_t FifoStatus = 0x8Cu;
inline constexpr std::uint32_t SystemReset = 0x90u;
inline constexpr std::uint32_t Revision = 0x9Cu;
inline constexpr std::uint32_t RootBusSplit = 0xA0u;
inline constexpr std::uint32_t BootReservedA4 = 0xA4u;
inline constexpr std::uint32_t BootReservedAc = 0xACu;
} // namespace system_bus_register

class DreamcastSystemBusControl final {
  public:
    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value);
    void reset() noexcept;
    [[nodiscard]] std::uint64_t system_reset_requests() const noexcept;

  private:
    [[nodiscard]] static std::size_t index(std::uint32_t offset);
    std::array<std::uint32_t, system_bus_control_register_size / 4u> registers_{};
    std::uint64_t system_reset_requests_ = 0u;
};

enum class SystemAsicEvent : std::uint16_t {
    PvrRenderDone = 0x0002u,
    PvrVblank = 0x0003u,
    MapleDma = 0x000Cu,
    GdromDma = 0x000Eu,
    AicaDma = 0x000Fu,
    GdromCommand = 0x0100u,
    AicaInterrupt = 0x0101u,
    GdromIllegalAddress = 0x020Cu,
    GdromOverrun = 0x020Du
};

struct SystemAsicEventRecord {
    std::uint64_t guest_cycle = 0u;
    std::uint64_t sequence = 0u;
    SystemAsicEvent event = SystemAsicEvent::PvrRenderDone;
};

class DreamcastSystemAsic final {
  public:
    explicit DreamcastSystemAsic(PlatformInterruptRouter& router) noexcept;
    void raise(SystemAsicEvent event, std::uint64_t guest_cycle);
    [[nodiscard]] SchedulerEventId
    schedule(EventScheduler& scheduler, SystemAsicEvent event, std::uint64_t guest_cycle);
    [[nodiscard]] std::uint32_t read(std::uint32_t offset) const;
    void write(std::uint32_t offset, std::uint32_t value);
    [[nodiscard]] const std::vector<SystemAsicEventRecord>& events() const noexcept;
    void reset() noexcept;

  private:
    void synchronize_lines();
    PlatformInterruptRouter& router_;
    std::array<std::uint32_t, 3u> pending_{};
    std::array<std::array<std::uint32_t, 3u>, 3u> masks_{};
    std::array<std::array<std::uint32_t, 2u>, 2u> dma_trigger_masks_{};
    std::vector<SystemAsicEventRecord> events_;
    std::uint64_t next_sequence_ = 1u;
    std::uint64_t last_guest_cycle_ = 0u;
};

[[nodiscard]] std::shared_ptr<DreamcastSystemAsic>
map_dreamcast_system_asic(Memory& memory, PlatformInterruptRouter& router);
[[nodiscard]] std::shared_ptr<DreamcastSystemBusControl>
map_dreamcast_system_bus_control(Memory& memory);

} // namespace katana::runtime

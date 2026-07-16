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
inline constexpr std::uint32_t system_asic_register_size = 0x40u;

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
    std::vector<SystemAsicEventRecord> events_;
    std::uint64_t next_sequence_ = 1u;
    std::uint64_t last_guest_cycle_ = 0u;
};

[[nodiscard]] std::shared_ptr<DreamcastSystemAsic>
map_dreamcast_system_asic(Memory& memory, PlatformInterruptRouter& router);

} // namespace katana::runtime

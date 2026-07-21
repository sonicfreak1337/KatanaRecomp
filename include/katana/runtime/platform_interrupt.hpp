#pragma once

#include "katana/runtime/dma.hpp"
#include "katana/runtime/interrupt.hpp"
#include "katana/runtime/timers.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace katana::runtime {

inline constexpr std::uint32_t sh4_intc_p4_base = 0xFFD00000u;
inline constexpr std::uint32_t sh4_intc_area7_base = 0x1FD00000u;
inline constexpr std::size_t sh4_intc_register_size = 0x14u;

enum class PlatformInterruptSource : std::uint32_t {
    ExternalIrl13 = 0x00000320u,
    ExternalIrl11 = 0x00000360u,
    ExternalIrl9 = 0x000003A0u,
    Tmu0 = 0x00000400u,
    Tmu1 = 0x00000420u,
    Tmu2 = 0x00000440u,
    RtcAlarm = 0x00000480u,
    RtcPeriodic = 0x000004A0u,
    RtcCarry = 0x000004C0u,
    Dma0 = 0x00000640u,
    Dma1 = 0x00000660u,
    Dma2 = 0x00000680u,
    Dma3 = 0x000006A0u,
    DmaError = 0x000006C0u,
};

class PlatformInterruptRouter final {
  public:
    static constexpr std::size_t external_line_count = 3u;

    PlatformInterruptRouter(InterruptController& controller,
                            Sh4Tmu& tmu,
                            Sh4Rtc& rtc,
                            Sh4Dmac& dmac) noexcept;

    void set_tmu_level(std::size_t channel, std::uint8_t level);
    void set_rtc_level(std::uint8_t level) noexcept;
    void set_dma_level(std::uint8_t level) noexcept;
    void set_external_pending(std::size_t line, bool pending);

    [[nodiscard]] std::uint8_t tmu_level(std::size_t channel) const;
    [[nodiscard]] std::uint8_t rtc_level() const noexcept;
    [[nodiscard]] std::uint8_t dma_level() const noexcept;
    [[nodiscard]] bool external_pending(std::size_t line) const;

    [[nodiscard]] std::size_t synchronize();
    [[nodiscard]] bool accept(CpuState& cpu);
    void reset() noexcept;

  private:
    static std::uint8_t clamp_level(std::uint8_t level) noexcept;
    static InterruptSource source_id(PlatformInterruptSource source) noexcept;
    void route(PlatformInterruptSource source, bool asserted, std::uint8_t level);

    InterruptController& controller_;
    Sh4Tmu& tmu_;
    Sh4Rtc& rtc_;
    Sh4Dmac& dmac_;
    std::array<std::uint8_t, Sh4Tmu::channel_count> tmu_levels_{};
    std::uint8_t rtc_level_ = 0u;
    std::uint8_t dma_level_ = 0u;
    std::array<bool, external_line_count> external_pending_{};
};

class Sh4InterruptRegisters final {
  public:
    explicit Sh4InterruptRegisters(PlatformInterruptRouter& router) noexcept;

    [[nodiscard]] std::uint16_t interrupt_control() const noexcept;
    [[nodiscard]] std::uint16_t priority_a() const noexcept;
    [[nodiscard]] std::uint16_t priority_b() const noexcept;
    [[nodiscard]] std::uint16_t priority_c() const noexcept;
    [[nodiscard]] std::uint16_t priority_d() const noexcept;

    void write_interrupt_control(std::uint16_t value) noexcept;
    void write_priority_a(std::uint16_t value) noexcept;
    void write_priority_b(std::uint16_t value) noexcept;
    void write_priority_c(std::uint16_t value) noexcept;
    void reset() noexcept;

  private:
    void synchronize_priorities() noexcept;
    PlatformInterruptRouter& router_;
    std::uint16_t interrupt_control_ = 0u;
    std::uint16_t priority_a_ = 0u;
    std::uint16_t priority_b_ = 0u;
    std::uint16_t priority_c_ = 0u;
};

[[nodiscard]] std::shared_ptr<Sh4InterruptRegisters>
map_sh4_interrupt_registers(Memory& memory, PlatformInterruptRouter& router);

} // namespace katana::runtime

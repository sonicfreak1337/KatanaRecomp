#include "katana/runtime/platform_interrupt.hpp"

#include <array>
#include <memory>
#include <stdexcept>

namespace katana::runtime {

namespace {

constexpr std::array<PlatformInterruptSource, Sh4Tmu::channel_count> tmu_sources = {
    PlatformInterruptSource::Tmu0,
    PlatformInterruptSource::Tmu1,
    PlatformInterruptSource::Tmu2,
};

constexpr std::array<PlatformInterruptSource, Sh4Dmac::channel_count> dma_sources = {
    PlatformInterruptSource::Dma0,
    PlatformInterruptSource::Dma1,
    PlatformInterruptSource::Dma2,
    PlatformInterruptSource::Dma3,
};

constexpr std::array<PlatformInterruptSource, PlatformInterruptRouter::external_line_count>
    external_sources = {
        PlatformInterruptSource::ExternalIrl13,
        PlatformInterruptSource::ExternalIrl11,
        PlatformInterruptSource::ExternalIrl9,
};

constexpr std::array<std::uint8_t, PlatformInterruptRouter::external_line_count> external_levels = {
    13u, 11u, 9u};

} // namespace

PlatformInterruptRouter::PlatformInterruptRouter(InterruptController& controller,
                                                 Sh4Tmu& tmu,
                                                 Sh4Rtc& rtc,
                                                 Sh4Dmac& dmac) noexcept
    : controller_(controller), tmu_(tmu), rtc_(rtc), dmac_(dmac) {}

std::uint8_t PlatformInterruptRouter::clamp_level(const std::uint8_t level) noexcept {
    return static_cast<std::uint8_t>(level > 15u ? 15u : level);
}

InterruptSource PlatformInterruptRouter::source_id(const PlatformInterruptSource source) noexcept {
    return static_cast<InterruptSource>(source);
}

void PlatformInterruptRouter::set_tmu_level(const std::size_t channel, const std::uint8_t level) {
    if (channel >= tmu_levels_.size()) {
        throw std::out_of_range("Ungueltiger TMU-Interruptkanal.");
    }
    tmu_levels_[channel] = clamp_level(level);
}

void PlatformInterruptRouter::set_rtc_level(const std::uint8_t level) noexcept {
    rtc_level_ = clamp_level(level);
}

void PlatformInterruptRouter::set_dma_level(const std::uint8_t level) noexcept {
    dma_level_ = clamp_level(level);
}

void PlatformInterruptRouter::set_external_pending(const std::size_t line, const bool pending) {
    if (line >= external_pending_.size()) {
        throw std::out_of_range("Ungueltige externe Interruptleitung.");
    }
    external_pending_[line] = pending;
}

std::uint8_t PlatformInterruptRouter::tmu_level(const std::size_t channel) const {
    if (channel >= tmu_levels_.size()) {
        throw std::out_of_range("Ungueltiger TMU-Interruptkanal.");
    }
    return tmu_levels_[channel];
}

std::uint8_t PlatformInterruptRouter::rtc_level() const noexcept {
    return rtc_level_;
}
std::uint8_t PlatformInterruptRouter::dma_level() const noexcept {
    return dma_level_;
}

bool PlatformInterruptRouter::external_pending(const std::size_t line) const {
    if (line >= external_pending_.size()) {
        throw std::out_of_range("Ungueltige externe Interruptleitung.");
    }
    return external_pending_[line];
}

void PlatformInterruptRouter::route(const PlatformInterruptSource source,
                                    const bool asserted,
                                    const std::uint8_t level) {
    const auto id = source_id(source);
    if (asserted) {
        controller_.request(id, level, static_cast<std::uint32_t>(source));
    } else {
        static_cast<void>(controller_.cancel(id));
    }
}

std::size_t PlatformInterruptRouter::synchronize() {
    std::size_t asserted = 0u;
    for (std::size_t channel = 0u; channel < tmu_sources.size(); ++channel) {
        const bool pending = tmu_.interrupt_pending(channel);
        route(tmu_sources[channel], pending, tmu_levels_[channel]);
        asserted += pending ? 1u : 0u;
    }
    const bool rtc_alarm = rtc_.alarm_interrupt_pending();
    const bool rtc_periodic = rtc_.periodic_interrupt_pending();
    const bool rtc_carry = rtc_.carry_interrupt_pending();
    route(PlatformInterruptSource::RtcAlarm, rtc_alarm, rtc_level_);
    route(PlatformInterruptSource::RtcPeriodic, rtc_periodic, rtc_level_);
    route(PlatformInterruptSource::RtcCarry, rtc_carry, rtc_level_);
    asserted += rtc_alarm ? 1u : 0u;
    asserted += rtc_periodic ? 1u : 0u;
    asserted += rtc_carry ? 1u : 0u;

    for (std::size_t channel = 0u; channel < dma_sources.size(); ++channel) {
        const bool pending = dmac_.interrupt_pending(channel);
        route(dma_sources[channel], pending, dma_level_);
        asserted += pending ? 1u : 0u;
    }
    const bool dma_error = dmac_.address_error();
    route(PlatformInterruptSource::DmaError, dma_error, dma_level_);
    asserted += dma_error ? 1u : 0u;

    for (std::size_t line = 0u; line < external_sources.size(); ++line) {
        route(external_sources[line], external_pending_[line], external_levels[line]);
        asserted += external_pending_[line] ? 1u : 0u;
    }
    return asserted;
}

bool PlatformInterruptRouter::accept(CpuState& cpu) {
    static_cast<void>(synchronize());
    return accept_pending_interrupt(cpu, controller_);
}

void PlatformInterruptRouter::reset() noexcept {
    for (const auto source : tmu_sources) {
        static_cast<void>(controller_.cancel(source_id(source)));
    }
    static_cast<void>(controller_.cancel(source_id(PlatformInterruptSource::RtcAlarm)));
    static_cast<void>(controller_.cancel(source_id(PlatformInterruptSource::RtcPeriodic)));
    static_cast<void>(controller_.cancel(source_id(PlatformInterruptSource::RtcCarry)));
    for (const auto source : dma_sources) {
        static_cast<void>(controller_.cancel(source_id(source)));
    }
    static_cast<void>(controller_.cancel(source_id(PlatformInterruptSource::DmaError)));
    for (const auto source : external_sources) {
        static_cast<void>(controller_.cancel(source_id(source)));
    }
    tmu_levels_ = {};
    rtc_level_ = 0u;
    dma_level_ = 0u;
    external_pending_ = {};
}

Sh4InterruptRegisters::Sh4InterruptRegisters(PlatformInterruptRouter& router) noexcept
    : router_(router) {
    synchronize_priorities();
}

std::uint16_t Sh4InterruptRegisters::interrupt_control() const noexcept {
    return interrupt_control_;
}
std::uint16_t Sh4InterruptRegisters::priority_a() const noexcept {
    return priority_a_;
}
std::uint16_t Sh4InterruptRegisters::priority_b() const noexcept {
    return priority_b_;
}
std::uint16_t Sh4InterruptRegisters::priority_c() const noexcept {
    return priority_c_;
}
std::uint16_t Sh4InterruptRegisters::priority_d() const noexcept {
    return 0u;
}

void Sh4InterruptRegisters::write_interrupt_control(const std::uint16_t value) noexcept {
    constexpr std::uint16_t writable_mask = 0x4380u;
    interrupt_control_ = value & writable_mask;
}

void Sh4InterruptRegisters::write_priority_a(const std::uint16_t value) noexcept {
    priority_a_ = value;
    synchronize_priorities();
}
void Sh4InterruptRegisters::write_priority_b(const std::uint16_t value) noexcept {
    priority_b_ = value;
}
void Sh4InterruptRegisters::write_priority_c(const std::uint16_t value) noexcept {
    priority_c_ = value;
    synchronize_priorities();
}

void Sh4InterruptRegisters::synchronize_priorities() noexcept {
    router_.set_tmu_level(0u, static_cast<std::uint8_t>((priority_a_ >> 12u) & 0xFu));
    router_.set_tmu_level(1u, static_cast<std::uint8_t>((priority_a_ >> 8u) & 0xFu));
    router_.set_tmu_level(2u, static_cast<std::uint8_t>((priority_a_ >> 4u) & 0xFu));
    router_.set_rtc_level(static_cast<std::uint8_t>(priority_a_ & 0xFu));
    router_.set_dma_level(static_cast<std::uint8_t>((priority_c_ >> 8u) & 0xFu));
}

void Sh4InterruptRegisters::reset() noexcept {
    interrupt_control_ = 0u;
    priority_a_ = 0u;
    priority_b_ = 0u;
    priority_c_ = 0u;
    synchronize_priorities();
}

std::shared_ptr<Sh4InterruptRegisters>
map_sh4_interrupt_registers(Memory& memory, PlatformInterruptRouter& router) {
    auto registers = std::make_shared<Sh4InterruptRegisters>(router);
    auto device = std::make_shared<MmioMemoryDevice>(
        sh4_intc_register_size,
        [registers](const std::uint32_t offset, const MemoryAccessWidth width) {
            if (width != MemoryAccessWidth::Halfword)
                throw std::runtime_error("INTC-Register erfordern 16-Bit-Zugriffe.");
            switch (offset) {
            case 0x00u:
                return static_cast<std::uint32_t>(registers->interrupt_control());
            case 0x04u:
                return static_cast<std::uint32_t>(registers->priority_a());
            case 0x08u:
                return static_cast<std::uint32_t>(registers->priority_b());
            case 0x0Cu:
                return static_cast<std::uint32_t>(registers->priority_c());
            case 0x10u:
                return static_cast<std::uint32_t>(registers->priority_d());
            default:
                throw std::runtime_error("Ungueltiger INTC-Registeroffset.");
            }
        },
        [registers](
            const std::uint32_t offset, const std::uint32_t value, const MemoryAccessWidth width) {
            if (width != MemoryAccessWidth::Halfword)
                throw std::runtime_error("INTC-Register erfordern 16-Bit-Zugriffe.");
            switch (offset) {
            case 0x00u:
                registers->write_interrupt_control(static_cast<std::uint16_t>(value));
                return;
            case 0x04u:
                registers->write_priority_a(static_cast<std::uint16_t>(value));
                return;
            case 0x08u:
                registers->write_priority_b(static_cast<std::uint16_t>(value));
                return;
            case 0x0Cu:
                registers->write_priority_c(static_cast<std::uint16_t>(value));
                return;
            case 0x10u:
                throw std::runtime_error("INTC-IPRD ist auf SH7750 read-only.");
            default:
                throw std::runtime_error("Ungueltiger INTC-Registeroffset.");
            }
        });
    memory.map_region("sh4-intc-p4", sh4_intc_p4_base, device);
    memory.map_region("sh4-intc-area7", sh4_intc_area7_base, device);
    return registers;
}

} // namespace katana::runtime

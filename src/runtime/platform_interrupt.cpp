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
        PlatformInterruptSource::ExternalLevel2,
        PlatformInterruptSource::ExternalLevel4,
        PlatformInterruptSource::ExternalLevel6,
    };

constexpr std::array<PlatformInterruptSource, PlatformInterruptRouter::scif_interrupt_count>
    scif_sources = {PlatformInterruptSource::ScifError,
                    PlatformInterruptSource::ScifReceive,
                    PlatformInterruptSource::ScifBreak,
                    PlatformInterruptSource::ScifTransmit};

constexpr std::array<std::uint8_t, PlatformInterruptRouter::external_line_count> external_levels = {
    2u, 4u, 6u};

} // namespace

PlatformInterruptRouter::PlatformInterruptRouter(InterruptController& controller,
                                                 Sh4Tmu& tmu,
                                                 Sh4Rtc& rtc,
                                                 Sh4Dmac& dmac) noexcept
    : controller_(controller), tmu_(tmu), rtc_(rtc), dmac_(dmac) {
    device_pending_state_ = device_pending_state();
}

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
    const auto clamped = clamp_level(level);
    if (tmu_levels_[channel] == clamped) return;
    tmu_levels_[channel] = clamped;
    ++source_epoch_;
}

void PlatformInterruptRouter::set_rtc_level(const std::uint8_t level) noexcept {
    const auto clamped = clamp_level(level);
    if (rtc_level_ == clamped) return;
    rtc_level_ = clamped;
    ++source_epoch_;
}

void PlatformInterruptRouter::set_dma_level(const std::uint8_t level) noexcept {
    const auto clamped = clamp_level(level);
    if (dma_level_ == clamped) return;
    dma_level_ = clamped;
    ++source_epoch_;
}
void PlatformInterruptRouter::set_scif_level(const std::uint8_t level) noexcept {
    const auto clamped = clamp_level(level);
    if (scif_level_ == clamped) return;
    scif_level_ = clamped;
    ++source_epoch_;
}
void PlatformInterruptRouter::set_scif_pending(const std::size_t source, const bool pending) {
    if (source >= scif_pending_.size())
        throw std::out_of_range("Ungueltige SH-4-SCIF-Interruptquelle.");
    if (scif_pending_[source] == pending) return;
    scif_pending_[source] = pending;
    ++source_epoch_;
}

void PlatformInterruptRouter::set_external_pending(const std::size_t line, const bool pending) {
    if (line >= external_pending_.size()) {
        throw std::out_of_range("Ungueltige externe Interruptleitung.");
    }
    if (external_pending_[line] == pending) return;
    external_pending_[line] = pending;
    ++source_epoch_;
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
std::uint8_t PlatformInterruptRouter::scif_level() const noexcept {
    return scif_level_;
}
bool PlatformInterruptRouter::scif_pending(const std::size_t source) const {
    if (source >= scif_pending_.size())
        throw std::out_of_range("Ungueltige SH-4-SCIF-Interruptquelle.");
    return scif_pending_[source];
}

bool PlatformInterruptRouter::external_pending(const std::size_t line) const {
    if (line >= external_pending_.size()) {
        throw std::out_of_range("Ungueltige externe Interruptleitung.");
    }
    return external_pending_[line];
}

PlatformInterruptRouterSnapshot PlatformInterruptRouter::snapshot() const noexcept {
    return {
        tmu_levels_,
        rtc_level_,
        dma_level_,
        scif_level_,
        scif_pending_,
        external_pending_,
    };
}

std::uint16_t PlatformInterruptRouter::device_pending_state() const noexcept {
    std::uint16_t state = 0u;
    const auto set = [&state](const std::size_t bit, const bool pending) {
        if (pending) state |= static_cast<std::uint16_t>(1u << bit);
    };
    for (std::size_t channel = 0u; channel < tmu_sources.size(); ++channel)
        set(channel, tmu_.interrupt_pending(channel));
    set(3u, rtc_.alarm_interrupt_pending());
    set(4u, rtc_.periodic_interrupt_pending());
    set(5u, rtc_.carry_interrupt_pending());
    for (std::size_t channel = 0u; channel < dma_sources.size(); ++channel)
        set(6u + channel, dmac_.interrupt_pending(channel));
    set(10u, dmac_.address_error());
    return state;
}

void PlatformInterruptRouter::refresh_device_source_epoch() const noexcept {
    if (!device_sources_dirty_) return;
    device_sources_dirty_ = false;
    const auto current = device_pending_state();
    if (current == device_pending_state_) return;
    device_pending_state_ = current;
    ++source_epoch_;
}

std::uint64_t PlatformInterruptRouter::source_epoch() const noexcept {
    refresh_device_source_epoch();
    return source_epoch_;
}

void PlatformInterruptRouter::mark_device_sources_dirty() noexcept {
    device_sources_dirty_ = true;
}

void PlatformInterruptRouter::route(const PlatformInterruptSource source,
                                    const bool asserted,
                                    const std::uint8_t level) {
    const auto id = source_id(source);
    if (asserted) {
        controller_.request(id, level, static_cast<std::uint32_t>(source));
    } else if (controller_.pending(id)) {
        static_cast<void>(controller_.cancel(id));
    }
}

std::size_t PlatformInterruptRouter::synchronize() {
    // A caller may explicitly synchronize without observing source_epoch()
    // first, for example after advancing a device from the scheduler. Sample
    // the compact device state once and preserve its epoch before publishing
    // the refreshed routes.
    device_sources_dirty_ = true;
    refresh_device_source_epoch();
    const auto device_pending = device_pending_state_;
    const auto pending_at = [device_pending](const std::size_t bit) noexcept {
        return (device_pending & static_cast<std::uint16_t>(1u << bit)) != 0u;
    };

    std::size_t asserted = 0u;
    for (std::size_t channel = 0u; channel < tmu_sources.size(); ++channel) {
        const bool pending = pending_at(channel);
        route(tmu_sources[channel], pending, tmu_levels_[channel]);
        asserted += pending ? 1u : 0u;
    }
    const bool rtc_alarm = pending_at(3u);
    const bool rtc_periodic = pending_at(4u);
    const bool rtc_carry = pending_at(5u);
    route(PlatformInterruptSource::RtcAlarm, rtc_alarm, rtc_level_);
    route(PlatformInterruptSource::RtcPeriodic, rtc_periodic, rtc_level_);
    route(PlatformInterruptSource::RtcCarry, rtc_carry, rtc_level_);
    asserted += rtc_alarm ? 1u : 0u;
    asserted += rtc_periodic ? 1u : 0u;
    asserted += rtc_carry ? 1u : 0u;

    for (std::size_t channel = 0u; channel < dma_sources.size(); ++channel) {
        const bool pending = pending_at(6u + channel);
        route(dma_sources[channel], pending, dma_level_);
        asserted += pending ? 1u : 0u;
    }
    const bool dma_error = pending_at(10u);
    route(PlatformInterruptSource::DmaError, dma_error, dma_level_);
    asserted += dma_error ? 1u : 0u;

    for (std::size_t source = 0u; source < scif_sources.size(); ++source) {
        route(scif_sources[source], scif_pending_[source], scif_level_);
        asserted += scif_pending_[source] ? 1u : 0u;
    }

    for (std::size_t line = 0u; line < external_sources.size(); ++line) {
        route(external_sources[line], external_pending_[line], external_levels[line]);
        asserted += external_pending_[line] ? 1u : 0u;
    }
    return asserted;
}

bool PlatformInterruptRouter::accept(CpuState& cpu) {
    // No SH-4 interrupt level can pass BL or IMASK=15.  Deferring the device
    // scan until the CPU lowers its mask preserves delivery while avoiding a
    // full router walk at every privileged bootstrap safepoint.
    if (cpu.interrupts_blocked() || cpu.interrupt_mask() == 15u) return false;
    static_cast<void>(synchronize());
    return accept_cached(cpu);
}

bool PlatformInterruptRouter::accept_cached(CpuState& cpu) noexcept {
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
    for (const auto source : scif_sources)
        static_cast<void>(controller_.cancel(source_id(source)));
    for (const auto source : external_sources) {
        static_cast<void>(controller_.cancel(source_id(source)));
    }
    tmu_levels_ = {};
    rtc_level_ = 0u;
    dma_level_ = 0u;
    scif_level_ = 0u;
    scif_pending_ = {};
    external_pending_ = {};
    ++source_epoch_;
    device_sources_dirty_ = true;
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

Sh4InterruptRegistersSnapshot Sh4InterruptRegisters::snapshot() const noexcept {
    return {
        interrupt_control_,
        priority_a_,
        priority_b_,
        priority_c_,
        0u,
    };
}

void Sh4InterruptRegisters::synchronize_priorities() noexcept {
    router_.set_tmu_level(0u, static_cast<std::uint8_t>((priority_a_ >> 12u) & 0xFu));
    router_.set_tmu_level(1u, static_cast<std::uint8_t>((priority_a_ >> 8u) & 0xFu));
    router_.set_tmu_level(2u, static_cast<std::uint8_t>((priority_a_ >> 4u) & 0xFu));
    router_.set_rtc_level(static_cast<std::uint8_t>(priority_a_ & 0xFu));
    router_.set_dma_level(static_cast<std::uint8_t>((priority_c_ >> 8u) & 0xFu));
    router_.set_scif_level(static_cast<std::uint8_t>((priority_c_ >> 4u) & 0xFu));
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
    memory.set_mmio_interrupt_state_sink(
        MmioInterruptStateSink{
            &router,
            [](void* const context) noexcept {
                static_cast<PlatformInterruptRouter*>(context)->mark_device_sources_dirty();
            },
        });
    return registers;
}

} // namespace katana::runtime

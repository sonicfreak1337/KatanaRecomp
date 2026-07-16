#include "katana/runtime/platform_interrupt.hpp"

#include <array>
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
    const bool rtc_periodic = rtc_.periodic_interrupt_pending();
    const bool rtc_carry = rtc_.carry_interrupt_pending();
    route(PlatformInterruptSource::RtcPeriodic, rtc_periodic, rtc_level_);
    route(PlatformInterruptSource::RtcCarry, rtc_carry, rtc_level_);
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

} // namespace katana::runtime

#include "katana/runtime/scif.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace katana::runtime {
namespace {

constexpr std::size_t fifo_capacity = 16u;

void require_width(const MemoryAccessWidth actual,
                   const MemoryAccessWidth expected,
                   const char* const name) {
    if (actual != expected)
        throw std::invalid_argument(std::string{"SH-4-SCIF "} + name +
                                    " besitzt eine ungueltige Zugriffsbreite.");
}

} // namespace

Sh4Scif::Sh4Scif(EventScheduler& scheduler,
                 InterruptObserver interrupt_observer,
                 TransmitObserver transmit_observer)
    : scheduler_(scheduler), interrupt_observer_(std::move(interrupt_observer)),
      transmit_observer_(std::move(transmit_observer)),
      scheduler_lifetime_(scheduler.lifetime_token()) {
    reset_observer_ = scheduler_.add_reset_observer([this] { handle_scheduler_reset(); });
    reset();
}

Sh4Scif::~Sh4Scif() {
    if (scheduler_lifetime_.expired()) return;
    cancel_transmit();
    static_cast<void>(scheduler_.remove_reset_observer(reset_observer_));
}

std::uint64_t Sh4Scif::frame_cycles() const {
    const auto clock_scale = std::uint64_t{1u} << ((mode_ & 3u) * 2u);
    const auto data_bits = (mode_ & 0x40u) != 0u ? 7u : 8u;
    const auto parity_bits = (mode_ & 0x20u) != 0u ? 1u : 0u;
    const auto stop_bits = (mode_ & 0x08u) != 0u ? 2u : 1u;
    const auto frame_bits = std::uint64_t{1u} + data_bits + parity_bits + stop_bits;
    const auto divisor = std::uint64_t{128u} * (static_cast<std::uint64_t>(bit_rate_) + 1u) *
                         clock_scale;
    if (divisor > std::numeric_limits<std::uint64_t>::max() / frame_bits)
        throw std::overflow_error("SH-4-SCIF-Framezeit ist uebergelaufen.");
    return divisor * frame_bits;
}

std::size_t Sh4Scif::receive_trigger() const noexcept {
    static constexpr std::array<std::size_t, 4u> levels{1u, 4u, 8u, 14u};
    return levels[(fifo_control_ >> 6u) & 3u];
}

std::size_t Sh4Scif::transmit_trigger() const noexcept {
    static constexpr std::array<std::size_t, 4u> levels{8u, 4u, 2u, 1u};
    return levels[(fifo_control_ >> 4u) & 3u];
}

void Sh4Scif::refresh_status() noexcept {
    if (transmit_fifo_.size() <= transmit_trigger()) status_ |= status_transmit_empty;
    else status_ &= static_cast<std::uint16_t>(~status_transmit_empty);
    if (!transmit_event_ && transmit_fifo_.empty()) status_ |= status_transmit_end;
    else status_ &= static_cast<std::uint16_t>(~status_transmit_end);
    if (receive_fifo_.empty())
        status_ &= static_cast<std::uint16_t>(~(status_receive_full | status_receive_ready));
    else {
        status_ |= status_receive_ready;
        if (receive_fifo_.size() >= receive_trigger()) status_ |= status_receive_full;
        else status_ &= static_cast<std::uint16_t>(~status_receive_full);
    }
}

void Sh4Scif::update_interrupts() noexcept {
    refresh_status();
    if (!interrupt_observer_) return;
    interrupt_observer_(Sh4ScifInterrupt::Transmit,
                        (control_ & 0x80u) != 0u && (status_ & status_transmit_empty) != 0u);
    interrupt_observer_(Sh4ScifInterrupt::Receive,
                        (control_ & 0x40u) != 0u &&
                            (status_ & (status_receive_full | status_receive_ready)) != 0u);
    interrupt_observer_(Sh4ScifInterrupt::Break,
                        (control_ & (0x40u | 0x08u)) != 0u &&
                            (status_ & status_break) != 0u);
    interrupt_observer_(Sh4ScifInterrupt::Error,
                        (control_ & (0x40u | 0x08u)) != 0u &&
                            ((status_ & (status_error | status_framing_error |
                                         status_parity_error)) != 0u ||
                             (line_status_ & 1u) != 0u));
}

void Sh4Scif::schedule_transmit() {
    if (transmit_event_ || transmit_fifo_.empty() || (control_ & 0x20u) == 0u ||
        (fifo_control_ & 0x04u) != 0u)
        return;
    transmit_event_ = scheduler_.schedule_after(
        frame_cycles(),
        [this](const auto event_id, const auto) { complete_transmit(event_id); },
        SchedulerEventKind::ScifTransmit);
    update_interrupts();
}

void Sh4Scif::complete_transmit(const SchedulerEventId event_id) {
    if (!transmit_event_ || *transmit_event_ != event_id || transmit_fifo_.empty())
        throw std::logic_error("SH-4-SCIF-Completion besitzt keinen aktiven Transfer.");
    transmit_event_.reset();
    const auto value = transmit_fifo_.front();
    transmit_fifo_.pop_front();
    transmitted_bytes_.push_back(value);
    if (transmit_observer_) transmit_observer_(value);
    update_interrupts();
    schedule_transmit();
}

void Sh4Scif::cancel_transmit() noexcept {
    if (transmit_event_ && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_.cancel(*transmit_event_));
    transmit_event_.reset();
}

std::uint32_t Sh4Scif::read(const std::uint32_t offset, const MemoryAccessWidth width) {
    switch (offset) {
    case 0x00u: require_width(width, MemoryAccessWidth::Halfword, "SCSMR2"); return mode_;
    case 0x04u: require_width(width, MemoryAccessWidth::Byte, "SCBRR2"); return bit_rate_;
    case 0x08u: require_width(width, MemoryAccessWidth::Halfword, "SCSCR2"); return control_;
    case 0x10u:
        require_width(width, MemoryAccessWidth::Halfword, "SCFSR2");
        status_last_read_ = status_;
        return status_;
    case 0x14u: {
        require_width(width, MemoryAccessWidth::Byte, "SCFRDR2");
        const auto value = receive_fifo_.empty() ? 0u : receive_fifo_.front();
        if (!receive_fifo_.empty()) receive_fifo_.pop_front();
        update_interrupts();
        return value;
    }
    case 0x18u: require_width(width, MemoryAccessWidth::Halfword, "SCFCR2"); return fifo_control_;
    case 0x1Cu:
        require_width(width, MemoryAccessWidth::Halfword, "SCFDR2");
        return static_cast<std::uint32_t>((transmit_fifo_.size() << 8u) | receive_fifo_.size());
    case 0x20u:
        require_width(width, MemoryAccessWidth::Halfword, "SCSPTR2");
        return port_ & static_cast<std::uint16_t>(~0x10u);
    case 0x24u: require_width(width, MemoryAccessWidth::Halfword, "SCLSR2"); return line_status_;
    default: throw std::invalid_argument("Unbekannter oder nicht lesbarer SH-4-SCIF-Offset.");
    }
}

void Sh4Scif::write(const std::uint32_t offset,
                    const std::uint32_t value,
                    const MemoryAccessWidth width) {
    switch (offset) {
    case 0x00u:
        require_width(width, MemoryAccessWidth::Halfword, "SCSMR2");
        mode_ = static_cast<std::uint16_t>(value & 0x007Bu);
        return;
    case 0x04u:
        require_width(width, MemoryAccessWidth::Byte, "SCBRR2");
        bit_rate_ = static_cast<std::uint8_t>(value);
        return;
    case 0x08u:
        require_width(width, MemoryAccessWidth::Halfword, "SCSCR2");
        control_ = static_cast<std::uint16_t>(value & 0x00FAu);
        if ((control_ & 0x20u) == 0u) cancel_transmit();
        update_interrupts();
        schedule_transmit();
        return;
    case 0x0Cu: {
        require_width(width, MemoryAccessWidth::Byte, "SCFTDR2");
        if ((fifo_control_ & 0x04u) == 0u && transmit_fifo_.size() < fifo_capacity) {
            auto byte = static_cast<std::uint8_t>(value);
            if ((mode_ & 0x40u) != 0u) byte &= 0x7Fu;
            transmit_fifo_.push_back(byte);
        }
        update_interrupts();
        schedule_transmit();
        return;
    }
    case 0x10u: {
        require_width(width, MemoryAccessWidth::Halfword, "SCFSR2");
        constexpr auto clearable = static_cast<std::uint16_t>(0x00F3u);
        const auto requested = static_cast<std::uint16_t>(value);
        const auto read_clear = static_cast<std::uint16_t>(status_last_read_ & ~requested & clearable);
        status_ &= static_cast<std::uint16_t>(~read_clear);
        status_last_read_ &= requested;
        update_interrupts();
        return;
    }
    case 0x18u:
        require_width(width, MemoryAccessWidth::Halfword, "SCFCR2");
        fifo_control_ = static_cast<std::uint16_t>(value & 0x00FFu);
        if ((fifo_control_ & 0x04u) != 0u) {
            cancel_transmit();
            transmit_fifo_.clear();
        }
        if ((fifo_control_ & 0x02u) != 0u) receive_fifo_.clear();
        update_interrupts();
        schedule_transmit();
        return;
    case 0x20u:
        require_width(width, MemoryAccessWidth::Halfword, "SCSPTR2");
        port_ = static_cast<std::uint16_t>(value & 0x00F3u);
        return;
    case 0x24u:
        require_width(width, MemoryAccessWidth::Halfword, "SCLSR2");
        if ((value & 1u) == 0u) line_status_ &= static_cast<std::uint16_t>(~1u);
        update_interrupts();
        return;
    default: throw std::invalid_argument("Unbekannter oder nicht schreibbarer SH-4-SCIF-Offset.");
    }
}

void Sh4Scif::inject_receive(const std::uint8_t value) {
    if ((control_ & 0x10u) == 0u || (fifo_control_ & 0x02u) != 0u) return;
    if (receive_fifo_.size() >= fifo_capacity) line_status_ |= 1u;
    else receive_fifo_.push_back((mode_ & 0x40u) != 0u ? value & 0x7Fu : value);
    update_interrupts();
}

void Sh4Scif::inject_break() noexcept {
    status_ |= status_break;
    update_interrupts();
}

void Sh4Scif::reset() noexcept {
    cancel_transmit();
    transmit_fifo_.clear();
    receive_fifo_.clear();
    transmitted_bytes_.clear();
    mode_ = 0u;
    bit_rate_ = 0xFFu;
    control_ = 0u;
    status_ = status_transmit_end | status_transmit_empty;
    status_last_read_ = 0u;
    fifo_control_ = 0u;
    port_ = 0u;
    line_status_ = 0u;
    update_interrupts();
}

void Sh4Scif::handle_scheduler_reset() noexcept {
    transmit_event_.reset();
    try {
        schedule_transmit();
    } catch (...) {
    }
}

std::size_t Sh4Scif::transmit_fifo_size() const noexcept { return transmit_fifo_.size(); }
std::size_t Sh4Scif::receive_fifo_size() const noexcept { return receive_fifo_.size(); }
const std::vector<std::uint8_t>& Sh4Scif::transmitted_bytes() const noexcept {
    return transmitted_bytes_;
}

Sh4ScifSnapshot Sh4Scif::snapshot() const {
    Sh4ScifSnapshot result;
    result.transmit_event = transmit_event_;
    result.transmit_fifo.assign(transmit_fifo_.begin(), transmit_fifo_.end());
    result.receive_fifo.assign(receive_fifo_.begin(), receive_fifo_.end());
    result.transmitted_bytes = transmitted_bytes_;
    result.mode = mode_;
    result.bit_rate = bit_rate_;
    result.control = control_;
    result.status = status_;
    result.status_last_read = status_last_read_;
    result.fifo_control = fifo_control_;
    result.port = port_;
    result.line_status = line_status_;
    return result;
}

std::shared_ptr<Sh4Scif> map_sh4_scif(Memory& memory,
                                      EventScheduler& scheduler,
                                      Sh4Scif::InterruptObserver interrupt_observer,
                                      Sh4Scif::TransmitObserver transmit_observer) {
    auto state = std::make_shared<Sh4Scif>(
        scheduler, std::move(interrupt_observer), std::move(transmit_observer));
    auto device = std::make_shared<MmioMemoryDevice>(
        sh4_scif_register_size,
        [state](const auto offset, const auto width) { return state->read(offset, width); },
        [state](const auto offset, const auto value, const auto width) {
            state->write(offset, value, width);
        });
    memory.map_region("sh4-scif-p4", sh4_scif_p4_base, device);
    memory.map_region("sh4-scif-area7", sh4_scif_area7_base, device);
    return state;
}

} // namespace katana::runtime

#include "katana/runtime/scif.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

namespace katana::runtime {
namespace {

constexpr std::size_t fifo_capacity = 16u;
constexpr std::array<std::uint8_t, 8u> sh4_scif_state_magic{
    'K', 'A', 'T', 'S', 'C', 'F', '1', '\n'};

class ScifStateWriter final {
  public:
    void u8(const std::uint8_t value) { bytes_.push_back(value); }
    void u16(const std::uint16_t value) {
        for (std::size_t byte = 0u; byte < 2u; ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void u32(const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void u64(const std::uint64_t value) {
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            bytes_.push_back(
                static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void raw(const std::span<const std::uint8_t> bytes) {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }
    void byte_vector(const std::span<const std::uint8_t> bytes) {
        if (bytes.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument(
                "SH-4-SCIF-Handoff-Payload ist zu gross.");
        u32(static_cast<std::uint32_t>(bytes.size()));
        raw(bytes);
    }
    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

class ScifStateReader final {
  public:
    explicit ScifStateReader(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {}
    [[nodiscard]] std::uint8_t u8() {
        require(1u);
        return bytes_[offset_++];
    }
    [[nodiscard]] std::uint16_t u16() {
        require(2u);
        std::uint16_t value = 0u;
        for (std::size_t byte = 0u; byte < 2u; ++byte)
            value |= static_cast<std::uint16_t>(bytes_[offset_ + byte])
                     << (byte * 8u);
        offset_ += 2u;
        return value;
    }
    [[nodiscard]] std::uint32_t u32() {
        require(4u);
        std::uint32_t value = 0u;
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            value |= static_cast<std::uint32_t>(bytes_[offset_ + byte])
                     << (byte * 8u);
        offset_ += 4u;
        return value;
    }
    [[nodiscard]] std::uint64_t u64() {
        require(8u);
        std::uint64_t value = 0u;
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            value |= static_cast<std::uint64_t>(bytes_[offset_ + byte])
                     << (byte * 8u);
        offset_ += 8u;
        return value;
    }
    [[nodiscard]] std::vector<std::uint8_t> raw(const std::size_t size) {
        require(size);
        std::vector<std::uint8_t> result(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
        offset_ += size;
        return result;
    }
    [[nodiscard]] std::vector<std::uint8_t> byte_vector() {
        return raw(u32());
    }
    void expect_end() const {
        if (offset_ != bytes_.size())
            throw std::invalid_argument(
                "SH-4-SCIF-Handoff-Payload besitzt nachlaufende Bytes.");
    }

  private:
    void require(const std::size_t size) const {
        if (offset_ > bytes_.size() || size > bytes_.size() - offset_)
            throw std::invalid_argument(
                "SH-4-SCIF-Handoff-Payload ist abgeschnitten.");
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0u;
};

void validate_scif_portable_shape(const Sh4ScifSnapshot& state,
                                  const bool require_live_event) {
    constexpr std::uint16_t transmit_end = 0x0040u;
    constexpr std::uint16_t transmit_empty = 0x0020u;
    constexpr std::uint16_t receive_full = 0x0002u;
    constexpr std::uint16_t receive_ready = 0x0001u;
    if (state.transmit_fifo.size() > fifo_capacity ||
        state.receive_fifo.size() > fifo_capacity ||
        (state.mode & ~std::uint16_t{0x007Bu}) != 0u ||
        (state.control & ~std::uint16_t{0x00FAu}) != 0u ||
        (state.status & ~std::uint16_t{0x00FFu}) != 0u ||
        (state.status_last_read & ~std::uint16_t{0x00FFu}) != 0u ||
        (state.fifo_control & ~std::uint16_t{0x00FFu}) != 0u ||
        (state.port & ~std::uint16_t{0x00F3u}) != 0u ||
        (state.line_status & ~std::uint16_t{0x0001u}) != 0u ||
        (state.transmit_event &&
         state.transmit_event_rehydration_pending))
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff besitzt ungueltige FIFO- oder Registerdaten.");

    const bool requires_event =
        !state.transmit_fifo.empty() && (state.control & 0x20u) != 0u &&
        (state.fifo_control & 0x04u) == 0u;
    if (require_live_event && requires_event &&
        (!(state.transmit_event ||
           state.transmit_event_rehydration_pending) ||
         !state.transmit_event_deadline))
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff besitzt keinen aktiven Sendeeventvertrag.");
    if (!requires_event &&
        (state.transmit_event || state.transmit_event_deadline ||
         state.transmit_event_rehydration_pending))
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff besitzt ein Event ohne aktiven Sendeframe.");
    if (state.transmit_event_deadline &&
        *state.transmit_event_deadline <= state.scheduler_cycle)
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff-Sendeevent liegt nicht hinter dem Schedulerzyklus.");

    static constexpr std::array<std::size_t, 4u> receive_levels{
        1u, 4u, 8u, 14u};
    static constexpr std::array<std::size_t, 4u> transmit_levels{
        8u, 4u, 2u, 1u};
    const auto receive_level =
        receive_levels[(state.fifo_control >> 6u) & 3u];
    const auto transmit_level =
        transmit_levels[(state.fifo_control >> 4u) & 3u];
    auto expected_status = state.status;
    if (state.transmit_fifo.size() <= transmit_level)
        expected_status |= transmit_empty;
    else
        expected_status &= static_cast<std::uint16_t>(~transmit_empty);
    if (!requires_event && state.transmit_fifo.empty())
        expected_status |= transmit_end;
    else
        expected_status &= static_cast<std::uint16_t>(~transmit_end);
    if (state.receive_fifo.empty()) {
        expected_status &= static_cast<std::uint16_t>(
            ~(receive_full | receive_ready));
    } else {
        expected_status |= receive_ready;
        if (state.receive_fifo.size() >= receive_level)
            expected_status |= receive_full;
        else
            expected_status &= static_cast<std::uint16_t>(~receive_full);
    }
    constexpr auto derived_status =
        transmit_end | transmit_empty | receive_full | receive_ready;
    if ((expected_status & derived_status) !=
        (state.status & derived_status))
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff besitzt inkonsistente FIFO-Statusbits.");
}

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
    if (!transmit_event_ && !transmit_event_rehydration_pending_ &&
        transmit_fifo_.empty())
        status_ |= status_transmit_end;
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
    if (transmit_event_ || transmit_event_rehydration_pending_ ||
        transmit_fifo_.empty() || (control_ & 0x20u) == 0u ||
        (fifo_control_ & 0x04u) != 0u)
        return;
    const auto cycles = frame_cycles();
    if (cycles >
        std::numeric_limits<std::uint64_t>::max() -
            scheduler_.current_cycle())
        throw std::overflow_error(
            "SH-4-SCIF-Sendefrist ist uebergelaufen.");
    const auto deadline = scheduler_.current_cycle() + cycles;
    const auto event_id = scheduler_.schedule_at(
        deadline,
        [this](const auto event_id, const auto) { complete_transmit(event_id); },
        SchedulerEventKind::ScifTransmit);
    transmit_event_ = event_id;
    transmit_event_deadline_ = deadline;
    transmit_event_rehydration_pending_ = false;
    update_interrupts();
}

void Sh4Scif::complete_transmit(const SchedulerEventId event_id) {
    if (!transmit_event_ || *transmit_event_ != event_id || transmit_fifo_.empty())
        throw std::logic_error("SH-4-SCIF-Completion besitzt keinen aktiven Transfer.");
    transmit_event_.reset();
    transmit_event_deadline_.reset();
    transmit_event_rehydration_pending_ = false;
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
    transmit_event_deadline_.reset();
    transmit_event_rehydration_pending_ = false;
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
    transmit_event_deadline_.reset();
    transmit_event_rehydration_pending_ = false;
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
    result.scheduler_cycle = scheduler_.current_cycle();
    result.transmit_event = transmit_event_;
    result.transmit_event_deadline = transmit_event_deadline_;
    result.transmit_event_rehydration_pending =
        transmit_event_rehydration_pending_;
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

std::vector<std::uint8_t> encode_sh4_scif_state(
    const Sh4ScifSnapshot& state) {
    validate_scif_portable_shape(state, true);
    ScifStateWriter writer;
    writer.raw(sh4_scif_state_magic);
    writer.u32(sh4_scif_state_contract_version);
    writer.u64(state.scheduler_cycle);
    writer.byte_vector(state.transmit_fifo);
    writer.byte_vector(state.receive_fifo);
    writer.byte_vector(state.transmitted_bytes);
    writer.u16(state.mode);
    writer.u8(state.bit_rate);
    writer.u16(state.control);
    writer.u16(state.status);
    writer.u16(state.status_last_read);
    writer.u16(state.fifo_control);
    writer.u16(state.port);
    writer.u16(state.line_status);
    return std::move(writer).finish();
}

Sh4ScifSnapshot decode_sh4_scif_state(
    const std::span<const std::uint8_t> bytes) {
    ScifStateReader reader(bytes);
    const auto magic = reader.raw(sh4_scif_state_magic.size());
    if (!std::equal(magic.begin(),
                    magic.end(),
                    sh4_scif_state_magic.begin(),
                    sh4_scif_state_magic.end()))
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff-Payload besitzt eine ungueltige Kennung.");
    if (reader.u32() != sh4_scif_state_contract_version)
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff-Payload besitzt eine unbekannte Vertragsversion.");

    Sh4ScifSnapshot state;
    state.scheduler_cycle = reader.u64();
    state.transmit_fifo = reader.byte_vector();
    state.receive_fifo = reader.byte_vector();
    state.transmitted_bytes = reader.byte_vector();
    state.mode = reader.u16();
    state.bit_rate = reader.u8();
    state.control = reader.u16();
    state.status = reader.u16();
    state.status_last_read = reader.u16();
    state.fifo_control = reader.u16();
    state.port = reader.u16();
    state.line_status = reader.u16();
    state.transmit_event.reset();
    state.transmit_event_deadline.reset();
    state.transmit_event_rehydration_pending = false;
    validate_scif_portable_shape(state, false);
    reader.expect_end();
    return state;
}

void Sh4Scif::validate_state_restore(const Sh4ScifSnapshot& state) const {
    validate_state_restore(state, scheduler_.current_cycle());
}

void Sh4Scif::validate_state_restore(
    const Sh4ScifSnapshot& state,
    const std::uint64_t expected_scheduler_cycle) const {
    if (state.scheduler_cycle != expected_scheduler_cycle) {
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff passt nicht zum wiederhergestellten Schedulerzyklus.");
    }
    if (state.transmit_fifo.size() > fifo_capacity ||
        state.receive_fifo.size() > fifo_capacity ||
        (state.mode & ~std::uint16_t{0x007Bu}) != 0u ||
        (state.control & ~std::uint16_t{0x00FAu}) != 0u ||
        (state.status & ~std::uint16_t{0x00FFu}) != 0u ||
        (state.status_last_read & ~std::uint16_t{0x00FFu}) != 0u ||
        (state.fifo_control & ~std::uint16_t{0x00FFu}) != 0u ||
        (state.port & ~std::uint16_t{0x00F3u}) != 0u ||
        (state.line_status & ~std::uint16_t{0x0001u}) != 0u ||
        (state.transmit_event &&
         state.transmit_event_rehydration_pending)) {
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff besitzt ungueltige FIFO- oder Registerdaten.");
    }

    const bool requires_event =
        !state.transmit_fifo.empty() && (state.control & 0x20u) != 0u &&
        (state.fifo_control & 0x04u) == 0u;
    if (!requires_event &&
        (state.transmit_event || state.transmit_event_deadline ||
         state.transmit_event_rehydration_pending)) {
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff besitzt ein Event ohne aktiven Sendeframe.");
    }
    if (state.transmit_event_deadline &&
        *state.transmit_event_deadline <= state.scheduler_cycle) {
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff-Sendeevent liegt nicht hinter dem Schedulerzyklus.");
    }
    if ((state.transmit_event || state.transmit_event_rehydration_pending) &&
        !state.transmit_event_deadline) {
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff-Sendeevent besitzt keine Ereignisfrist.");
    }

    static constexpr std::array<std::size_t, 4u> receive_levels{1u, 4u, 8u, 14u};
    static constexpr std::array<std::size_t, 4u> transmit_levels{8u, 4u, 2u, 1u};
    const auto receive_level = receive_levels[(state.fifo_control >> 6u) & 3u];
    const auto transmit_level = transmit_levels[(state.fifo_control >> 4u) & 3u];
    auto expected_status = state.status;
    if (state.transmit_fifo.size() <= transmit_level)
        expected_status |= status_transmit_empty;
    else
        expected_status &= static_cast<std::uint16_t>(~status_transmit_empty);
    if (!requires_event && state.transmit_fifo.empty())
        expected_status |= status_transmit_end;
    else
        expected_status &= static_cast<std::uint16_t>(~status_transmit_end);
    if (state.receive_fifo.empty()) {
        expected_status &= static_cast<std::uint16_t>(
            ~(status_receive_full | status_receive_ready));
    } else {
        expected_status |= status_receive_ready;
        if (state.receive_fifo.size() >= receive_level)
            expected_status |= status_receive_full;
        else
            expected_status &= static_cast<std::uint16_t>(~status_receive_full);
    }
    constexpr auto derived_status =
        status_transmit_end | status_transmit_empty | status_receive_full |
        status_receive_ready;
    if ((expected_status & derived_status) !=
        (state.status & derived_status)) {
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff besitzt inkonsistente FIFO-Statusbits.");
    }
}

PreparedSh4ScifStateRestore Sh4Scif::prepare_state_restore(
    const Sh4ScifSnapshot& state) const {
    validate_state_restore(state);
    PreparedSh4ScifStateRestore prepared;
    prepared.owner_ = this;
    prepared.state_ = state;
    prepared.transmit_fifo_ = std::deque<std::uint8_t>(
        state.transmit_fifo.begin(), state.transmit_fifo.end());
    prepared.receive_fifo_ = std::deque<std::uint8_t>(
        state.receive_fifo.begin(), state.receive_fifo.end());
    return prepared;
}

void Sh4Scif::commit_prepared_state_restore(
    PreparedSh4ScifStateRestore prepared) noexcept {
    if (prepared.owner_ != this) std::terminate();
    auto& state = prepared.state_;
    cancel_transmit();
    transmit_fifo_.swap(prepared.transmit_fifo_);
    receive_fifo_.swap(prepared.receive_fifo_);
    transmitted_bytes_ = std::move(state.transmitted_bytes);
    mode_ = state.mode;
    bit_rate_ = state.bit_rate;
    control_ = state.control;
    status_ = state.status;
    status_last_read_ = state.status_last_read;
    fifo_control_ = state.fifo_control;
    port_ = state.port;
    line_status_ = state.line_status;
    transmit_event_.reset();
    transmit_event_deadline_ = state.transmit_event_deadline;
    transmit_event_rehydration_pending_ =
        !transmit_fifo_.empty() && (control_ & 0x20u) != 0u &&
        (fifo_control_ & 0x04u) == 0u;
}

void Sh4Scif::restore_state_passive(const Sh4ScifSnapshot& state) {
    auto prepared = prepare_state_restore(state);
    commit_prepared_state_restore(std::move(prepared));
}

SchedulerEventId Sh4Scif::rehydrate_scheduled_event(
    const std::uint64_t guest_cycle,
    const std::uint32_t channel,
    const std::uint64_t token) {
    if (channel != sh4_scif_transmit_event_channel ||
        token != sh4_scif_transmit_event_token_v1) {
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff besitzt einen unbekannten Eventkanal oder Token.");
    }
    if (!transmit_event_rehydration_pending_ || transmit_event_ ||
        transmit_fifo_.empty() || (control_ & 0x20u) == 0u ||
        (fifo_control_ & 0x04u) != 0u) {
        throw std::logic_error(
            "SH-4-SCIF-Handoff erwartet kein Sendeevent.");
    }
    if (guest_cycle < scheduler_.current_cycle() ||
        (transmit_event_deadline_ &&
         guest_cycle != *transmit_event_deadline_)) {
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff-Sendeevent passt nicht zur erfassten Ereignisfrist.");
    }

    const auto event_id = scheduler_.schedule_at(
        guest_cycle,
        make_rehydrated_scheduled_event_callback(channel, token),
        SchedulerEventKind::ScifTransmit);
    commit_rehydrated_scheduled_event(event_id, channel, token);
    transmit_event_deadline_ = guest_cycle;
    return event_id;
}

SchedulerCallback Sh4Scif::make_rehydrated_scheduled_event_callback(
    const std::uint32_t channel,
    const std::uint64_t token) {
    if (channel != sh4_scif_transmit_event_channel ||
        token != sh4_scif_transmit_event_token_v1)
        throw std::invalid_argument(
            "SH-4-SCIF-Handoff besitzt einen unbekannten Eventkanal oder "
            "Token.");
    return [this](const auto restored_event_id, const auto) {
        complete_transmit(restored_event_id);
    };
}

void Sh4Scif::commit_rehydrated_scheduled_event(
    const SchedulerEventId event_id,
    const std::uint32_t channel,
    const std::uint64_t token) noexcept {
    if (channel != sh4_scif_transmit_event_channel ||
        token != sh4_scif_transmit_event_token_v1 ||
        !transmit_event_rehydration_pending_ || transmit_event_ ||
        transmit_fifo_.empty() || (control_ & 0x20u) == 0u ||
        (fifo_control_ & 0x04u) != 0u)
        std::terminate();
    transmit_event_ = event_id;
    transmit_event_rehydration_pending_ = false;
}

bool Sh4Scif::event_rehydration_pending() const noexcept {
    return transmit_event_rehydration_pending_;
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

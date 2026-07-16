#include "katana/runtime/dma.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#include <string>

namespace katana::runtime {

namespace {

constexpr std::uint32_t transfer_count_mask = 0x00FFFFFFu;
constexpr std::uint32_t source_mode_mask = 0x00003000u;
constexpr std::uint32_t destination_mode_mask = 0x0000C000u;
constexpr std::uint32_t request_source_mask = 0x00000F00u;
constexpr std::uint32_t transfer_size_mask = 0x00000070u;
constexpr std::uint32_t control_writable_mask =
    source_mode_mask | destination_mode_mask | request_source_mask |
    transfer_size_mask | Sh4Dmac::interrupt_enable | Sh4Dmac::channel_enable |
    0x00000080u;
constexpr std::uint32_t operation_writable_mask = 0x00000301u;

std::uint64_t checked_delay(const std::uint64_t cycles_per_byte, const std::size_t bytes) {
    if (cycles_per_byte > std::numeric_limits<std::uint64_t>::max() / bytes) {
        throw std::overflow_error("DMA-Ereignisfrist ist uebergelaufen.");
    }
    return cycles_per_byte * bytes;
}

} // namespace

Sh4Dmac::Sh4Dmac(EventScheduler& scheduler, Memory& memory, const DmaTiming timing)
    : scheduler_(scheduler), memory_(memory), timing_(timing) {
    if (timing_.guest_cycles_per_byte == 0u) {
        throw std::invalid_argument("DMA-Takt muss groesser null sein.");
    }
}

Sh4Dmac::~Sh4Dmac() { cancel_event(); }

Sh4Dmac::Channel& Sh4Dmac::channel(const std::size_t index) {
    if (index >= channels_.size()) { throw std::out_of_range("Ungueltiger DMA-Kanal."); }
    return channels_[index];
}

const Sh4Dmac::Channel& Sh4Dmac::channel(const std::size_t index) const {
    if (index >= channels_.size()) { throw std::out_of_range("Ungueltiger DMA-Kanal."); }
    return channels_[index];
}

void Sh4Dmac::write_source(const std::size_t index, const std::uint32_t value) {
    channel(index).source = value;
    reevaluate();
}

void Sh4Dmac::write_destination(const std::size_t index, const std::uint32_t value) {
    channel(index).destination = value;
    reevaluate();
}

void Sh4Dmac::write_count(const std::size_t index, const std::uint32_t value) {
    channel(index).count = value & transfer_count_mask;
    reevaluate();
}

void Sh4Dmac::write_control(const std::size_t index, const std::uint32_t value) {
    auto& current = channel(index);
    const auto preserved_te = (current.control & transfer_end) & (value & transfer_end);
    current.control = (value & control_writable_mask) | preserved_te;
    if ((current.control & transfer_end) == 0u) { current.interrupt_pending = false; }
    reevaluate();
}

void Sh4Dmac::write_operation(const std::uint32_t value) {
    if ((value & 0x00008000u) != 0u) {
        throw std::invalid_argument("SH-4-DDT-Modus ist im v0.31-DMA-Profil nicht implementiert.");
    }
    const auto preserved_flags =
        (operation_ & (address_error_flag | nmi_flag)) &
        (value & (address_error_flag | nmi_flag));
    operation_ = (value & operation_writable_mask) | preserved_flags;
    if ((operation_ & address_error_flag) == 0u) { last_fault_.reset(); }
    reevaluate();
}

std::uint32_t Sh4Dmac::source(const std::size_t index) const { return channel(index).source; }
std::uint32_t Sh4Dmac::destination(const std::size_t index) const { return channel(index).destination; }
std::uint32_t Sh4Dmac::count(const std::size_t index) const { return channel(index).count; }
std::uint32_t Sh4Dmac::control(const std::size_t index) const { return channel(index).control; }
std::uint32_t Sh4Dmac::operation() const noexcept { return operation_; }

void Sh4Dmac::request_transfer(const std::size_t index) {
    auto& value = channel(index);
    if (value.pending_requests == std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("DMA-Anforderungszaehler ist uebergelaufen.");
    }
    ++value.pending_requests;
    reevaluate();
}

void Sh4Dmac::signal_nmi() noexcept {
    operation_ |= nmi_flag;
    cancel_event();
    discard_external_requests();
}

bool Sh4Dmac::interrupt_pending(const std::size_t index) const {
    return channel(index).interrupt_pending;
}

bool Sh4Dmac::address_error() const noexcept {
    return (operation_ & address_error_flag) != 0u;
}

const std::optional<DmaFault>& Sh4Dmac::last_fault() const noexcept { return last_fault_; }

std::uint64_t Sh4Dmac::completed_transfer_units(const std::size_t index) const {
    return channel(index).completed_units;
}

bool Sh4Dmac::enabled(const std::size_t index) const noexcept {
    const auto& value = channels_[index];
    return (operation_ & master_enable) != 0u &&
           (operation_ & (address_error_flag | nmi_flag)) == 0u &&
           (value.control & channel_enable) != 0u &&
           (value.control & transfer_end) == 0u;
}

bool Sh4Dmac::automatic(const Channel& value) const noexcept {
    const auto request_source = (value.control & request_source_mask) >> 8u;
    return request_source >= 4u && request_source <= 6u;
}

std::optional<std::size_t> Sh4Dmac::select_channel() const noexcept {
    static constexpr std::array<std::array<std::size_t, channel_count>, 3> fixed_orders = {{
        {{0u, 1u, 2u, 3u}},
        {{0u, 2u, 3u, 1u}},
        {{2u, 0u, 1u, 3u}},
    }};
    const auto eligible = [this](const std::size_t index) {
        return enabled(index) && (automatic(channels_[index]) || channels_[index].pending_requests != 0u);
    };
    const auto priority = static_cast<std::size_t>((operation_ >> 8u) & 0x3u);
    if (priority < fixed_orders.size()) {
        for (const auto index : fixed_orders[priority]) {
            if (eligible(index)) { return index; }
        }
        return std::nullopt;
    }
    for (std::size_t offset = 0u; offset < channel_count; ++offset) {
        const auto index = (round_robin_cursor_ + offset) % channel_count;
        if (eligible(index)) { return index; }
    }
    return std::nullopt;
}

std::size_t Sh4Dmac::transfer_size(const Channel& value) noexcept {
    switch ((value.control & transfer_size_mask) >> 4u) {
        case 0u: return 8u;
        case 1u: return 1u;
        case 2u: return 2u;
        case 3u: return 4u;
        case 4u: return 32u;
        default: return 0u;
    }
}

std::uint8_t Sh4Dmac::address_mode(const std::uint32_t control, const unsigned shift) noexcept {
    return static_cast<std::uint8_t>((control >> shift) & 0x3u);
}

void Sh4Dmac::reevaluate() {
    if (event_) {
        if (scheduled_channel_ && enabled(*scheduled_channel_)) { return; }
        cancel_event();
    }
    if (const auto next = select_channel()) { schedule(*next); }
}

void Sh4Dmac::discard_external_requests() noexcept {
    for (auto& value : channels_) {
        value.pending_requests = 0u;
    }
}

void Sh4Dmac::cancel_event() noexcept {
    if (event_) { static_cast<void>(scheduler_.cancel(*event_)); }
    event_.reset();
    scheduled_channel_.reset();
}

void Sh4Dmac::schedule(const std::size_t index) {
    const auto size = transfer_size(channel(index));
    if (size == 0u) {
        set_fault(index, DmaFaultReason::InvalidTransferSize, 0u);
        return;
    }
    scheduled_channel_ = index;
    event_ = scheduler_.schedule_after(
        checked_delay(timing_.guest_cycles_per_byte, size),
        [this, index](const auto, const auto) { handle_transfer(index); }
    );
}

void Sh4Dmac::handle_transfer(const std::size_t index) {
    event_.reset();
    scheduled_channel_.reset();
    if (!enabled(index)) { reevaluate(); return; }
    auto& value = channel(index);
    if (!automatic(value)) {
        if (value.pending_requests == 0u) { reevaluate(); return; }
        --value.pending_requests;
    }
    const auto size = transfer_size(value);
    if (!transfer_one(index, size)) { return; }
    ++value.completed_units;
    value.count = (value.count == 0u ? transfer_count_mask : value.count - 1u);
    update_addresses(value, size);
    if (value.count == 0u) {
        value.control |= transfer_end;
        value.interrupt_pending = (value.control & interrupt_enable) != 0u;
    }
    if (((operation_ >> 8u) & 0x3u) == 3u) {
        round_robin_cursor_ = (index + 1u) % channel_count;
    }
    reevaluate();
}

bool Sh4Dmac::transfer_one(const std::size_t index, const std::size_t size) noexcept {
    auto& value = channels_[index];
    if (size == 0u) { set_fault(index, DmaFaultReason::InvalidTransferSize, size); return false; }
    const auto request_source = (value.control & request_source_mask) >> 8u;
    const bool prohibited_request = request_source == 1u || request_source == 7u ||
                                    request_source == 15u ||
                                    (index > 1u && request_source <= 3u);
    if (prohibited_request) {
        set_fault(index, DmaFaultReason::InvalidRequestSource, size);
        return false;
    }
    const auto source_mode = address_mode(value.control, 12u);
    const auto destination_mode = address_mode(value.control, 14u);
    if (source_mode == 3u || destination_mode == 3u) {
        set_fault(index, DmaFaultReason::ProhibitedAddressMode, size);
        return false;
    }
    if ((value.source % size) != 0u || (value.destination % size) != 0u) {
        set_fault(index, DmaFaultReason::MisalignedAddress, size);
        return false;
    }
    const auto tail = static_cast<std::uint32_t>(size - 1u);
    if (value.source > std::numeric_limits<std::uint32_t>::max() - tail ||
        value.destination > std::numeric_limits<std::uint32_t>::max() - tail) {
        set_fault(index, DmaFaultReason::MemoryAccess, size);
        return false;
    }
    try {
        if (size == 1u) {
            memory_.write_u8(value.destination, memory_.read_u8(value.source));
        } else if (size == 2u) {
            memory_.write_u16(value.destination, memory_.read_u16(value.source));
        } else if (size == 4u) {
            memory_.write_u32(value.destination, memory_.read_u32(value.source));
        } else {
            std::array<std::uint32_t, 8u> words{};
            const auto word_count = size / 4u;
            for (std::size_t word = 0u; word < word_count; ++word) {
                const auto offset = static_cast<std::uint32_t>(word * 4u);
                words[word] = memory_.read_u32(value.source + offset);
            }
            for (std::size_t word = 0u; word < word_count; ++word) {
                const auto offset = static_cast<std::uint32_t>(word * 4u);
                memory_.write_u32(value.destination + offset, words[word]);
            }
        }
    } catch (const std::exception&) {
        set_fault(index, DmaFaultReason::MemoryAccess, size);
        return false;
    }
    return true;
}

void Sh4Dmac::update_addresses(Channel& value, const std::size_t size) noexcept {
    const auto update = [size](std::uint32_t& address, const std::uint8_t mode) {
        if (mode == 1u) { address += static_cast<std::uint32_t>(size); }
        else if (mode == 2u) { address -= static_cast<std::uint32_t>(size); }
    };
    update(value.source, address_mode(value.control, 12u));
    update(value.destination, address_mode(value.control, 14u));
}

void Sh4Dmac::set_fault(
    const std::size_t index,
    const DmaFaultReason reason,
    const std::size_t size
) noexcept {
    operation_ |= address_error_flag;
    const auto& value = channels_[index];
    last_fault_ = DmaFault{reason, index, value.source, value.destination, size};
    cancel_event();
    discard_external_requests();
}

void Sh4Dmac::reset() noexcept {
    cancel_event();
    channels_ = {};
    operation_ = 0u;
    last_fault_.reset();
    round_robin_cursor_ = 0u;
}

std::shared_ptr<Sh4Dmac> map_sh4_dmac_registers(
    Memory& memory,
    EventScheduler& scheduler,
    const DmaTiming timing
) {
    auto dmac = std::make_shared<Sh4Dmac>(scheduler, memory, timing);
    auto device = std::make_shared<MmioMemoryDevice>(
        sh4_dmac_register_size,
        [dmac](const std::uint32_t offset, const MemoryAccessWidth width) {
            if (width != MemoryAccessWidth::Word) {
                throw std::runtime_error("DMAC-Register erfordern 32-Bit-Zugriffe.");
            }
            if (offset == 0x40u) { return dmac->operation(); }
            const auto index = static_cast<std::size_t>(offset / 0x10u);
            switch (offset % 0x10u) {
                case 0x00u: return dmac->source(index);
                case 0x04u: return dmac->destination(index);
                case 0x08u: return dmac->count(index);
                case 0x0Cu: return dmac->control(index);
                default: throw std::runtime_error("Ungueltiger DMAC-Registeroffset.");
            }
        },
        [dmac](const std::uint32_t offset, const std::uint32_t value, const MemoryAccessWidth width) {
            if (width != MemoryAccessWidth::Word) {
                throw std::runtime_error("DMAC-Register erfordern 32-Bit-Zugriffe.");
            }
            if (offset == 0x40u) { dmac->write_operation(value); return; }
            const auto index = static_cast<std::size_t>(offset / 0x10u);
            switch (offset % 0x10u) {
                case 0x00u: dmac->write_source(index, value); return;
                case 0x04u: dmac->write_destination(index, value); return;
                case 0x08u: dmac->write_count(index, value); return;
                case 0x0Cu: dmac->write_control(index, value); return;
                default: throw std::runtime_error("Ungueltiger DMAC-Registeroffset.");
            }
        }
    );
    memory.map_region("sh4-dmac-p4", sh4_dmac_p4_base, device);
    memory.map_region("sh4-dmac-area7", sh4_dmac_area7_base, device);
    return dmac;
}

} // namespace katana::runtime

#include "katana/runtime/holly_dma.hpp"

#include "katana/runtime/dma.hpp"
#include "katana/runtime/dreamcast_memory.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace katana::runtime {
namespace {
constexpr std::array g2_completion_events{SystemAsicEvent::AicaDma,
                                          SystemAsicEvent::Ext1Dma,
                                          SystemAsicEvent::Ext2Dma,
                                          SystemAsicEvent::DeviceDma};
constexpr std::array g2_illegal_address_events{
    SystemAsicEvent::AicaDmaIllegalAddress,
    SystemAsicEvent::Ext1DmaIllegalAddress,
    SystemAsicEvent::Ext2DmaIllegalAddress,
    SystemAsicEvent::DeviceDmaIllegalAddress};
constexpr std::array g2_overrun_events{SystemAsicEvent::AicaDmaOverrun,
                                       SystemAsicEvent::Ext1DmaOverrun,
                                       SystemAsicEvent::Ext2DmaOverrun,
                                       SystemAsicEvent::DeviceDmaOverrun};
constexpr std::uint32_t holly_dma_transfer_unit_bytes = 32u;

std::uint64_t dma_latency(const std::size_t bytes, const HollyDmaTiming timing) {
    if (bytes == 0u || timing.cycles_per_byte == 0u ||
        bytes > std::numeric_limits<std::uint64_t>::max() / timing.cycles_per_byte)
        throw std::invalid_argument("Holly-DMA-Laenge oder Timing ist ungueltig.");
    return static_cast<std::uint64_t>(bytes) * timing.cycles_per_byte;
}

void transfer(Memory& memory,
              const std::uint32_t source,
              const std::uint32_t destination,
              const std::size_t bytes) {
    memory.copy_bytes(destination, source, bytes, CodeWriteSource::Dma);
}

bool protected_range(const std::uint32_t protection,
                     const std::uint32_t address,
                     const std::size_t size) noexcept {
    if (size == 0u) return false;
    const auto bottom = (((protection >> 8u) & 0x7Fu) << 20u) | 0x08000000u;
    const auto top = ((protection & 0x7Fu) << 20u) | 0x080FFFFFu;
    const auto physical = address & 0x1FFFFFFFu;
    if (size - 1u > std::numeric_limits<std::uint32_t>::max() - physical) return false;
    return physical >= bottom && physical + static_cast<std::uint32_t>(size - 1u) <= top;
}

std::shared_ptr<MmioMemoryDevice>
make_word_device(const std::size_t size,
                 const std::function<std::uint32_t(std::uint32_t)>& read,
                 const std::function<void(std::uint32_t, std::uint32_t)>& write,
                 const char* description) {
    return std::make_shared<MmioMemoryDevice>(
        size,
        [read, description](const auto offset, const auto width) {
            if (width != MemoryAccessWidth::Word)
                throw std::runtime_error(std::string(description) + " erfordert 32-Bit-MMIO.");
            return read(offset);
        },
        [write, description](const auto offset, const auto value, const auto width) {
            if (width != MemoryAccessWidth::Word)
                throw std::runtime_error(std::string(description) + " erfordert 32-Bit-MMIO.");
            write(offset, value);
        });
}

void map_direct(Memory& memory,
                const std::string& name,
                const std::uint32_t physical_base,
                const std::shared_ptr<MemoryDevice>& device) {
    for (const auto segment : dreamcast_direct_segment_bases)
        memory.map_region(name + "-" + std::to_string(segment), segment + physical_base, device);
}
} // namespace

DreamcastG2DmaController::DreamcastG2DmaController(
    Memory& memory,
    EventScheduler& scheduler,
    const HollyDmaTiming timing,
    std::function<void(SystemAsicEvent)> completion_observer)
    : memory_(memory), scheduler_(scheduler), timing_(timing),
      completion_observer_(std::move(completion_observer)),
      scheduler_lifetime_(scheduler.lifetime_token()) {
    if (timing_.cycles_per_byte == 0u)
        throw std::invalid_argument("G2-DMA braucht positive Zyklen pro Byte.");
    reset_observer_ = scheduler_.add_reset_observer([this] { handle_scheduler_reset(); });
    reset();
}

DreamcastG2DmaController::~DreamcastG2DmaController() {
    if (scheduler_lifetime_.expired()) return;
    cancel_events();
    static_cast<void>(scheduler_.remove_reset_observer(reset_observer_));
}

std::uint32_t DreamcastG2DmaController::read(const std::uint32_t offset) const {
    if (offset < 0x80u) {
        const auto channel = static_cast<std::size_t>(offset / 0x20u);
        const auto reg = offset % 0x20u;
        const auto& state = channels_[channel];
        switch (reg) {
        case 0x00u:
            return state.peripheral_address;
        case 0x04u:
            return state.system_address;
        case 0x08u:
            return state.length;
        case 0x0Cu:
            return state.direction;
        case 0x10u:
            return state.trigger_select;
        case 0x14u:
            return state.enabled;
        case 0x18u:
            return state.active;
        case 0x1Cu:
            return state.suspend;
        default:
            break;
        }
    }
    if (offset == 0x80u) return 0x12u;
    if (offset == 0x90u) return ds_timeout_;
    if (offset == 0x94u) return tr_timeout_;
    if (offset == 0x98u) return modem_timeout_;
    if (offset == 0x9Cu) return modem_wait_;
    if (offset >= 0xC0u && offset <= 0xF8u && (offset & 3u) == 0u) {
        const auto relative = offset - 0xC0u;
        const auto channel = static_cast<std::size_t>(relative / 0x10u);
        const auto reg = relative % 0x10u;
        if (channel < channels_.size()) {
            if (reg == 0u) return channels_[channel].peripheral_counter;
            if (reg == 4u) return channels_[channel].system_counter;
            if (reg == 8u) return channels_[channel].remaining;
        }
    }
    throw std::runtime_error("Unbekannter oder nicht lesbarer G2-DMA-MMIO-Offset.");
}

void DreamcastG2DmaController::write(const std::uint32_t offset, const std::uint32_t value) {
    if (offset < 0x80u) {
        const auto channel = static_cast<std::size_t>(offset / 0x20u);
        const auto reg = offset % 0x20u;
        auto& state = channels_[channel];
        switch (reg) {
        case 0x00u:
            state.peripheral_address = value & 0x1FFFFFE0u;
            return;
        case 0x04u:
            state.system_address = value & 0x1FFFFFE0u;
            return;
        case 0x08u:
            state.length = value & 0x81FFFFE0u;
            return;
        case 0x0Cu:
            state.direction = value & 1u;
            return;
        case 0x10u:
            state.trigger_select = value & 7u;
            return;
        case 0x14u:
            state.enabled = value & 1u;
            if (state.enabled == 0u) {
                if (state.completion_event)
                    static_cast<void>(scheduler_.cancel(*state.completion_event));
                state.completion_event.reset();
                state.completion_event_rehydration_pending = false;
                state.active = 0u;
                state.completion_cycle = 0u;
                state.remaining_cycles = 0u;
                state.suspend &= 1u;
            }
            return;
        case 0x18u:
            if ((value & 1u) != 0u && state.enabled != 0u) arm(channel);
            return;
        case 0x1Cu:
            set_suspended(channel, (value & 1u) != 0u);
            return;
        default:
            break;
        }
    }
    if (offset == 0x90u) {
        ds_timeout_ = value;
        return;
    }
    if (offset == 0x94u) {
        tr_timeout_ = value;
        return;
    }
    if (offset == 0x98u) {
        modem_timeout_ = value & 0xFFu;
        return;
    }
    if (offset == 0x9Cu) {
        modem_wait_ = value & 0xFFu;
        return;
    }
    if (offset >= 0xA0u && offset <= 0xB8u && (offset & 3u) == 0u) {
        if (value != 0u)
            throw std::runtime_error("Reserviertes G2-Bootregister akzeptiert nur Null.");
        return;
    }
    if (offset == 0xBCu) {
        if ((value >> 16u) == 0x4659u) address_protect_ = value & 0x00007F7Fu;
        return;
    }
    throw std::runtime_error("Unbekannter oder nicht schreibbarer G2-DMA-MMIO-Offset.");
}

bool DreamcastG2DmaController::protected_system_range(const std::uint32_t address,
                                                      const std::size_t size) const noexcept {
    return protected_range(address_protect_, address, size) && memory_.contains(address, size);
}

void DreamcastG2DmaController::arm(const std::size_t channel) {
    auto& state = channels_.at(channel);
    state.fault = HollyDmaFaultReason::None;
    if (state.active != 0u || state.completion_event) {
        fail(channel, HollyDmaFaultReason::Overrun, g2_overrun_events[channel]);
        return;
    }
    const auto bytes = static_cast<std::size_t>(state.length & 0x7FFFFFFFu);
    if (bytes == 0u) {
        fail(channel, HollyDmaFaultReason::InvalidLength, g2_illegal_address_events[channel]);
        return;
    }
    if (!protected_system_range(state.system_address, bytes)) {
        fail(channel, HollyDmaFaultReason::Overrun, g2_overrun_events[channel]);
        return;
    }
    auto source = state.system_address;
    auto destination = state.peripheral_address;
    if (state.direction != 0u) std::swap(source, destination);
    if (!memory_.contains(source, bytes) || !memory_.contains(destination, bytes)) {
        fail(channel, HollyDmaFaultReason::IllegalAddress, g2_illegal_address_events[channel]);
        return;
    }
    state.active = 1u;
    state.peripheral_counter = state.peripheral_address;
    state.system_counter = state.system_address;
    state.remaining = static_cast<std::uint32_t>(bytes);
    state.remaining_cycles = 0u;
    switch (state.trigger_select & 3u) {
    case 0u:
        start(channel);
        return;
    case 1u:
        if (hardware_request_probes_[channel] &&
            hardware_request_probes_[channel]())
            start(channel);
        return;
    case 2u:
        return;
    default:
        fail(channel, HollyDmaFaultReason::InvalidTrigger, g2_illegal_address_events[channel]);
        return;
    }
}

void DreamcastG2DmaController::start(const std::size_t channel) {
    auto& state = channels_.at(channel);
    if (state.active == 0u || state.completion_event) {
        fail(channel, HollyDmaFaultReason::Overrun, g2_overrun_events[channel]);
        return;
    }
    std::uint64_t cycles = state.remaining_cycles;
    if (cycles == 0u) {
        try {
            cycles = dma_latency(
                std::min<std::size_t>(state.remaining, transfer_chunk_bytes), timing_);
        } catch (...) {
            fail(channel, HollyDmaFaultReason::SchedulerFailure, std::nullopt);
            return;
        }
    }
    if ((state.trigger_select & 4u) != 0u && (state.suspend & 1u) != 0u) {
        state.suspend |= 0x10u;
        state.remaining_cycles = cycles;
        return;
    }
    state.suspend &= ~0x10u;
    schedule_completion(channel, cycles);
}

void DreamcastG2DmaController::schedule_completion(const std::size_t channel,
                                                   const std::uint64_t cycles) {
    auto& state = channels_.at(channel);
    if (cycles == 0u || cycles > std::numeric_limits<std::uint64_t>::max() -
                                      scheduler_.current_cycle()) {
        fail(channel, HollyDmaFaultReason::SchedulerFailure, std::nullopt);
        return;
    }
    state.remaining_cycles = 0u;
    state.completion_cycle = scheduler_.current_cycle() + cycles;
    try {
        state.completion_event = scheduler_.schedule_after(
            cycles,
            [this, channel](const auto event_id, const auto) { complete(channel, event_id); },
            SchedulerEventKind::HollyG2Dma);
        state.completion_event_rehydration_pending = false;
    } catch (...) {
        fail(channel, HollyDmaFaultReason::SchedulerFailure, std::nullopt);
    }
}

void DreamcastG2DmaController::set_suspended(const std::size_t channel,
                                             const bool suspended) {
    auto& state = channels_.at(channel);
    state.suspend = (state.suspend & ~1u) | (suspended ? 1u : 0u);
    if ((state.trigger_select & 4u) == 0u) {
        state.suspend &= ~0x10u;
        return;
    }
    if (suspended && state.completion_event) {
        const auto now = scheduler_.current_cycle();
        state.remaining_cycles = state.completion_cycle > now ? state.completion_cycle - now : 1u;
        static_cast<void>(scheduler_.cancel(*state.completion_event));
        state.completion_event.reset();
        state.completion_event_rehydration_pending = false;
        state.completion_cycle = 0u;
        state.suspend |= 0x10u;
    } else if (!suspended && state.active != 0u && (state.suspend & 0x10u) != 0u) {
        state.suspend &= ~0x10u;
        start(channel);
    }
}

void DreamcastG2DmaController::complete(const std::size_t channel,
                                        const SchedulerEventId event_id) {
    auto& state = channels_.at(channel);
    if (!state.completion_event || *state.completion_event != event_id || state.active == 0u) {
        fail(channel, HollyDmaFaultReason::SchedulerFailure, std::nullopt);
        return;
    }
    state.completion_event.reset();
    state.completion_event_rehydration_pending = false;
    state.completion_cycle = 0u;
    state.remaining_cycles = 0u;
    const auto chunk =
        std::min<std::uint32_t>(state.remaining, transfer_chunk_bytes);
    std::uint32_t transferred = 0u;
    while (transferred != chunk) {
        const auto unit =
            std::min<std::uint32_t>(chunk - transferred, holly_dma_transfer_unit_bytes);
        auto source = state.system_counter;
        auto destination = state.peripheral_counter;
        if (state.direction != 0u) std::swap(source, destination);
        try {
            transfer(memory_, source, destination, unit);
        } catch (...) {
            fail(channel,
                 HollyDmaFaultReason::TransferFailure,
                 g2_illegal_address_events[channel]);
            return;
        }
        state.peripheral_counter += unit;
        state.system_counter += unit;
        state.remaining -= unit;
        transferred += unit;
    }
    if (state.remaining != 0u) {
        start(channel);
        return;
    }
    state.enabled = (state.length & 0x80000000u) != 0u ? 0u : 1u;
    state.length = 0u;
    state.active = 0u;
    state.suspend |= 0x10u;
    ++completed_dma_count_;
    if (completion_observer_) completion_observer_(g2_completion_events[channel]);
}

void DreamcastG2DmaController::fail(const std::size_t channel,
                                    const HollyDmaFaultReason reason,
                                    const std::optional<SystemAsicEvent> event) noexcept {
    if (channel >= channels_.size()) return;
    auto& state = channels_[channel];
    const auto in_flight = state.active != 0u;
    if (state.completion_event && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_.cancel(*state.completion_event));
    state.completion_event.reset();
    state.completion_event_rehydration_pending = false;
    state.active = 0u;
    state.enabled = 0u;
    state.completion_cycle = 0u;
    state.remaining_cycles = 0u;
    state.fault = reason;
    ++state.fault_count;
    last_fault_ = HollyDmaFault{reason,
                                event,
                                channel,
                                in_flight ? state.peripheral_counter
                                          : state.peripheral_address,
                                in_flight ? state.system_counter : state.system_address,
                                in_flight ? state.remaining
                                          : (state.length & 0x7FFFFFFFu)};
    if (event && completion_observer_) {
        try {
            completion_observer_(*event);
        } catch (...) {
        }
    }
}

void DreamcastG2DmaController::set_hardware_request_probe(
    const std::size_t channel,
    std::function<bool()> probe) {
    if (channel >= hardware_request_probes_.size())
        throw std::out_of_range("Ungueltiger G2-DMA-Hardwaretriggerkanal.");
    hardware_request_probes_[channel] = std::move(probe);
}

void DreamcastG2DmaController::hardware_trigger(const std::size_t channel) {
    auto& state = channels_.at(channel);
    if (state.active == 0u || state.completion_event || (state.trigger_select & 3u) != 1u)
        return;
    start(channel);
}

void DreamcastG2DmaController::interrupt_trigger(const SystemAsicEvent) {
    for (std::size_t channel = 0u; channel < channels_.size(); ++channel) {
        auto& state = channels_[channel];
        if (state.active != 0u && !state.completion_event &&
            (state.trigger_select & 3u) == 2u)
            start(channel);
    }
}

void DreamcastG2DmaController::cancel_events() noexcept {
    for (auto& channel : channels_) {
        if (channel.completion_event)
            static_cast<void>(scheduler_.cancel(*channel.completion_event));
        channel.completion_event.reset();
        channel.completion_event_rehydration_pending = false;
    }
}

void DreamcastG2DmaController::handle_scheduler_reset() noexcept {
    for (auto& channel : channels_) {
        channel.completion_event.reset();
        channel.completion_event_rehydration_pending = false;
        channel.active = 0u;
        channel.completion_cycle = 0u;
        channel.remaining_cycles = 0u;
    }
}

void DreamcastG2DmaController::reset() noexcept {
    if (!scheduler_lifetime_.expired()) cancel_events();
    channels_ = {};
    address_protect_ = 0x00007F00u;
    ds_timeout_ = 0u;
    tr_timeout_ = 0u;
    modem_timeout_ = 0u;
    modem_wait_ = 0u;
    completed_dma_count_ = 0u;
    last_fault_.reset();
}

std::uint64_t DreamcastG2DmaController::completed_dma_count() const noexcept {
    return completed_dma_count_;
}

const HollyDmaChannelState&
DreamcastG2DmaController::channel_state(const std::size_t channel) const {
    if (channel >= channels_.size()) throw std::out_of_range("Ungueltiger G2-DMA-Kanal.");
    return channels_[channel];
}

const std::optional<HollyDmaFault>& DreamcastG2DmaController::last_fault() const noexcept {
    return last_fault_;
}

DreamcastG2DmaSnapshot DreamcastG2DmaController::snapshot() const {
    return {
        channels_,
        timing_,
        address_protect_,
        ds_timeout_,
        tr_timeout_,
        modem_timeout_,
        modem_wait_,
        completed_dma_count_,
        last_fault_,
        reset_observer_,
        static_cast<bool>(completion_observer_),
    };
}

void DreamcastG2DmaController::validate_state_restore(
    const DreamcastG2DmaSnapshot& state) const {
    if (state.timing != timing_ ||
        state.completion_observer_bound !=
            static_cast<bool>(completion_observer_))
        throw std::invalid_argument(
            "G2-DMA-Handoff passt nicht zum Runtime-Vertrag.");
    for (std::size_t index = 0u; index < state.channels.size(); ++index) {
        const auto& channel = state.channels[index];
        if (channel.direction > 1u || channel.trigger_select > 7u ||
            channel.enabled > 1u || channel.active > 1u ||
            (channel.suspend & ~0x11u) != 0u ||
            static_cast<std::uint8_t>(channel.fault) >
                static_cast<std::uint8_t>(
                    HollyDmaFaultReason::HandshakeMismatch) ||
            (channel.completion_event &&
             channel.completion_event_rehydration_pending))
            throw std::invalid_argument(
                "G2-DMA-Handoff besitzt ungueltige Kanaldaten.");
        const auto scheduled =
            channel.completion_event.has_value() ||
            channel.completion_event_rehydration_pending;
        if (scheduled) {
            if (channel.active == 0u || channel.completion_cycle == 0u ||
                channel.remaining_cycles != 0u)
                throw std::invalid_argument(
                    "G2-DMA-Handoff besitzt ein ungueltiges Completionevent.");
        } else if (channel.completion_cycle != 0u) {
            throw std::invalid_argument(
                "G2-DMA-Handoff besitzt einen Zyklus ohne Completionevent.");
        }
        if (channel.active != 0u) {
            if (channel.remaining == 0u ||
                !protected_range(state.address_protect,
                                 channel.system_counter,
                                 channel.remaining) ||
                !memory_.contains(channel.system_counter,
                                  channel.remaining) ||
                !memory_.contains(channel.peripheral_counter,
                                  channel.remaining))
                throw std::invalid_argument(
                    "G2-DMA-Handoff passt nicht zum Runtime-Speichervertrag.");
        }
    }
    if (state.last_fault &&
        (state.last_fault->channel >= state.channels.size() ||
         static_cast<std::uint8_t>(state.last_fault->reason) >
             static_cast<std::uint8_t>(
                 HollyDmaFaultReason::HandshakeMismatch)))
        throw std::invalid_argument(
            "G2-DMA-Handoff besitzt einen ungueltigen Fehlerzustand.");
}

void DreamcastG2DmaController::restore_state_passive(
    const DreamcastG2DmaSnapshot& state) {
    validate_state_restore(state);
    cancel_events();
    channels_ = state.channels;
    for (auto& channel : channels_) {
        const auto pending =
            channel.completion_event.has_value() ||
            channel.completion_event_rehydration_pending;
        channel.completion_event.reset();
        channel.completion_event_rehydration_pending = pending;
    }
    address_protect_ = state.address_protect;
    ds_timeout_ = state.ds_timeout;
    tr_timeout_ = state.tr_timeout;
    modem_timeout_ = state.modem_timeout;
    modem_wait_ = state.modem_wait;
    completed_dma_count_ = state.completed_dma_count;
    last_fault_ = state.last_fault;
}

SchedulerEventId DreamcastG2DmaController::rehydrate_scheduled_event(
    const std::uint64_t guest_cycle,
    const std::uint32_t channel,
    const std::uint64_t token) {
    if (channel >= channels_.size() ||
        token != dreamcast_holly_dma_event_token_v1)
        throw std::invalid_argument(
            "G2-DMA-Handoff besitzt einen unbekannten Eventkanal oder Token.");
    auto& state = channels_[channel];
    if (!state.completion_event_rehydration_pending ||
        state.completion_event || state.active == 0u)
        throw std::logic_error(
            "G2-DMA-Handoff erwartet dieses Completionevent nicht.");
    if (guest_cycle != state.completion_cycle ||
        guest_cycle < scheduler_.current_cycle())
        throw std::invalid_argument(
            "G2-DMA-Completion passt nicht zur gespeicherten Gastzeit.");
    const auto event_id = scheduler_.schedule_at(
        guest_cycle,
        [this, channel](const auto restored_event_id, const auto) {
            complete(channel, restored_event_id);
        },
        SchedulerEventKind::HollyG2Dma);
    state.completion_event = event_id;
    state.completion_event_rehydration_pending = false;
    return event_id;
}

bool DreamcastG2DmaController::event_rehydration_pending() const noexcept {
    return std::any_of(channels_.begin(), channels_.end(), [](const auto& channel) {
        return channel.completion_event_rehydration_pending;
    });
}

DreamcastG1BusController::DreamcastG1BusController(
    EventScheduler& scheduler,
    const HollyDmaTiming timing,
    TransferHandler transfer_handler,
    std::function<void(SystemAsicEvent)> completion_observer,
    RangeValidator range_validator)
    : scheduler_(scheduler), timing_(timing), transfer_handler_(std::move(transfer_handler)),
      completion_observer_(std::move(completion_observer)),
      range_validator_(std::move(range_validator)),
      scheduler_lifetime_(scheduler.lifetime_token()) {
    if (timing_.cycles_per_byte == 0u)
        throw std::invalid_argument("G1-DMA braucht positive Zyklen pro Byte.");
    reset_observer_ = scheduler_.add_reset_observer([this] { handle_scheduler_reset(); });
    reset();
}

DreamcastG1BusController::~DreamcastG1BusController() {
    if (scheduler_lifetime_.expired()) return;
    if (completion_event_) static_cast<void>(scheduler_.cancel(*completion_event_));
    static_cast<void>(scheduler_.remove_reset_observer(reset_observer_));
}

std::uint32_t DreamcastG1BusController::read(const std::uint32_t offset) const {
    switch (offset) {
    case 0x04u:
        return configured_address_;
    case 0x08u:
        return configured_length_;
    case 0x0Cu:
        return dma_direction_;
    case 0x14u:
        return dma_enabled_;
    case 0x18u:
        return dma_active_;
    case 0xB0u:
        return system_mode_;
    case 0xF4u:
        return live_address_;
    case 0xF8u:
        return transferred_length_;
    default:
        throw std::runtime_error("Unbekannter oder nicht lesbarer G1-MMIO-Offset.");
    }
}

void DreamcastG1BusController::write(const std::uint32_t offset, const std::uint32_t value) {
    switch (offset) {
    case 0x04u:
        configured_address_ = value & 0x1FFFFFE0u;
        return;
    case 0x08u:
        configured_length_ = value & 0x01FFFFFFu;
        return;
    case 0x0Cu:
        dma_direction_ = value & 1u;
        return;
    case 0x14u:
        dma_enabled_ = value & 1u;
        if (dma_enabled_ == 0u) abort_transfer();
        return;
    case 0x18u:
        if ((value & 1u) != 0u && dma_enabled_ != 0u)
            static_cast<void>(
                begin_transfer(configured_address_, configured_length_, dma_direction_));
        return;
    case 0x80u:
    case 0x84u:
    case 0x88u:
    case 0x8Cu:
    case 0x90u:
    case 0x94u:
    case 0xA4u:
    case 0xB4u:
    case 0xE4u:
        return;
    case 0xA0u:
        gdrom_read_access_timing_ = value;
        return;
    case 0xB8u:
        if ((value >> 16u) == address_protect_key)
            address_protect_ = value & 0x00007F7Fu;
        return;
    default:
        throw std::runtime_error("Unbekannter oder nicht schreibbarer G1-MMIO-Offset.");
    }
}

bool DreamcastG1BusController::begin_transfer(const std::uint32_t address,
                                              const std::uint32_t length,
                                              const std::uint32_t direction) {
    fault_ = HollyDmaFaultReason::None;
    if (dma_active_ != 0u || completion_event_) {
        fail(HollyDmaFaultReason::Overrun, SystemAsicEvent::GdromOverrun);
        return false;
    }
    const auto encoded_length = length & 0x01FFFFFFu;
    const auto effective_length = encoded_length == 0u ? maximum_transfer_bytes : encoded_length;
    configured_address_ = address & 0x1FFFFFE0u;
    configured_length_ = encoded_length;
    dma_direction_ = direction;
    live_address_ = configured_address_;
    transferred_length_ = 0u;
    remaining_length_ = effective_length;
    if ((effective_length & (transfer_alignment - 1u)) != 0u) {
        fail(HollyDmaFaultReason::InvalidLength, SystemAsicEvent::GdromIllegalAddress);
        return false;
    }
    if ((address & (transfer_alignment - 1u)) != 0u) {
        fail(HollyDmaFaultReason::IllegalAddress,
             SystemAsicEvent::GdromIllegalAddress,
             G1DmaFaultPhase::Start,
             address);
        return false;
    }
    if (direction != 1u) {
        fail(HollyDmaFaultReason::InvalidDirection, SystemAsicEvent::GdromIllegalAddress);
        return false;
    }
    if (static_cast<std::uint64_t>(address) + effective_length - 1u >
            std::numeric_limits<std::uint32_t>::max() ||
        !protected_system_range(configured_address_, effective_length) ||
        (range_validator_ && !range_validator_(configured_address_, effective_length))) {
        fail(HollyDmaFaultReason::IllegalAddress, SystemAsicEvent::GdromIllegalAddress);
        return false;
    }
    if (!transfer_handler_) {
        fail(HollyDmaFaultReason::MissingBackend);
        return false;
    }
    dma_enabled_ = 1u;
    dma_active_ = 1u;
    schedule_chunk(G1DmaFaultPhase::Start);
    return dma_active_ != 0u;
}

void DreamcastG1BusController::schedule_chunk(const G1DmaFaultPhase failure_phase) {
    const auto chunk = std::min(remaining_length_, transfer_chunk_bytes);
    try {
        const auto cycles = dma_latency(chunk, timing_);
        next_chunk_cycle_ = scheduler_.current_cycle() + cycles;
        completion_event_ = scheduler_.schedule_after(
            cycles,
            [this](const auto event_id, const auto) { complete_chunk(event_id); },
            SchedulerEventKind::HollyG1Dma);
    } catch (...) {
        fail(HollyDmaFaultReason::SchedulerFailure, std::nullopt, failure_phase);
    }
}

void DreamcastG1BusController::complete_chunk(const SchedulerEventId event_id) {
    if (!completion_event_ || *completion_event_ != event_id || dma_active_ == 0u) {
        fail(HollyDmaFaultReason::SchedulerFailure,
             std::nullopt,
             G1DmaFaultPhase::Chunk);
        return;
    }
    completion_event_.reset();
    next_chunk_cycle_ = 0u;
    const auto chunk = std::min(remaining_length_, transfer_chunk_bytes);
    try {
        transfer_handler_(live_address_, chunk, dma_direction_);
    } catch (...) {
        fail(HollyDmaFaultReason::TransferFailure,
             std::nullopt,
             G1DmaFaultPhase::Chunk);
        return;
    }
    live_address_ += chunk;
    transferred_length_ += chunk;
    remaining_length_ -= chunk;
    if (remaining_length_ != 0u) {
        schedule_chunk(G1DmaFaultPhase::Chunk);
        return;
    }
    dma_active_ = 0u;
    if (completion_observer_) completion_observer_(SystemAsicEvent::GdromDma);
}

void DreamcastG1BusController::abort_transfer() noexcept {
    if (completion_event_ && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_.cancel(*completion_event_));
    completion_event_.reset();
    next_chunk_cycle_ = 0u;
    dma_active_ = 0u;
    remaining_length_ = 0u;
}

void DreamcastG1BusController::fail(const HollyDmaFaultReason reason,
                                    const std::optional<SystemAsicEvent> event,
                                    const G1DmaFaultPhase phase,
                                    const std::optional<std::uint32_t> fault_address_override)
    noexcept {
    const auto fault_address = fault_address_override.value_or(
        dma_active_ != 0u ? live_address_ : configured_address_);
    const auto fault_remaining =
        dma_active_ != 0u
            ? remaining_length_
            : (configured_length_ == 0u ? maximum_transfer_bytes : configured_length_);
    if (completion_event_ && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_.cancel(*completion_event_));
    completion_event_.reset();
    next_chunk_cycle_ = 0u;
    dma_active_ = 0u;
    remaining_length_ = 0u;
    dma_enabled_ = 0u;
    fault_ = reason;
    ++fault_count_;
    last_fault_ = HollyDmaFault{reason, event, 0u, 0u, fault_address, fault_remaining};
    last_g1_fault_ =
        G1DmaFault{reason, fault_address, transferred_length_, fault_remaining, phase};
    if (event && completion_observer_) {
        try {
            completion_observer_(*event);
        } catch (...) {
        }
    }
    if (fault_observer_) {
        try {
            fault_observer_(*last_g1_fault_);
        } catch (...) {
        }
    }
}

bool DreamcastG1BusController::protected_system_range(const std::uint32_t address,
                                                      const std::uint32_t size) const noexcept {
    if (size == 0u) return false;
    const auto start_page = (address_protect_ >> 8u) & 0x7Fu;
    const auto end_page = address_protect_ & 0x7Fu;
    if (start_page > end_page) return false;
    const auto bottom = 0x08000000u | (start_page << 20u);
    const auto top = 0x080FFFFFu | (end_page << 20u);
    const auto physical = address & 0x1FFFFFFFu;
    const auto physical_end = static_cast<std::uint64_t>(physical) + size - 1u;
    return physical >= bottom && physical_end <= top;
}

void DreamcastG1BusController::handle_scheduler_reset() noexcept {
    completion_event_.reset();
    next_chunk_cycle_ = 0u;
    dma_active_ = 0u;
    remaining_length_ = 0u;
}

void DreamcastG1BusController::configure_bios_handoff(const std::uint32_t live_address) noexcept {
    bios_handoff_live_address_ = live_address & 0x1FFFFFE0u;
    restore_bios_handoff();
}

void DreamcastG1BusController::restore_bios_handoff() noexcept {
    live_address_ = bios_handoff_live_address_;
    transferred_length_ = 0u;
    remaining_length_ = 0u;
}

void DreamcastG1BusController::reset() noexcept {
    abort_transfer();
    configured_address_ = 0u;
    configured_length_ = 0u;
    live_address_ = 0u;
    transferred_length_ = 0u;
    remaining_length_ = 0u;
    bios_handoff_live_address_ = 0u;
    dma_direction_ = 0u;
    dma_enabled_ = 0u;
    dma_active_ = 0u;
    system_mode_ = 1u;
    gdrom_read_access_timing_ = 0u;
    address_protect_ = 0x0000407Fu;
    fault_ = HollyDmaFaultReason::None;
    fault_count_ = 0u;
    last_fault_.reset();
    last_g1_fault_.reset();
}

HollyDmaChannelState DreamcastG1BusController::state() const noexcept {
    HollyDmaChannelState result;
    result.system_address = configured_address_;
    result.length = configured_length_;
    result.direction = dma_direction_;
    result.enabled = dma_enabled_;
    result.active = dma_active_;
    result.peripheral_counter = transferred_length_;
    result.system_counter = live_address_;
    result.remaining = remaining_length_;
    result.completion_cycle = next_chunk_cycle_;
    result.remaining_cycles = next_chunk_cycle_ > scheduler_.current_cycle()
                                  ? next_chunk_cycle_ - scheduler_.current_cycle()
                                  : 0u;
    result.completion_event = completion_event_;
    result.completion_event_rehydration_pending =
        event_rehydration_pending();
    result.fault = fault_;
    result.fault_count = fault_count_;
    return result;
}

const std::optional<HollyDmaFault>& DreamcastG1BusController::last_fault() const noexcept {
    return last_fault_;
}

const std::optional<G1DmaFault>& DreamcastG1BusController::last_g1_fault() const noexcept {
    return last_g1_fault_;
}

void DreamcastG1BusController::set_fault_observer(FaultObserver observer) {
    fault_observer_ = std::move(observer);
}

std::uint32_t DreamcastG1BusController::gdrom_read_access_timing() const noexcept {
    return gdrom_read_access_timing_;
}

std::uint32_t DreamcastG1BusController::address_protect() const noexcept {
    return address_protect_;
}

DreamcastG1DmaSnapshot DreamcastG1BusController::snapshot() const noexcept {
    return {
        state(),
        timing_,
        bios_handoff_live_address_,
        system_mode_,
        gdrom_read_access_timing_,
        address_protect_,
        last_fault_,
        last_g1_fault_,
        reset_observer_,
        static_cast<bool>(transfer_handler_),
        static_cast<bool>(completion_observer_),
        static_cast<bool>(range_validator_),
        static_cast<bool>(fault_observer_),
    };
}

void DreamcastG1BusController::validate_state_restore(
    const DreamcastG1DmaSnapshot& state) const {
    if (state.timing != timing_ ||
        state.transfer_handler_bound !=
            static_cast<bool>(transfer_handler_) ||
        state.completion_observer_bound !=
            static_cast<bool>(completion_observer_) ||
        state.range_validator_bound !=
            static_cast<bool>(range_validator_) ||
        state.fault_observer_bound != static_cast<bool>(fault_observer_))
        throw std::invalid_argument(
            "G1-DMA-Handoff passt nicht zum Runtime-Vertrag.");
    const auto& channel = state.channel;
    if (channel.direction > 1u || channel.enabled > 1u ||
        channel.active > 1u ||
        static_cast<std::uint8_t>(channel.fault) >
            static_cast<std::uint8_t>(
                HollyDmaFaultReason::HandshakeMismatch) ||
        (channel.completion_event &&
         channel.completion_event_rehydration_pending))
        throw std::invalid_argument(
            "G1-DMA-Handoff besitzt ungueltige Kanaldaten.");
    const auto scheduled =
        channel.completion_event.has_value() ||
        channel.completion_event_rehydration_pending;
    if (channel.active != 0u) {
        const auto start_page =
            (state.address_protect >> 8u) & 0x7Fu;
        const auto end_page = state.address_protect & 0x7Fu;
        const auto bottom =
            0x08000000u | (start_page << 20u);
        const auto top = 0x080FFFFFu | (end_page << 20u);
        const auto physical =
            channel.system_counter & 0x1FFFFFFFu;
        const auto physical_end =
            static_cast<std::uint64_t>(physical) +
            channel.remaining - 1u;
        if (!scheduled || channel.completion_cycle == 0u ||
            channel.remaining_cycles > channel.completion_cycle ||
            channel.remaining == 0u ||
            channel.system_counter !=
                channel.system_address + channel.peripheral_counter ||
            start_page > end_page || physical < bottom ||
            physical_end > top)
            throw std::invalid_argument(
                "G1-DMA-Handoff besitzt keinen fortsetzbaren Transfer.");
    } else if (scheduled || channel.completion_cycle != 0u ||
               channel.remaining != 0u) {
        throw std::invalid_argument(
            "G1-DMA-Handoff besitzt Schedulingdaten ohne aktiven Transfer.");
    }
    if (state.last_g1_fault &&
        static_cast<std::uint8_t>(state.last_g1_fault->phase) >
            static_cast<std::uint8_t>(G1DmaFaultPhase::Chunk))
        throw std::invalid_argument(
            "G1-DMA-Handoff besitzt eine ungueltige Fehlerphase.");
}

void DreamcastG1BusController::restore_state_passive(
    const DreamcastG1DmaSnapshot& state) {
    validate_state_restore(state);
    if (completion_event_ && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_.cancel(*completion_event_));
    completion_event_.reset();
    configured_address_ = state.channel.system_address;
    configured_length_ = state.channel.length;
    live_address_ = state.channel.system_counter;
    transferred_length_ = state.channel.peripheral_counter;
    remaining_length_ = state.channel.remaining;
    bios_handoff_live_address_ = state.bios_handoff_live_address;
    dma_direction_ = state.channel.direction;
    dma_enabled_ = state.channel.enabled;
    dma_active_ = state.channel.active;
    system_mode_ = state.system_mode;
    gdrom_read_access_timing_ = state.gdrom_read_access_timing;
    address_protect_ = state.address_protect;
    next_chunk_cycle_ = state.channel.completion_cycle;
    fault_ = state.channel.fault;
    fault_count_ = state.channel.fault_count;
    last_fault_ = state.last_fault;
    last_g1_fault_ = state.last_g1_fault;
    // The absence of a local event ID while next_chunk_cycle_ is non-zero is
    // the passive rehydration marker for G1.
}

SchedulerEventId DreamcastG1BusController::rehydrate_scheduled_event(
    const std::uint64_t guest_cycle,
    const std::uint32_t channel,
    const std::uint64_t token) {
    if (channel != 0u ||
        token != dreamcast_holly_dma_event_token_v1)
        throw std::invalid_argument(
            "G1-DMA-Handoff besitzt einen unbekannten Eventkanal oder Token.");
    if (completion_event_ || dma_active_ == 0u ||
        next_chunk_cycle_ == 0u)
        throw std::logic_error(
            "G1-DMA-Handoff erwartet kein Completionevent.");
    if (guest_cycle != next_chunk_cycle_ ||
        guest_cycle < scheduler_.current_cycle())
        throw std::invalid_argument(
            "G1-DMA-Completion passt nicht zur gespeicherten Gastzeit.");
    const auto event_id = scheduler_.schedule_at(
        guest_cycle,
        [this](const auto restored_event_id, const auto) {
            complete_chunk(restored_event_id);
        },
        SchedulerEventKind::HollyG1Dma);
    completion_event_ = event_id;
    return event_id;
}

bool DreamcastG1BusController::event_rehydration_pending() const noexcept {
    return dma_active_ != 0u && next_chunk_cycle_ != 0u &&
           !completion_event_;
}

DreamcastPvrDmaController::DreamcastPvrDmaController(
    Memory& memory,
    EventScheduler& scheduler,
    const HollyDmaTiming timing,
    std::function<void(SystemAsicEvent)> completion_observer)
    : memory_(memory), scheduler_(scheduler), timing_(timing),
      completion_observer_(std::move(completion_observer)),
      scheduler_lifetime_(scheduler.lifetime_token()) {
    if (timing_.cycles_per_byte == 0u)
        throw std::invalid_argument("PVR-DMA braucht positive Zyklen pro Byte.");
    reset_observer_ = scheduler_.add_reset_observer([this] { handle_scheduler_reset(); });
    reset();
}

DreamcastPvrDmaController::~DreamcastPvrDmaController() {
    if (scheduler_lifetime_.expired()) return;
    cancel();
    static_cast<void>(scheduler_.remove_reset_observer(reset_observer_));
}

std::uint32_t DreamcastPvrDmaController::read(const std::uint32_t offset) const {
    switch (offset) {
    case 0x00u:
        return pvr_address_;
    case 0x04u:
        return system_address_;
    case 0x08u:
        return length_;
    case 0x0Cu:
        return direction_;
    case 0x10u:
        return trigger_select_;
    case 0x14u:
        return enabled_;
    case 0x18u:
        return active_;
    case 0xF0u:
        return pvr_counter_;
    case 0xF4u:
        return system_counter_;
    case 0xF8u:
        return remaining_;
    default:
        throw std::runtime_error("Unbekannter oder nicht lesbarer PVR-DMA-MMIO-Offset.");
    }
}

void DreamcastPvrDmaController::write(const std::uint32_t offset,
                                      const std::uint32_t value) {
    if (active_ != 0u && offset <= 0x10u) return;
    switch (offset) {
    case 0x00u:
        pvr_address_ = value & 0x1FFFFFE0u;
        return;
    case 0x04u:
        system_address_ = value & 0x1FFFFFE0u;
        return;
    case 0x08u:
        length_ = value & 0x00FFFFE0u;
        return;
    case 0x0Cu:
        direction_ = value & 1u;
        return;
    case 0x10u:
        trigger_select_ = value & 1u;
        return;
    case 0x14u:
        enabled_ = value & 1u;
        if (enabled_ == 0u) abort();
        return;
    case 0x18u:
        if ((value & 1u) != 0u && enabled_ != 0u && trigger_select_ == 0u) start();
        return;
    case 0x80u:
        if ((value >> 16u) == 0x4659u) address_protect_ = value & 0x00007F7Fu;
        return;
    default:
        throw std::runtime_error("Unbekannter oder nicht schreibbarer PVR-DMA-MMIO-Offset.");
    }
}

bool DreamcastPvrDmaController::protected_system_range(const std::uint32_t address,
                                                       const std::size_t size) const noexcept {
    return protected_range(address_protect_, address, size) && memory_.contains(address, size);
}

void DreamcastPvrDmaController::start() {
    fault_ = HollyDmaFaultReason::None;
    if (active_ != 0u || completion_event_) {
        fail(HollyDmaFaultReason::Overrun, SystemAsicEvent::PvrOverrun);
        return;
    }
    const auto bytes = static_cast<std::size_t>(length_);
    if (bytes == 0u) {
        fail(HollyDmaFaultReason::InvalidLength, SystemAsicEvent::PvrIllegalAddress);
        return;
    }
    if (direction_ != 0u) {
        fail(HollyDmaFaultReason::InvalidDirection, SystemAsicEvent::PvrIllegalAddress);
        return;
    }
    if (!protected_system_range(system_address_, bytes)) {
        fail(HollyDmaFaultReason::Overrun, SystemAsicEvent::PvrOverrun);
        return;
    }
    if (!memory_.contains(pvr_address_, bytes)) {
        fail(HollyDmaFaultReason::IllegalAddress, SystemAsicEvent::PvrIllegalAddress);
        return;
    }
    if (dmac_contract_required_) {
        const auto dmac = dmac_.lock();
        if (!dmac ||
            !dmac->validate_external_transfer(dmac_channel_, system_address_, bytes, 32u)) {
            fail(HollyDmaFaultReason::HandshakeMismatch, SystemAsicEvent::PvrOverrun);
            return;
        }
    }
    pvr_counter_ = pvr_address_;
    system_counter_ = system_address_;
    remaining_ = length_;
    active_ = 1u;
    suspend_ &= 1u;
    schedule_chunk();
}

void DreamcastPvrDmaController::schedule_chunk(std::uint64_t cycles) {
    try {
        if (cycles == 0u)
            cycles = dma_latency(
                std::min<std::uint32_t>(remaining_, transfer_chunk_bytes), timing_);
        if ((suspend_ & 1u) != 0u) {
            suspend_ |= 0x10u;
            remaining_cycles_ = cycles;
            completion_cycle_ = 0u;
            return;
        }
        if (cycles == 0u ||
            cycles > std::numeric_limits<std::uint64_t>::max() -
                         scheduler_.current_cycle()) {
            fail(HollyDmaFaultReason::SchedulerFailure, std::nullopt);
            return;
        }
        suspend_ &= ~0x10u;
        completion_cycle_ = scheduler_.current_cycle() + cycles;
        remaining_cycles_ = 0u;
        completion_event_ = scheduler_.schedule_after(
            cycles,
            [this](const auto event_id, const auto) { complete(event_id); },
            SchedulerEventKind::HollyPvrDma);
    } catch (...) {
        fail(HollyDmaFaultReason::SchedulerFailure, std::nullopt);
    }
}

void DreamcastPvrDmaController::complete(const SchedulerEventId event_id) {
    if (!completion_event_ || *completion_event_ != event_id || active_ == 0u) {
        fail(HollyDmaFaultReason::SchedulerFailure, std::nullopt);
        return;
    }
    completion_event_.reset();
    completion_cycle_ = 0u;
    remaining_cycles_ = 0u;
    const auto chunk = std::min<std::uint32_t>(remaining_, transfer_chunk_bytes);
    if (dmac_contract_required_) {
        const auto dmac = dmac_.lock();
        if (!dmac ||
            !dmac->validate_external_transfer(
                dmac_channel_, system_counter_, remaining_, 32u)) {
            fail(HollyDmaFaultReason::HandshakeMismatch, SystemAsicEvent::PvrOverrun);
            return;
        }
    }
    std::uint32_t transferred = 0u;
    while (transferred != chunk) {
        const auto unit =
            std::min<std::uint32_t>(chunk - transferred, holly_dma_transfer_unit_bytes);
        try {
            memory_.copy_bytes(
                pvr_counter_, system_counter_, unit, CodeWriteSource::Dma);
        } catch (...) {
            if (const auto dmac = dmac_.lock())
                dmac->report_external_fault(
                    dmac_channel_, DmaFaultReason::MemoryAccess, unit);
            fail(HollyDmaFaultReason::TransferFailure,
                 SystemAsicEvent::PvrIllegalAddress);
            return;
        }
        if (dmac_contract_required_) {
            const auto dmac = dmac_.lock();
            if (!dmac || !dmac->progress_external_transfer(dmac_channel_, unit)) {
                // The PVR unit is already visible. Preserve that committed prefix in the
                // controller counters even if an internal DMAC invariant unexpectedly fails.
                pvr_counter_ += unit;
                system_counter_ += unit;
                remaining_ -= unit;
                fail(HollyDmaFaultReason::HandshakeMismatch,
                     SystemAsicEvent::PvrOverrun);
                return;
            }
        }
        pvr_counter_ += unit;
        system_counter_ += unit;
        remaining_ -= unit;
        transferred += unit;
    }
    if (remaining_ != 0u) {
        schedule_chunk();
        return;
    }
    if (dmac_contract_required_) {
        const auto dmac = dmac_.lock();
        if (!dmac || !dmac->finish_external_transfer(dmac_channel_)) {
            fail(HollyDmaFaultReason::HandshakeMismatch, SystemAsicEvent::PvrOverrun);
            return;
        }
    }
    active_ = 0u;
    suspend_ |= 0x10u;
    if (completion_observer_) completion_observer_(SystemAsicEvent::PvrDma);
}

void DreamcastPvrDmaController::set_suspended(const bool suspended) {
    suspend_ = (suspend_ & ~1u) | (suspended ? 1u : 0u);
    if (suspended && completion_event_) {
        const auto now = scheduler_.current_cycle();
        remaining_cycles_ = completion_cycle_ > now ? completion_cycle_ - now : 1u;
        static_cast<void>(scheduler_.cancel(*completion_event_));
        completion_event_.reset();
        completion_cycle_ = 0u;
        suspend_ |= 0x10u;
    } else if (!suspended && active_ != 0u && (suspend_ & 0x10u) != 0u) {
        suspend_ &= ~0x10u;
        schedule_chunk(remaining_cycles_);
    }
}

void DreamcastPvrDmaController::abort() noexcept {
    cancel();
    active_ = 0u;
    enabled_ = 0u;
    suspend_ &= 1u;
}

void DreamcastPvrDmaController::hardware_trigger() {
    if (enabled_ != 0u && trigger_select_ != 0u && active_ == 0u) start();
}

void DreamcastPvrDmaController::bind_sh4_dmac(std::shared_ptr<Sh4Dmac> dmac,
                                              const std::size_t channel) {
    if (!dmac) throw std::invalid_argument("PVR-DMA-DMAC-Vertrag braucht eine Instanz.");
    if (channel >= Sh4Dmac::channel_count)
        throw std::out_of_range("PVR-DMA-DMAC-Kanal ist ungueltig.");
    dmac_ = std::move(dmac);
    dmac_channel_ = channel;
    dmac_contract_required_ = true;
}

void DreamcastPvrDmaController::fail(const HollyDmaFaultReason reason,
                                     const std::optional<SystemAsicEvent> event) noexcept {
    const auto in_flight = active_ != 0u;
    const auto residue = in_flight ? remaining_ : length_;
    if (completion_event_ && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_.cancel(*completion_event_));
    completion_event_.reset();
    completion_cycle_ = 0u;
    remaining_cycles_ = 0u;
    active_ = 0u;
    enabled_ = 0u;
    suspend_ &= 1u;
    fault_ = reason;
    ++fault_count_;
    last_fault_ = HollyDmaFault{reason,
                                event,
                                0u,
                                in_flight ? pvr_counter_ : pvr_address_,
                                in_flight ? system_counter_ : system_address_,
                                residue};
    if (event && completion_observer_) {
        try {
            completion_observer_(*event);
        } catch (...) {
        }
    }
}

void DreamcastPvrDmaController::cancel() noexcept {
    if (completion_event_) static_cast<void>(scheduler_.cancel(*completion_event_));
    completion_event_.reset();
    completion_cycle_ = 0u;
    remaining_cycles_ = 0u;
}

void DreamcastPvrDmaController::handle_scheduler_reset() noexcept {
    completion_event_.reset();
    active_ = 0u;
    remaining_ = 0u;
    suspend_ = 0u;
    completion_cycle_ = 0u;
    remaining_cycles_ = 0u;
    fault_ = HollyDmaFaultReason::None;
    fault_count_ = 0u;
    last_fault_.reset();
}

void DreamcastPvrDmaController::reset() noexcept {
    if (!scheduler_lifetime_.expired()) cancel();
    pvr_address_ = 0u;
    system_address_ = 0u;
    length_ = 0u;
    direction_ = 0u;
    trigger_select_ = 0u;
    enabled_ = 0u;
    active_ = 0u;
    suspend_ = 0u;
    address_protect_ = 0x00007F00u;
    pvr_counter_ = 0u;
    system_counter_ = 0u;
    remaining_ = 0u;
    completion_cycle_ = 0u;
    remaining_cycles_ = 0u;
    fault_ = HollyDmaFaultReason::None;
    fault_count_ = 0u;
    last_fault_.reset();
}

HollyDmaChannelState DreamcastPvrDmaController::state() const noexcept {
    HollyDmaChannelState result;
    result.peripheral_address = pvr_address_;
    result.system_address = system_address_;
    result.length = length_;
    result.direction = direction_;
    result.trigger_select = trigger_select_;
    result.enabled = enabled_;
    result.active = active_;
    result.suspend = suspend_;
    result.peripheral_counter = pvr_counter_;
    result.system_counter = system_counter_;
    result.remaining = remaining_;
    result.completion_cycle = completion_cycle_;
    result.remaining_cycles =
        completion_event_ && completion_cycle_ > scheduler_.current_cycle()
            ? completion_cycle_ - scheduler_.current_cycle()
            : remaining_cycles_;
    result.completion_event = completion_event_;
    result.completion_event_rehydration_pending =
        event_rehydration_pending();
    result.fault = fault_;
    result.fault_count = fault_count_;
    return result;
}

const std::optional<HollyDmaFault>& DreamcastPvrDmaController::last_fault() const noexcept {
    return last_fault_;
}

DreamcastPvrDmaSnapshot DreamcastPvrDmaController::snapshot() const noexcept {
    return {
        state(),
        timing_,
        address_protect_,
        last_fault_,
        reset_observer_,
        dmac_channel_,
        !dmac_.expired(),
        dmac_contract_required_,
        static_cast<bool>(completion_observer_),
    };
}

void DreamcastPvrDmaController::validate_state_restore(
    const DreamcastPvrDmaSnapshot& state) const {
    if (state.timing != timing_ ||
        state.completion_observer_bound !=
            static_cast<bool>(completion_observer_) ||
        state.dmac_contract_required != dmac_contract_required_ ||
        state.dmac_bound != !dmac_.expired() ||
        state.dmac_channel != dmac_channel_)
        throw std::invalid_argument(
            "PVR-DMA-Handoff passt nicht zum Runtime-Vertrag.");
    const auto& channel = state.channel;
    if (channel.direction > 1u || channel.trigger_select > 1u ||
        channel.enabled > 1u || channel.active > 1u ||
        (channel.suspend & ~0x11u) != 0u ||
        static_cast<std::uint8_t>(channel.fault) >
            static_cast<std::uint8_t>(
                HollyDmaFaultReason::HandshakeMismatch) ||
        (channel.completion_event &&
         channel.completion_event_rehydration_pending))
        throw std::invalid_argument(
            "PVR-DMA-Handoff besitzt ungueltige Kanaldaten.");
    const auto scheduled =
        channel.completion_event.has_value() ||
        channel.completion_event_rehydration_pending;
    if (scheduled) {
        if (channel.active == 0u || channel.completion_cycle == 0u ||
            channel.remaining_cycles > channel.completion_cycle)
            throw std::invalid_argument(
                "PVR-DMA-Handoff besitzt ein ungueltiges Completionevent.");
    } else if (channel.completion_cycle != 0u) {
        throw std::invalid_argument(
            "PVR-DMA-Handoff besitzt einen Zyklus ohne Completionevent.");
    }
    if (channel.active != 0u &&
        (channel.remaining == 0u ||
         !protected_range(state.address_protect,
                          channel.system_counter,
                          channel.remaining) ||
         !memory_.contains(channel.system_counter, channel.remaining) ||
         !memory_.contains(channel.peripheral_counter, channel.remaining)))
        throw std::invalid_argument(
            "PVR-DMA-Handoff passt nicht zum Runtime-Speichervertrag.");
}

void DreamcastPvrDmaController::restore_state_passive(
    const DreamcastPvrDmaSnapshot& state) {
    validate_state_restore(state);
    cancel();
    const auto& channel = state.channel;
    pvr_address_ = channel.peripheral_address;
    system_address_ = channel.system_address;
    length_ = channel.length;
    direction_ = channel.direction;
    trigger_select_ = channel.trigger_select;
    enabled_ = channel.enabled;
    active_ = channel.active;
    suspend_ = channel.suspend;
    address_protect_ = state.address_protect;
    pvr_counter_ = channel.peripheral_counter;
    system_counter_ = channel.system_counter;
    remaining_ = channel.remaining;
    completion_cycle_ = channel.completion_cycle;
    remaining_cycles_ = channel.remaining_cycles;
    fault_ = channel.fault;
    fault_count_ = channel.fault_count;
    last_fault_ = state.last_fault;
}

SchedulerEventId DreamcastPvrDmaController::rehydrate_scheduled_event(
    const std::uint64_t guest_cycle,
    const std::uint32_t channel,
    const std::uint64_t token) {
    if (channel != 0u ||
        token != dreamcast_holly_dma_event_token_v1)
        throw std::invalid_argument(
            "PVR-DMA-Handoff besitzt einen unbekannten Eventkanal oder Token.");
    if (completion_event_ || active_ == 0u || completion_cycle_ == 0u)
        throw std::logic_error(
            "PVR-DMA-Handoff erwartet kein Completionevent.");
    if (guest_cycle != completion_cycle_ ||
        guest_cycle < scheduler_.current_cycle())
        throw std::invalid_argument(
            "PVR-DMA-Completion passt nicht zur gespeicherten Gastzeit.");
    const auto event_id = scheduler_.schedule_at(
        guest_cycle,
        [this](const auto restored_event_id, const auto) {
            complete(restored_event_id);
        },
        SchedulerEventKind::HollyPvrDma);
    completion_event_ = event_id;
    return event_id;
}

bool DreamcastPvrDmaController::event_rehydration_pending() const noexcept {
    return active_ != 0u && completion_cycle_ != 0u &&
           !completion_event_;
}

namespace {

class HollyStateWriter final {
  public:
    void u8(const std::uint8_t value) { bytes_.push_back(value); }
    void boolean(const bool value) { u8(value ? 1u : 0u); }
    void u32(const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            u8(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void u64(const std::uint64_t value) {
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            u8(static_cast<std::uint8_t>(value >> (byte * 8u)));
    }
    void magic(const std::string_view value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    [[nodiscard]] std::vector<std::uint8_t> finish() && {
        return std::move(bytes_);
    }
  private:
    std::vector<std::uint8_t> bytes_;
};

class HollyStateReader final {
  public:
    explicit HollyStateReader(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {}
    [[nodiscard]] std::uint8_t u8() {
        require(1u);
        return bytes_[cursor_++];
    }
    [[nodiscard]] bool boolean() {
        const auto value = u8();
        if (value > 1u)
            throw std::invalid_argument(
                "Holly-DMA-State besitzt ein ungueltiges Boolean.");
        return value != 0u;
    }
    [[nodiscard]] std::uint32_t u32() {
        std::uint32_t value = 0u;
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            value |= static_cast<std::uint32_t>(u8()) << (byte * 8u);
        return value;
    }
    [[nodiscard]] std::uint64_t u64() {
        std::uint64_t value = 0u;
        for (std::size_t byte = 0u; byte < 8u; ++byte)
            value |= static_cast<std::uint64_t>(u8()) << (byte * 8u);
        return value;
    }
    void magic(const std::string_view expected) {
        require(expected.size());
        for (const auto expected_byte : expected)
            if (u8() != static_cast<std::uint8_t>(expected_byte))
                throw std::invalid_argument(
                    "Holly-DMA-State besitzt ein ungueltiges Magic.");
    }
    void finish() const {
        if (cursor_ != bytes_.size())
            throw std::invalid_argument(
                "Holly-DMA-State besitzt nachlaufende Daten.");
    }
  private:
    void require(const std::size_t size) const {
        if (size > bytes_.size() - cursor_)
            throw std::invalid_argument("Holly-DMA-State ist abgeschnitten.");
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_ = 0u;
};

void write_channel(HollyStateWriter& writer,
                   const HollyDmaChannelState& channel) {
    writer.u32(channel.peripheral_address);
    writer.u32(channel.system_address);
    writer.u32(channel.length);
    writer.u32(channel.direction);
    writer.u32(channel.trigger_select);
    writer.u32(channel.enabled);
    writer.u32(channel.active);
    writer.u32(channel.suspend);
    writer.u32(channel.peripheral_counter);
    writer.u32(channel.system_counter);
    writer.u32(channel.remaining);
    writer.u64(channel.completion_cycle);
    writer.u64(channel.remaining_cycles);
    writer.boolean(channel.completion_event.has_value() ||
                   channel.completion_event_rehydration_pending);
    writer.u8(static_cast<std::uint8_t>(channel.fault));
    writer.u64(channel.fault_count);
}

HollyDmaChannelState read_channel(HollyStateReader& reader) {
    HollyDmaChannelState channel;
    channel.peripheral_address = reader.u32();
    channel.system_address = reader.u32();
    channel.length = reader.u32();
    channel.direction = reader.u32();
    channel.trigger_select = reader.u32();
    channel.enabled = reader.u32();
    channel.active = reader.u32();
    channel.suspend = reader.u32();
    channel.peripheral_counter = reader.u32();
    channel.system_counter = reader.u32();
    channel.remaining = reader.u32();
    channel.completion_cycle = reader.u64();
    channel.remaining_cycles = reader.u64();
    channel.completion_event.reset();
    channel.completion_event_rehydration_pending = reader.boolean();
    channel.fault = static_cast<HollyDmaFaultReason>(reader.u8());
    channel.fault_count = reader.u64();
    return channel;
}

void write_fault(HollyStateWriter& writer,
                 const std::optional<HollyDmaFault>& fault) {
    writer.boolean(fault.has_value());
    if (!fault) return;
    writer.u8(static_cast<std::uint8_t>(fault->reason));
    writer.boolean(fault->event.has_value());
    if (fault->event)
        writer.u32(static_cast<std::uint16_t>(*fault->event));
    writer.u64(fault->channel);
    writer.u32(fault->peripheral_address);
    writer.u32(fault->system_address);
    writer.u32(fault->remaining);
}

std::optional<HollyDmaFault> read_fault(HollyStateReader& reader) {
    if (!reader.boolean()) return std::nullopt;
    HollyDmaFault fault;
    fault.reason = static_cast<HollyDmaFaultReason>(reader.u8());
    if (reader.boolean())
        fault.event =
            static_cast<SystemAsicEvent>(
                static_cast<std::uint16_t>(reader.u32()));
    fault.channel = static_cast<std::size_t>(reader.u64());
    fault.peripheral_address = reader.u32();
    fault.system_address = reader.u32();
    fault.remaining = reader.u32();
    return fault;
}

void write_g1_fault(HollyStateWriter& writer,
                    const std::optional<G1DmaFault>& fault) {
    writer.boolean(fault.has_value());
    if (!fault) return;
    writer.u8(static_cast<std::uint8_t>(fault->reason));
    writer.u32(fault->fault_address);
    writer.u32(fault->transferred_bytes);
    writer.u32(fault->residue);
    writer.u8(static_cast<std::uint8_t>(fault->phase));
}

std::optional<G1DmaFault> read_g1_fault(HollyStateReader& reader) {
    if (!reader.boolean()) return std::nullopt;
    G1DmaFault fault;
    fault.reason = static_cast<HollyDmaFaultReason>(reader.u8());
    fault.fault_address = reader.u32();
    fault.transferred_bytes = reader.u32();
    fault.residue = reader.u32();
    fault.phase = static_cast<G1DmaFaultPhase>(reader.u8());
    return fault;
}

void write_common_header(HollyStateWriter& writer,
                         const std::string_view magic) {
    writer.magic(magic);
    writer.u32(dreamcast_holly_dma_state_contract_version);
}

void read_common_header(HollyStateReader& reader,
                        const std::string_view magic) {
    reader.magic(magic);
    if (reader.u32() != dreamcast_holly_dma_state_contract_version)
        throw std::invalid_argument(
            "Holly-DMA-State besitzt eine unbekannte Version.");
}

} // namespace

std::vector<std::uint8_t>
encode_dreamcast_g2_dma_state(const DreamcastG2DmaSnapshot& state) {
    HollyStateWriter writer;
    write_common_header(writer, "KATG2D1");
    for (const auto& channel : state.channels) write_channel(writer, channel);
    writer.u64(state.timing.cycles_per_byte);
    writer.u32(state.address_protect);
    writer.u32(state.ds_timeout);
    writer.u32(state.tr_timeout);
    writer.u32(state.modem_timeout);
    writer.u32(state.modem_wait);
    writer.u64(state.completed_dma_count);
    write_fault(writer, state.last_fault);
    writer.boolean(state.completion_observer_bound);
    return std::move(writer).finish();
}

DreamcastG2DmaSnapshot
decode_dreamcast_g2_dma_state(const std::span<const std::uint8_t> bytes) {
    HollyStateReader reader(bytes);
    read_common_header(reader, "KATG2D1");
    DreamcastG2DmaSnapshot state;
    for (auto& channel : state.channels) channel = read_channel(reader);
    state.timing.cycles_per_byte = reader.u64();
    state.address_protect = reader.u32();
    state.ds_timeout = reader.u32();
    state.tr_timeout = reader.u32();
    state.modem_timeout = reader.u32();
    state.modem_wait = reader.u32();
    state.completed_dma_count = reader.u64();
    state.last_fault = read_fault(reader);
    state.reset_observer = 0u;
    state.completion_observer_bound = reader.boolean();
    reader.finish();
    return state;
}

std::vector<std::uint8_t>
encode_dreamcast_g1_dma_state(const DreamcastG1DmaSnapshot& state) {
    HollyStateWriter writer;
    write_common_header(writer, "KATG1D1");
    write_channel(writer, state.channel);
    writer.u64(state.timing.cycles_per_byte);
    writer.u32(state.bios_handoff_live_address);
    writer.u32(state.system_mode);
    writer.u32(state.gdrom_read_access_timing);
    writer.u32(state.address_protect);
    write_fault(writer, state.last_fault);
    write_g1_fault(writer, state.last_g1_fault);
    writer.boolean(state.transfer_handler_bound);
    writer.boolean(state.completion_observer_bound);
    writer.boolean(state.range_validator_bound);
    writer.boolean(state.fault_observer_bound);
    return std::move(writer).finish();
}

DreamcastG1DmaSnapshot
decode_dreamcast_g1_dma_state(const std::span<const std::uint8_t> bytes) {
    HollyStateReader reader(bytes);
    read_common_header(reader, "KATG1D1");
    DreamcastG1DmaSnapshot state;
    state.channel = read_channel(reader);
    state.timing.cycles_per_byte = reader.u64();
    state.bios_handoff_live_address = reader.u32();
    state.system_mode = reader.u32();
    state.gdrom_read_access_timing = reader.u32();
    state.address_protect = reader.u32();
    state.last_fault = read_fault(reader);
    state.last_g1_fault = read_g1_fault(reader);
    state.reset_observer = 0u;
    state.transfer_handler_bound = reader.boolean();
    state.completion_observer_bound = reader.boolean();
    state.range_validator_bound = reader.boolean();
    state.fault_observer_bound = reader.boolean();
    reader.finish();
    return state;
}

std::vector<std::uint8_t>
encode_dreamcast_pvr_dma_state(const DreamcastPvrDmaSnapshot& state) {
    HollyStateWriter writer;
    write_common_header(writer, "KATPVD1");
    write_channel(writer, state.channel);
    writer.u64(state.timing.cycles_per_byte);
    writer.u32(state.address_protect);
    write_fault(writer, state.last_fault);
    writer.u64(state.dmac_channel);
    writer.boolean(state.dmac_bound);
    writer.boolean(state.dmac_contract_required);
    writer.boolean(state.completion_observer_bound);
    return std::move(writer).finish();
}

DreamcastPvrDmaSnapshot
decode_dreamcast_pvr_dma_state(const std::span<const std::uint8_t> bytes) {
    HollyStateReader reader(bytes);
    read_common_header(reader, "KATPVD1");
    DreamcastPvrDmaSnapshot state;
    state.channel = read_channel(reader);
    state.timing.cycles_per_byte = reader.u64();
    state.address_protect = reader.u32();
    state.last_fault = read_fault(reader);
    state.reset_observer = 0u;
    state.dmac_channel = static_cast<std::size_t>(reader.u64());
    state.dmac_bound = reader.boolean();
    state.dmac_contract_required = reader.boolean();
    state.completion_observer_bound = reader.boolean();
    reader.finish();
    return state;
}

void validate_dreamcast_pvr_dma_dmac_restore_contract(
    const DreamcastPvrDmaSnapshot& pvr,
    const Sh4DmacSnapshot& dmac) {
    if (!pvr.dmac_contract_required || pvr.channel.active == 0u)
        return;
    if (!pvr.dmac_bound ||
        pvr.dmac_channel >= dmac.channels.size() ||
        pvr.channel.remaining == 0u ||
        pvr.channel.remaining % holly_dma_transfer_unit_bytes != 0u)
        throw std::invalid_argument(
            "PVR-DMA-Handoff besitzt keinen gebundenen SH4-DMAC-Vertrag.");
    const auto& channel = dmac.channels[pvr.dmac_channel];
    const auto control = channel.control;
    const auto contract_matches =
        (dmac.operation & Sh4Dmac::master_enable) != 0u &&
        (dmac.operation &
         (Sh4Dmac::address_error_flag | Sh4Dmac::nmi_flag)) == 0u &&
        (control & Sh4Dmac::channel_enable) != 0u &&
        (control & Sh4Dmac::transfer_end) == 0u &&
        ((control >> 8u) & 0xFu) == 8u &&
        ((control >> 12u) & 0x3u) == 1u &&
        ((control >> 14u) & 0x3u) == 0u &&
        ((control >> 4u) & 0x7u) == 4u &&
        (channel.source & 0x1FFFFFFFu) ==
            (pvr.channel.system_counter & 0x1FFFFFFFu) &&
        channel.count ==
            pvr.channel.remaining / holly_dma_transfer_unit_bytes;
    if (!contract_matches)
        throw std::invalid_argument(
            "PVR-DMA-Handoff passt nicht zum SH4-DMAC-Snapshot.");
}

DreamcastHollyDmaControllers
map_dreamcast_holly_dma(Memory& memory,
                        EventScheduler& scheduler,
                        const HollyDmaTiming timing,
                        std::function<void(SystemAsicEvent)> completion_observer,
                        DreamcastG1BusController::TransferHandler g1_transfer_handler) {
    DreamcastHollyDmaControllers result;
    result.g1 = std::make_shared<DreamcastG1BusController>(
        scheduler,
        timing,
        std::move(g1_transfer_handler),
        completion_observer,
        [&memory](const std::uint32_t address, const std::size_t size) {
            const auto physical = address < 0xE0000000u ? address & 0x1FFFFFFFu : address;
            return memory.is_writable_linear_range(physical, size);
        });
    result.g2 =
        std::make_shared<DreamcastG2DmaController>(memory, scheduler, timing, completion_observer);
    result.pvr =
        std::make_shared<DreamcastPvrDmaController>(memory, scheduler, timing, completion_observer);
    const auto g1_device = make_word_device(
        holly_dma_register_size,
        [controller = result.g1](const auto offset) { return controller->read(offset); },
        [controller = result.g1](const auto offset, const auto value) {
            controller->write(offset, value);
        },
        "G1-Steuerblock");
    const auto g2_device = make_word_device(
        holly_dma_register_size,
        [controller = result.g2](const auto offset) { return controller->read(offset); },
        [controller = result.g2](const auto offset, const auto value) {
            controller->write(offset, value);
        },
        "G2-DMA-Steuerblock");
    const auto pvr_device = make_word_device(
        holly_dma_register_size,
        [controller = result.pvr](const auto offset) { return controller->read(offset); },
        [controller = result.pvr](const auto offset, const auto value) {
            controller->write(offset, value);
        },
        "PVR-DMA-Steuerblock");
    map_direct(memory, "dreamcast-g1-mmio", g1_mmio_physical_base, g1_device);
    map_direct(memory, "dreamcast-g2-mmio", g2_mmio_physical_base, g2_device);
    map_direct(memory, "dreamcast-pvr-dma-mmio", pvr_dma_mmio_physical_base, pvr_device);
    return result;
}

} // namespace katana::runtime

#include "katana/runtime/holly_dma.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace katana::runtime {
namespace {
constexpr std::array g2_completion_events{SystemAsicEvent::AicaDma,
                                          SystemAsicEvent::Ext1Dma,
                                          SystemAsicEvent::Ext2Dma,
                                          SystemAsicEvent::DeviceDma};

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
    const auto bottom = ((protection & 0x7Fu) << 20u) | 0x08000000u;
    const auto top = (((protection >> 8u) & 0x7Fu) << 20u) | 0x080FFFFFu;
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
                state.active = 0u;
                state.remaining = 0u;
                state.completion_cycle = 0u;
                state.remaining_cycles = 0u;
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
    if (state.active != 0u || state.completion_event)
        throw std::logic_error("G2-DMA-Kanal wurde waehrend eines Transfers erneut gestartet.");
    const auto bytes = static_cast<std::size_t>(state.length & 0x7FFFFFFFu);
    if (!protected_system_range(state.system_address, bytes))
        throw std::out_of_range("G2-DMA-Systemspeicher liegt ausserhalb des Schutzfensters.");
    if (bytes == 0u) throw std::invalid_argument("G2-DMA braucht eine positive Laenge.");
    auto source = state.system_address;
    auto destination = state.peripheral_address;
    if (state.direction != 0u) std::swap(source, destination);
    if (!memory_.contains(source, bytes) || !memory_.contains(destination, bytes))
        throw std::out_of_range("G2-DMA-Quelle oder -Ziel ist nicht vollstaendig abgebildet.");
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
    case 2u:
        return;
    default:
        state.active = 0u;
        state.remaining = 0u;
        throw std::invalid_argument("G2-DMA-Triggerauswahl 3 ist reserviert.");
    }
}

void DreamcastG2DmaController::start(const std::size_t channel) {
    auto& state = channels_.at(channel);
    if (state.active == 0u || state.completion_event)
        throw std::logic_error("G2-DMA-Trigger besitzt keinen bereitstehenden Kanal.");
    const auto cycles = state.remaining_cycles != 0u
                            ? state.remaining_cycles
                            : dma_latency(static_cast<std::size_t>(state.remaining), timing_);
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
                                      scheduler_.current_cycle())
        throw std::overflow_error("G2-DMA-Completionfrist ist ungueltig.");
    state.remaining_cycles = 0u;
    state.completion_cycle = scheduler_.current_cycle() + cycles;
    state.completion_event = scheduler_.schedule_after(
        cycles, [this, channel](const auto event_id, const auto) { complete(channel, event_id); });
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
    if (!state.completion_event || *state.completion_event != event_id || state.active == 0u)
        throw std::logic_error("G2-DMA-Completion besitzt keinen aktiven Kanal.");
    const auto bytes = state.length & 0x7FFFFFFFu;
    auto source = state.system_address;
    auto destination = state.peripheral_address;
    if (state.direction != 0u) std::swap(source, destination);
    transfer(memory_, source, destination, bytes);
    state.peripheral_address += bytes;
    state.system_address += bytes;
    state.peripheral_counter = state.peripheral_address;
    state.system_counter = state.system_address;
    state.remaining = 0u;
    state.enabled = (state.length & 0x80000000u) != 0u ? 0u : 1u;
    state.length = 0u;
    state.active = 0u;
    state.suspend |= 0x10u;
    state.completion_event.reset();
    state.completion_cycle = 0u;
    state.remaining_cycles = 0u;
    ++completed_dma_count_;
    if (completion_observer_) completion_observer_(g2_completion_events[channel]);
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
    }
}

void DreamcastG2DmaController::handle_scheduler_reset() noexcept {
    for (auto& channel : channels_) {
        channel.completion_event.reset();
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
}

std::uint64_t DreamcastG2DmaController::completed_dma_count() const noexcept {
    return completed_dma_count_;
}

DreamcastG1BusController::DreamcastG1BusController(
    EventScheduler& scheduler,
    const HollyDmaTiming timing,
    TransferHandler transfer_handler,
    std::function<void(SystemAsicEvent)> completion_observer)
    : scheduler_(scheduler), timing_(timing), transfer_handler_(std::move(transfer_handler)),
      completion_observer_(std::move(completion_observer)),
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
        return dma_address_;
    case 0x08u:
        return dma_length_;
    case 0x0Cu:
        return dma_direction_;
    case 0x14u:
        return dma_enabled_;
    case 0x18u:
        return dma_active_;
    case 0xB0u:
        return system_mode_;
    case 0xF4u:
        return dma_address_;
    case 0xF8u:
        return dma_length_;
    default:
        throw std::runtime_error("Unbekannter oder nicht lesbarer G1-MMIO-Offset.");
    }
}

void DreamcastG1BusController::write(const std::uint32_t offset, const std::uint32_t value) {
    switch (offset) {
    case 0x04u:
        dma_address_ = value & 0x1FFFFFE0u;
        return;
    case 0x08u:
        dma_length_ = value & 0x01FFFFFFu;
        return;
    case 0x0Cu:
        dma_direction_ = value & 1u;
        return;
    case 0x14u:
        dma_enabled_ = value & 1u;
        return;
    case 0x18u:
        if ((value & 1u) != 0u && dma_enabled_ != 0u) start();
        return;
    case 0x80u:
    case 0x84u:
    case 0x88u:
    case 0x8Cu:
    case 0x90u:
    case 0x94u:
    case 0xA0u:
    case 0xA4u:
    case 0xB4u:
    case 0xB8u:
    case 0xE4u:
        return;
    default:
        throw std::runtime_error("Unbekannter oder nicht schreibbarer G1-MMIO-Offset.");
    }
}

void DreamcastG1BusController::start() {
    if (dma_active_ != 0u || completion_event_)
        throw std::logic_error("G1-DMA wurde waehrend eines Transfers erneut gestartet.");
    if (!transfer_handler_)
        throw std::runtime_error("G1-DMA besitzt keinen GD-ROM-Transferpfad.");
    if (dma_length_ == 0u) throw std::invalid_argument("G1-DMA braucht eine Laenge.");
    transfer_handler_(dma_address_, dma_length_, dma_direction_);
    dma_active_ = 1u;
    completion_event_ = scheduler_.schedule_after(
        dma_latency(dma_length_, timing_),
        [this](const auto event_id, const auto) { complete(event_id); });
}

void DreamcastG1BusController::complete(const SchedulerEventId event_id) {
    if (!completion_event_ || *completion_event_ != event_id || dma_active_ == 0u)
        throw std::logic_error("G1-DMA-Completion besitzt keinen aktiven Transfer.");
    dma_address_ += dma_length_;
    dma_length_ = 0u;
    dma_active_ = 0u;
    completion_event_.reset();
    if (completion_observer_) completion_observer_(SystemAsicEvent::GdromDma);
}

void DreamcastG1BusController::handle_scheduler_reset() noexcept {
    completion_event_.reset();
    dma_active_ = 0u;
}

void DreamcastG1BusController::reset() noexcept {
    if (completion_event_ && !scheduler_lifetime_.expired())
        static_cast<void>(scheduler_.cancel(*completion_event_));
    completion_event_.reset();
    dma_address_ = 0u;
    dma_length_ = 0u;
    dma_direction_ = 0u;
    dma_enabled_ = 0u;
    dma_active_ = 0u;
    system_mode_ = 1u;
}

DreamcastHollyDmaControllers
map_dreamcast_holly_dma(Memory& memory,
                        EventScheduler& scheduler,
                        const HollyDmaTiming timing,
                        std::function<void(SystemAsicEvent)> completion_observer,
                        DreamcastG1BusController::TransferHandler g1_transfer_handler) {
    DreamcastHollyDmaControllers result;
    result.g1 = std::make_shared<DreamcastG1BusController>(
        scheduler, timing, std::move(g1_transfer_handler), completion_observer);
    result.g2 =
        std::make_shared<DreamcastG2DmaController>(memory, scheduler, timing, completion_observer);
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
    map_direct(memory, "dreamcast-g1-mmio", g1_mmio_physical_base, g1_device);
    map_direct(memory, "dreamcast-g2-mmio", g2_mmio_physical_base, g2_device);
    return result;
}

} // namespace katana::runtime

#include "katana/runtime/system_asic.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace katana::runtime {
namespace {
using system_bus_register::BavlWaitCount;
using system_bus_register::BootReservedA4;
using system_bus_register::BootReservedAc;
using system_bus_register::Channel2Destination;
using system_bus_register::Channel2Length;
using system_bus_register::Channel2MaxBurst;
using system_bus_register::Channel2Priority;
using system_bus_register::Channel2Start;
using system_bus_register::DbreqMask;
using system_bus_register::FifoStatus;
using system_bus_register::Revision;
using system_bus_register::RootBusSplit;
using system_bus_register::SortAddressShift;
using system_bus_register::SortBaseAddress;
using system_bus_register::SortDivider;
using system_bus_register::SortLinkWidth;
using system_bus_register::SortStart;
using system_bus_register::SortStartAddress;
using system_bus_register::SystemReset;
using system_bus_register::TaFifoRemaining;
using system_bus_register::TextureMemoryMode0;
using system_bus_register::TextureMemoryMode1;

std::pair<std::size_t, std::uint32_t> event_bit(const SystemAsicEvent event) {
    const auto code = static_cast<std::uint16_t>(event);
    const auto bank = static_cast<std::size_t>((code >> 8u) & 0xFFu);
    const auto bit = static_cast<std::uint32_t>(code & 0xFFu);
    if (bank >= 3u || bit >= 32u) throw std::invalid_argument("Ungueltiges System-ASIC-Ereignis.");
    return {bank, std::uint32_t{1u} << bit};
}

std::array<bool, 3u> calculate_expected_external_lines(
    const std::array<std::uint32_t, 3u>& pending,
    const std::array<std::array<std::uint32_t, 3u>, 3u>& masks) noexcept {
    std::array<bool, 3u> lines{};
    for (std::size_t line = 0u; line < lines.size(); ++line)
        for (std::size_t bank = 0u; bank < pending.size(); ++bank)
            lines[line] =
                lines[line] ||
                (pending[bank] & masks[line][bank]) != 0u;
    return lines;
}

bool is_system_bus_readable(const std::uint32_t offset) {
    constexpr std::array offsets{Channel2Destination,
                                 Channel2Length,
                                 Channel2Start,
                                 SortStartAddress,
                                 SortBaseAddress,
                                 SortLinkWidth,
                                 SortAddressShift,
                                 SortStart,
                                 DbreqMask,
                                 BavlWaitCount,
                                 Channel2Priority,
                                 Channel2MaxBurst,
                                 SortDivider,
                                 TaFifoRemaining,
                                 TextureMemoryMode0,
                                 TextureMemoryMode1,
                                 FifoStatus,
                                 Revision,
                                 RootBusSplit};
    for (const auto candidate : offsets)
        if (candidate == offset) return true;
    return false;
}

std::uint32_t system_bus_write_mask(const std::uint32_t offset) {
    switch (offset) {
    case Channel2Destination:
        return 0x03FFFFE0u;
    case Channel2Length:
        return 0x00FFFFE0u;
    case Channel2Start:
    case SortLinkWidth:
    case SortAddressShift:
    case SortStart:
    case DbreqMask:
    case TextureMemoryMode0:
    case TextureMemoryMode1:
        return 0x00000001u;
    case SortStartAddress:
    case SortBaseAddress:
        return 0x07FFFFE0u;
    case BavlWaitCount:
        return 0x0000001Fu;
    case Channel2Priority:
        return 0x0000000Fu;
    case Channel2MaxBurst:
        return 0x00000003u;
    case RootBusSplit:
        return 0x80000000u;
    default:
        throw std::runtime_error("Unbekannter oder nicht schreibbarer Systembus-MMIO-Offset.");
    }
}
} // namespace

std::array<bool, 3u>
dreamcast_system_asic_expected_external_lines(
    const DreamcastSystemAsicSnapshot& state) noexcept {
    return calculate_expected_external_lines(state.pending, state.masks);
}

DreamcastSystemBusControl::DreamcastSystemBusControl(
    Channel2StartObserver channel2_start_observer)
    : channel2_start_observer_(std::move(channel2_start_observer)) {}

std::size_t DreamcastSystemBusControl::index(const std::uint32_t offset) {
    if (offset >= system_bus_control_register_size || (offset & 3u) != 0u)
        throw std::out_of_range("Ungueltiger oder nicht ausgerichteter Systembus-Registeroffset.");
    return offset / 4u;
}

std::uint32_t DreamcastSystemBusControl::read(const std::uint32_t offset) const {
    static_cast<void>(index(offset));
    if (!is_system_bus_readable(offset))
        throw std::runtime_error("Unbekannter oder nicht lesbarer Systembus-MMIO-Offset.");
    return registers_[index(offset)];
}

void DreamcastSystemBusControl::write(const std::uint32_t offset, const std::uint32_t value) {
    static_cast<void>(index(offset));
    if (offset == BootReservedA4 || offset == BootReservedAc) {
        if (value != 0u)
            throw std::runtime_error("Reserviertes Systembus-Bootregister akzeptiert nur Null.");
        return;
    }
    if (offset == SystemReset) {
        if ((value & 0xFFFFu) == 0x7611u) {
            ++system_reset_requests_;
            reset();
            if (system_reset_observer_) system_reset_observer_();
        }
        return;
    }
    if (offset == Channel2Start && (value & 1u) != 0u) {
        if (registers_[index(Channel2Start)] != 0u)
            throw std::logic_error("Systembus-Channel-2-DMA ist bereits aktiv.");
        if (!channel2_start_observer_)
            throw std::runtime_error("Systembus-Channel-2-DMA besitzt keinen Transferpfad.");
        const auto length = registers_[index(Channel2Length)];
        if (length == 0u)
            throw std::invalid_argument("Systembus-Channel-2-DMA braucht eine Laenge.");
        static_cast<void>(trigger_channel2());
        return;
    }
    if (offset == SortStart && (value & 1u) != 0u) {
        throw std::runtime_error("Systembus-Sort-DMA besitzt noch keinen Transferpfad.");
    }
    const auto mask = system_bus_write_mask(offset);
    auto normalized = value & mask;
    if (offset == Channel2Destination) normalized |= 0x10000000u;
    if (offset == SortStartAddress || offset == SortBaseAddress) normalized |= 0x08000000u;
    registers_[index(offset)] = normalized;
}

void DreamcastSystemBusControl::reset() noexcept {
    registers_.fill(0u);
    registers_[index(Channel2Destination)] = 0x10000000u;
    registers_[index(SortStartAddress)] = 0x08000000u;
    registers_[index(SortBaseAddress)] = 0x08000000u;
    registers_[index(TaFifoRemaining)] = 8u;
    registers_[index(Revision)] = 0xBu;
}

std::uint64_t DreamcastSystemBusControl::system_reset_requests() const noexcept {
    return system_reset_requests_;
}

bool DreamcastSystemBusControl::aica_write_buffer_empty() const noexcept {
    return (registers_[FifoStatus / 4u] & 1u) == 0u;
}

DreamcastSystemBusSnapshot DreamcastSystemBusControl::snapshot() const noexcept {
    DreamcastSystemBusSnapshot result;
    result.registers = registers_;
    result.channel2_destination = registers_[Channel2Destination / 4u];
    result.channel2_length = registers_[Channel2Length / 4u];
    result.channel2_start = registers_[Channel2Start / 4u];
    result.sort_start_address = registers_[SortStartAddress / 4u];
    result.sort_base_address = registers_[SortBaseAddress / 4u];
    result.sort_link_width = registers_[SortLinkWidth / 4u];
    result.sort_address_shift = registers_[SortAddressShift / 4u];
    result.sort_start = registers_[SortStart / 4u];
    result.dbreq_mask = registers_[DbreqMask / 4u];
    result.bavl_wait_count = registers_[BavlWaitCount / 4u];
    result.channel2_priority = registers_[Channel2Priority / 4u];
    result.channel2_max_burst = registers_[Channel2MaxBurst / 4u];
    result.sort_divider = registers_[SortDivider / 4u];
    result.ta_fifo_remaining = registers_[TaFifoRemaining / 4u];
    result.texture_memory_mode0 = registers_[TextureMemoryMode0 / 4u];
    result.texture_memory_mode1 = registers_[TextureMemoryMode1 / 4u];
    result.fifo_status = registers_[FifoStatus / 4u];
    result.revision = registers_[Revision / 4u];
    result.root_bus_split = registers_[RootBusSplit / 4u];
    result.system_reset_requests = system_reset_requests_;
    result.channel2_start_observer_bound =
        static_cast<bool>(channel2_start_observer_);
    result.system_reset_observer_bound =
        static_cast<bool>(system_reset_observer_);
    return result;
}

void DreamcastSystemBusControl::validate_state_restore(
    const DreamcastSystemBusSnapshot& state) const {
    if (state.channel2_start_observer_bound !=
            static_cast<bool>(channel2_start_observer_) ||
        state.system_reset_observer_bound !=
            static_cast<bool>(system_reset_observer_))
        throw std::invalid_argument(
            "Systembus-Handoff passt nicht zum Runtime-Wiring.");
    const auto require_register =
        [&state](const std::uint32_t offset, const std::uint32_t value) {
            if (state.registers[offset / 4u] != value)
                throw std::invalid_argument(
                    "Systembus-Handoff besitzt widerspruechliche Registerdaten.");
        };
    require_register(Channel2Destination, state.channel2_destination);
    require_register(Channel2Length, state.channel2_length);
    require_register(Channel2Start, state.channel2_start);
    require_register(SortStartAddress, state.sort_start_address);
    require_register(SortBaseAddress, state.sort_base_address);
    require_register(SortLinkWidth, state.sort_link_width);
    require_register(SortAddressShift, state.sort_address_shift);
    require_register(SortStart, state.sort_start);
    require_register(DbreqMask, state.dbreq_mask);
    require_register(BavlWaitCount, state.bavl_wait_count);
    require_register(Channel2Priority, state.channel2_priority);
    require_register(Channel2MaxBurst, state.channel2_max_burst);
    require_register(SortDivider, state.sort_divider);
    require_register(TaFifoRemaining, state.ta_fifo_remaining);
    require_register(TextureMemoryMode0, state.texture_memory_mode0);
    require_register(TextureMemoryMode1, state.texture_memory_mode1);
    require_register(FifoStatus, state.fifo_status);
    require_register(Revision, state.revision);
    require_register(RootBusSplit, state.root_bus_split);
    if ((state.channel2_destination & ~0x13FFFFE0u) != 0u ||
        (state.channel2_destination & 0x10000000u) == 0u ||
        (state.channel2_length & ~0x00FFFFE0u) != 0u ||
        state.channel2_start > 1u ||
        (state.sort_start_address & 0x08000000u) == 0u ||
        (state.sort_base_address & 0x08000000u) == 0u ||
        state.sort_link_width > 1u ||
        state.sort_address_shift > 1u || state.sort_start > 1u ||
        state.dbreq_mask > 1u || state.bavl_wait_count > 0x1Fu ||
        state.channel2_priority > 0xFu ||
        state.channel2_max_burst > 3u ||
        state.texture_memory_mode0 > 1u ||
        state.texture_memory_mode1 > 1u ||
        (state.root_bus_split & ~0x80000000u) != 0u)
        throw std::invalid_argument(
            "Systembus-Handoff besitzt ungueltige Registerbits.");
    if (state.channel2_start != 0u &&
        (state.channel2_length == 0u ||
         !state.channel2_start_observer_bound))
        throw std::invalid_argument(
            "Systembus-Handoff besitzt keinen fortsetzbaren Channel-2-Transfer.");
}

void DreamcastSystemBusControl::restore_state_passive(
    const DreamcastSystemBusSnapshot& state) {
    validate_state_restore(state);
    registers_ = state.registers;
    system_reset_requests_ = state.system_reset_requests;
}

void DreamcastSystemBusControl::complete_channel2() noexcept {
    registers_[index(Channel2Start)] = 0u;
    registers_[index(Channel2Length)] = 0u;
}

bool DreamcastSystemBusControl::trigger_channel2() {
    if (registers_[index(Channel2Start)] != 0u ||
        registers_[index(Channel2Length)] == 0u || !channel2_start_observer_)
        return false;
    registers_[index(Channel2Start)] = 1u;
    try {
        channel2_start_observer_(registers_[index(Channel2Destination)],
                                 registers_[index(Channel2Length)]);
    } catch (...) {
        registers_[index(Channel2Start)] = 0u;
        throw;
    }
    return true;
}

void DreamcastSystemBusControl::set_system_reset_observer(SystemResetObserver observer) {
    system_reset_observer_ = std::move(observer);
}

DreamcastSystemAsic::DreamcastSystemAsic(PlatformInterruptRouter& router,
                                         const std::size_t event_capacity) noexcept
    : router_(router), event_capacity_(event_capacity) {}

void DreamcastSystemAsic::synchronize_lines() {
    for (std::size_t line = 0u; line < masks_.size(); ++line) {
        bool asserted = false;
        for (std::size_t bank = 0u; bank < pending_.size(); ++bank)
            asserted = asserted || (pending_[bank] & masks_[line][bank]) != 0u;
        router_.set_external_pending(line, asserted);
    }
}
void DreamcastSystemAsic::raise(const SystemAsicEvent event, const std::uint64_t guest_cycle) {
    if (total_events_ != 0u && guest_cycle < last_guest_cycle_)
        throw std::invalid_argument("System-ASIC-Ereignisse muessen gastzeitmonoton sein.");
    if (next_sequence_ == 0u)
        throw std::overflow_error("System-ASIC-Ereignisfolge ist erschoepft.");
    const auto [bank, bit] = event_bit(event);
    pending_[bank] |= bit;
    const auto sequence = next_sequence_;
    next_sequence_ =
        sequence == std::numeric_limits<std::uint64_t>::max() ? 0u : sequence + 1u;
    const auto record = SystemAsicEventRecord{guest_cycle, sequence, event};
    last_event_ = record;
    if (total_events_ != std::numeric_limits<std::uint64_t>::max()) ++total_events_;
    const auto note_dropped_event = [this]() noexcept {
        if (dropped_events_ != std::numeric_limits<std::uint64_t>::max())
            ++dropped_events_;
    };
    if (event_capacity_ == 0u) {
        note_dropped_event();
    } else {
        if (events_.size() == event_capacity_) {
            events_.pop_front();
            note_dropped_event();
        }
        try {
            events_.push_back(record);
        } catch (...) {
            // The bounded diagnostic history must never take down the guest interrupt path.
            note_dropped_event();
        }
    }
    last_guest_cycle_ = guest_cycle;
    synchronize_lines();
    if (bank < 2u) {
        if ((dma_trigger_masks_[0u][bank] & bit) != 0u && pvr_dma_trigger_observer_)
            pvr_dma_trigger_observer_(event);
        if ((dma_trigger_masks_[1u][bank] & bit) != 0u && g2_dma_trigger_observer_)
            g2_dma_trigger_observer_(event);
    }
}
SchedulerEventId DreamcastSystemAsic::schedule(EventScheduler& scheduler,
                                               const SystemAsicEvent event,
                                               const std::uint64_t guest_cycle) {
    static_cast<void>(event_bit(event));
    const auto effective_cycle =
        std::max(guest_cycle, scheduler.current_cycle());
    const auto event_id = scheduler.schedule_at(
        effective_cycle,
        [this, event](const auto restored_event_id, const auto cycle) {
            scheduled_events_.erase(
                std::remove_if(
                    scheduled_events_.begin(),
                    scheduled_events_.end(),
                    [restored_event_id](const auto& pending) {
                        return pending.event_id == restored_event_id;
                    }),
                scheduled_events_.end());
            raise(event, cycle);
        },
        SchedulerEventKind::SystemAsic);
    try {
        scheduled_events_.push_back({&scheduler,
                                     scheduler.lifetime_token(),
                                     effective_cycle,
                                     event,
                                     event_id,
                                     false});
    } catch (...) {
        static_cast<void>(scheduler.cancel(event_id));
        throw;
    }
    return event_id;
}
std::uint32_t DreamcastSystemAsic::read(const std::uint32_t offset) const {
    if (offset == 0x00u) {
        auto normal = pending_[0u] & 0x3FFFFFFFu;
        if (pending_[1u] != 0u) normal |= 1u << 30u;
        if (pending_[2u] != 0u) normal |= 1u << 31u;
        return normal;
    }
    if (offset <= 0x08u && offset % 4u == 0u) return pending_[offset / 4u];
    if (offset >= 0x10u && offset <= 0x38u && offset % 4u == 0u) {
        const auto linear = (offset - 0x10u) / 4u;
        if (linear / 4u >= masks_.size() || linear % 4u >= 3u)
            throw std::runtime_error("Unbekannter System-ASIC-MMIO-Maskenoffset.");
        return masks_[linear / 4u][linear % 4u];
    }
    if ((offset >= 0x40u && offset <= 0x44u) || (offset >= 0x50u && offset <= 0x54u)) {
        const auto group = static_cast<std::size_t>((offset - 0x40u) / 0x10u);
        const auto bank = static_cast<std::size_t>((offset & 0x0Fu) / 4u);
        return dma_trigger_masks_[group][bank];
    }
    throw std::runtime_error("Unbekannter System-ASIC-MMIO-Leseoffset.");
}
void DreamcastSystemAsic::write(const std::uint32_t offset, const std::uint32_t value) {
    if (offset == 0x00u) {
        pending_[0u] &= ~(value & 0x3FFFFFFFu);
        synchronize_lines();
        return;
    }
    if (offset <= 0x08u && offset % 4u == 0u) {
        pending_[offset / 4u] &= ~value;
        synchronize_lines();
        return;
    }
    if (offset >= 0x10u && offset <= 0x38u && offset % 4u == 0u) {
        const auto linear = (offset - 0x10u) / 4u;
        if (linear / 4u >= masks_.size() || linear % 4u >= 3u)
            throw std::runtime_error("Unbekannter System-ASIC-MMIO-Maskenoffset.");
        masks_[linear / 4u][linear % 4u] = value;
        synchronize_lines();
        return;
    }
    if ((offset >= 0x40u && offset <= 0x44u) || (offset >= 0x50u && offset <= 0x54u)) {
        const auto group = static_cast<std::size_t>((offset - 0x40u) / 0x10u);
        const auto bank = static_cast<std::size_t>((offset & 0x0Fu) / 4u);
        dma_trigger_masks_[group][bank] = value & (bank == 0u ? 0x003FFFFFu : 0x0000000Fu);
        return;
    }
    throw std::runtime_error("Unbekannter System-ASIC-MMIO-Schreiboffset.");
}
const std::deque<SystemAsicEventRecord>& DreamcastSystemAsic::events() const noexcept {
    return events_;
}

const std::optional<SystemAsicEventRecord>& DreamcastSystemAsic::last_event() const noexcept {
    return last_event_;
}

std::size_t DreamcastSystemAsic::event_capacity() const noexcept {
    return event_capacity_;
}

std::uint64_t DreamcastSystemAsic::total_event_count() const noexcept {
    return total_events_;
}

std::uint64_t DreamcastSystemAsic::dropped_event_count() const noexcept {
    return dropped_events_;
}

DreamcastSystemAsicSnapshot DreamcastSystemAsic::snapshot() const {
    DreamcastSystemAsicSnapshot result;
    result.pending = pending_;
    result.masks = masks_;
    result.dma_trigger_masks = dma_trigger_masks_;
    result.events = events_;
    result.last_event = last_event_;
    result.next_sequence = next_sequence_;
    result.last_guest_cycle = last_guest_cycle_;
    result.event_capacity = event_capacity_;
    result.total_events = total_events_;
    result.dropped_events = dropped_events_;
    result.pvr_dma_trigger_observer_bound =
        static_cast<bool>(pvr_dma_trigger_observer_);
    result.g2_dma_trigger_observer_bound =
        static_cast<bool>(g2_dma_trigger_observer_);
    result.scheduled_events.reserve(scheduled_events_.size());
    for (const auto& pending : scheduled_events_)
        result.scheduled_events.push_back(
            {pending.guest_cycle,
             pending.event,
             pending.event_id,
             pending.event_rehydration_pending});
    return result;
}

void DreamcastSystemAsic::validate_state_restore(
    const DreamcastSystemAsicSnapshot& state) const {
    if (state.event_capacity != event_capacity_ ||
        state.events.size() > state.event_capacity ||
        state.pvr_dma_trigger_observer_bound !=
            static_cast<bool>(pvr_dma_trigger_observer_) ||
        state.g2_dma_trigger_observer_bound !=
            static_cast<bool>(g2_dma_trigger_observer_))
        throw std::invalid_argument(
            "System-ASIC-Handoff passt nicht zum Runtime-Vertrag.");
    if (state.next_sequence == 0u && state.total_events == 0u)
        throw std::invalid_argument(
            "System-ASIC-Handoff besitzt eine ungueltige Sequenz.");
    std::uint64_t previous_cycle = 0u;
    std::uint64_t previous_sequence = 0u;
    bool first = true;
    for (const auto& event : state.events) {
        static_cast<void>(event_bit(event.event));
        if (event.sequence == 0u ||
            (!first &&
             (event.guest_cycle < previous_cycle ||
              event.sequence <= previous_sequence)))
            throw std::invalid_argument(
                "System-ASIC-Handoff besitzt eine ungueltige Ereignishistorie.");
        first = false;
        previous_cycle = event.guest_cycle;
        previous_sequence = event.sequence;
    }
    if (state.last_event) {
        static_cast<void>(event_bit(state.last_event->event));
        if (state.last_event->sequence == 0u ||
            state.last_event->guest_cycle != state.last_guest_cycle)
            throw std::invalid_argument(
                "System-ASIC-Handoff besitzt ein ungueltiges letztes Ereignis.");
    } else if (state.total_events != 0u || state.last_guest_cycle != 0u) {
        throw std::invalid_argument(
            "System-ASIC-Handoff fehlt das letzte Ereignis.");
    }
    if (state.total_events < state.events.size() ||
        state.dropped_events > state.total_events)
        throw std::invalid_argument(
            "System-ASIC-Handoff besitzt ungueltige Diagnosezaehler.");
    for (const auto& scheduled : state.scheduled_events) {
        static_cast<void>(event_bit(scheduled.event));
        if ((!scheduled.event_id &&
             !scheduled.event_rehydration_pending) ||
            (scheduled.event_id &&
             scheduled.event_rehydration_pending))
            throw std::invalid_argument(
                "System-ASIC-Handoff besitzt keinen eindeutigen Eventvertrag.");
    }
}

void DreamcastSystemAsic::restore_state_passive(
    EventScheduler& scheduler,
    const DreamcastSystemAsicSnapshot& state) {
    validate_state_restore(state);
    for (const auto& scheduled : state.scheduled_events)
        if (scheduled.guest_cycle < scheduler.current_cycle())
            throw std::invalid_argument(
                "System-ASIC-Handoff plant ein Ereignis in der Vergangenheit.");
    std::vector<ScheduledEvent> restored_events;
    restored_events.reserve(state.scheduled_events.size());
    for (const auto& scheduled : state.scheduled_events)
        restored_events.push_back({&scheduler,
                                   scheduler.lifetime_token(),
                                   scheduled.guest_cycle,
                                   scheduled.event,
                                   std::nullopt,
                                   true});
    auto restored_history = state.events;

    for (const auto& scheduled : scheduled_events_)
        if (scheduled.event_id && scheduled.scheduler &&
            !scheduled.scheduler_lifetime.expired())
            static_cast<void>(
                scheduled.scheduler->cancel(*scheduled.event_id));
    pending_ = state.pending;
    masks_ = state.masks;
    dma_trigger_masks_ = state.dma_trigger_masks;
    events_ = std::move(restored_history);
    last_event_ = state.last_event;
    next_sequence_ = state.next_sequence;
    last_guest_cycle_ = state.last_guest_cycle;
    total_events_ = state.total_events;
    dropped_events_ = state.dropped_events;
    scheduled_events_ = std::move(restored_events);
}

SchedulerEventId DreamcastSystemAsic::rehydrate_scheduled_event(
    EventScheduler& scheduler,
    const std::uint64_t guest_cycle,
    const std::uint32_t channel,
    const std::uint64_t token) {
    if (channel != dreamcast_system_asic_event_channel ||
        token > std::numeric_limits<std::uint16_t>::max())
        throw std::invalid_argument(
            "System-ASIC-Handoff besitzt einen unbekannten Eventkanal oder Token.");
    const auto event =
        static_cast<SystemAsicEvent>(static_cast<std::uint16_t>(token));
    static_cast<void>(event_bit(event));
    const auto pending = std::find_if(
        scheduled_events_.begin(),
        scheduled_events_.end(),
        [&](const auto& candidate) {
            return candidate.event_rehydration_pending &&
                   candidate.guest_cycle == guest_cycle &&
                   candidate.event == event;
        });
    if (pending == scheduled_events_.end() || pending->event_id)
        throw std::logic_error(
            "System-ASIC-Handoff erwartet dieses Ereignis nicht.");
    if (guest_cycle < scheduler.current_cycle())
        throw std::invalid_argument(
            "System-ASIC-Ereignis darf nicht in der Vergangenheit liegen.");
    const auto event_id = scheduler.schedule_at(
        guest_cycle,
        [this, event](const auto restored_event_id, const auto cycle) {
            scheduled_events_.erase(
                std::remove_if(
                    scheduled_events_.begin(),
                    scheduled_events_.end(),
                    [restored_event_id](const auto& candidate) {
                        return candidate.event_id == restored_event_id;
                    }),
                scheduled_events_.end());
            raise(event, cycle);
        },
        SchedulerEventKind::SystemAsic);
    pending->scheduler = &scheduler;
    pending->scheduler_lifetime = scheduler.lifetime_token();
    pending->event_id = event_id;
    pending->event_rehydration_pending = false;
    return event_id;
}

bool DreamcastSystemAsic::event_rehydration_pending() const noexcept {
    return std::any_of(
        scheduled_events_.begin(),
        scheduled_events_.end(),
        [](const auto& event) {
            return event.event_rehydration_pending;
        });
}

std::array<bool, 3u>
DreamcastSystemAsic::expected_external_lines() const noexcept {
    return calculate_expected_external_lines(pending_, masks_);
}
void DreamcastSystemAsic::set_dma_trigger_observers(DmaTriggerObserver pvr,
                                                     DmaTriggerObserver g2) {
    pvr_dma_trigger_observer_ = std::move(pvr);
    g2_dma_trigger_observer_ = std::move(g2);
}
void DreamcastSystemAsic::reset() noexcept {
    for (const auto& scheduled : scheduled_events_)
        if (scheduled.event_id && scheduled.scheduler &&
            !scheduled.scheduler_lifetime.expired())
            static_cast<void>(
                scheduled.scheduler->cancel(*scheduled.event_id));
    scheduled_events_.clear();
    pending_ = {};
    masks_ = {};
    dma_trigger_masks_ = {};
    events_.clear();
    last_event_.reset();
    next_sequence_ = 1u;
    last_guest_cycle_ = 0u;
    total_events_ = 0u;
    dropped_events_ = 0u;
    synchronize_lines();
}

namespace {

class AsicStateWriter final {
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

class AsicStateReader final {
  public:
    explicit AsicStateReader(const std::span<const std::uint8_t> bytes)
        : bytes_(bytes) {}
    [[nodiscard]] std::uint8_t u8() {
        require(1u);
        return bytes_[cursor_++];
    }
    [[nodiscard]] bool boolean() {
        const auto value = u8();
        if (value > 1u)
            throw std::invalid_argument(
                "System-State besitzt ein ungueltiges Boolean.");
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
                    "System-State besitzt ein ungueltiges Magic.");
    }
    void finish() const {
        if (cursor_ != bytes_.size())
            throw std::invalid_argument(
                "System-State besitzt nachlaufende Daten.");
    }
  private:
    void require(const std::size_t size) const {
        if (size > bytes_.size() - cursor_)
            throw std::invalid_argument("System-State ist abgeschnitten.");
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_ = 0u;
};

void write_event_record(AsicStateWriter& writer,
                        const SystemAsicEventRecord& record) {
    writer.u64(record.guest_cycle);
    writer.u64(record.sequence);
    writer.u32(static_cast<std::uint16_t>(record.event));
}

SystemAsicEventRecord read_event_record(AsicStateReader& reader) {
    return {reader.u64(),
            reader.u64(),
            static_cast<SystemAsicEvent>(
                static_cast<std::uint16_t>(reader.u32()))};
}

} // namespace

std::vector<std::uint8_t>
encode_dreamcast_system_bus_state(
    const DreamcastSystemBusSnapshot& state) {
    AsicStateWriter writer;
    writer.magic("KATBUS1");
    writer.u32(dreamcast_system_bus_state_contract_version);
    for (const auto value : state.registers) writer.u32(value);
    writer.u64(state.system_reset_requests);
    writer.boolean(state.channel2_start_observer_bound);
    writer.boolean(state.system_reset_observer_bound);
    return std::move(writer).finish();
}

DreamcastSystemBusSnapshot
decode_dreamcast_system_bus_state(
    const std::span<const std::uint8_t> bytes) {
    AsicStateReader reader(bytes);
    reader.magic("KATBUS1");
    if (reader.u32() != dreamcast_system_bus_state_contract_version)
        throw std::invalid_argument(
            "Systembus-State besitzt eine unbekannte Version.");
    DreamcastSystemBusSnapshot state;
    for (auto& value : state.registers) value = reader.u32();
    state.channel2_destination =
        state.registers[Channel2Destination / 4u];
    state.channel2_length = state.registers[Channel2Length / 4u];
    state.channel2_start = state.registers[Channel2Start / 4u];
    state.sort_start_address =
        state.registers[SortStartAddress / 4u];
    state.sort_base_address =
        state.registers[SortBaseAddress / 4u];
    state.sort_link_width = state.registers[SortLinkWidth / 4u];
    state.sort_address_shift =
        state.registers[SortAddressShift / 4u];
    state.sort_start = state.registers[SortStart / 4u];
    state.dbreq_mask = state.registers[DbreqMask / 4u];
    state.bavl_wait_count = state.registers[BavlWaitCount / 4u];
    state.channel2_priority =
        state.registers[Channel2Priority / 4u];
    state.channel2_max_burst =
        state.registers[Channel2MaxBurst / 4u];
    state.sort_divider = state.registers[SortDivider / 4u];
    state.ta_fifo_remaining =
        state.registers[TaFifoRemaining / 4u];
    state.texture_memory_mode0 =
        state.registers[TextureMemoryMode0 / 4u];
    state.texture_memory_mode1 =
        state.registers[TextureMemoryMode1 / 4u];
    state.fifo_status = state.registers[FifoStatus / 4u];
    state.revision = state.registers[Revision / 4u];
    state.root_bus_split =
        state.registers[RootBusSplit / 4u];
    state.system_reset_requests = reader.u64();
    state.channel2_start_observer_bound = reader.boolean();
    state.system_reset_observer_bound = reader.boolean();
    reader.finish();
    return state;
}

std::vector<std::uint8_t>
encode_dreamcast_system_asic_state(
    const DreamcastSystemAsicSnapshot& state) {
    AsicStateWriter writer;
    writer.magic("KATASI1");
    writer.u32(dreamcast_system_asic_state_contract_version);
    for (const auto value : state.pending) writer.u32(value);
    for (const auto& line : state.masks)
        for (const auto value : line) writer.u32(value);
    for (const auto& group : state.dma_trigger_masks)
        for (const auto value : group) writer.u32(value);
    if (state.events.size() > std::numeric_limits<std::uint32_t>::max() ||
        state.scheduled_events.size() >
            std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("System-ASIC-State ist zu gross.");
    writer.u32(static_cast<std::uint32_t>(state.events.size()));
    for (const auto& event : state.events)
        write_event_record(writer, event);
    writer.boolean(state.last_event.has_value());
    if (state.last_event) write_event_record(writer, *state.last_event);
    writer.u64(state.next_sequence);
    writer.u64(state.last_guest_cycle);
    writer.u64(state.event_capacity);
    writer.u64(state.total_events);
    writer.u64(state.dropped_events);
    writer.boolean(state.pvr_dma_trigger_observer_bound);
    writer.boolean(state.g2_dma_trigger_observer_bound);
    writer.u32(
        static_cast<std::uint32_t>(state.scheduled_events.size()));
    for (const auto& scheduled : state.scheduled_events) {
        writer.u64(scheduled.guest_cycle);
        writer.u32(static_cast<std::uint16_t>(scheduled.event));
        // Process-local scheduler IDs are deliberately omitted.
        writer.boolean(true);
    }
    return std::move(writer).finish();
}

DreamcastSystemAsicSnapshot
decode_dreamcast_system_asic_state(
    const std::span<const std::uint8_t> bytes) {
    AsicStateReader reader(bytes);
    reader.magic("KATASI1");
    if (reader.u32() != dreamcast_system_asic_state_contract_version)
        throw std::invalid_argument(
            "System-ASIC-State besitzt eine unbekannte Version.");
    DreamcastSystemAsicSnapshot state;
    for (auto& value : state.pending) value = reader.u32();
    for (auto& line : state.masks)
        for (auto& value : line) value = reader.u32();
    for (auto& group : state.dma_trigger_masks)
        for (auto& value : group) value = reader.u32();
    const auto event_count = reader.u32();
    for (std::uint32_t index = 0u; index < event_count; ++index)
        state.events.push_back(read_event_record(reader));
    if (reader.boolean()) state.last_event = read_event_record(reader);
    state.next_sequence = reader.u64();
    state.last_guest_cycle = reader.u64();
    state.event_capacity = static_cast<std::size_t>(reader.u64());
    state.total_events = reader.u64();
    state.dropped_events = reader.u64();
    state.pvr_dma_trigger_observer_bound = reader.boolean();
    state.g2_dma_trigger_observer_bound = reader.boolean();
    const auto scheduled_count = reader.u32();
    state.scheduled_events.reserve(scheduled_count);
    for (std::uint32_t index = 0u; index < scheduled_count; ++index) {
        DreamcastSystemAsicSnapshot::ScheduledEvent scheduled;
        scheduled.guest_cycle = reader.u64();
        scheduled.event =
            static_cast<SystemAsicEvent>(
                static_cast<std::uint16_t>(reader.u32()));
        scheduled.event_id.reset();
        scheduled.event_rehydration_pending = reader.boolean();
        state.scheduled_events.push_back(scheduled);
    }
    reader.finish();
    return state;
}

std::shared_ptr<DreamcastSystemBusControl> map_dreamcast_system_bus_control(Memory& memory) {
    return map_dreamcast_system_bus_control(memory, {});
}

std::shared_ptr<DreamcastSystemBusControl> map_dreamcast_system_bus_control(
    Memory& memory, DreamcastSystemBusControl::Channel2StartObserver channel2_start_observer) {
    auto control =
        std::make_shared<DreamcastSystemBusControl>(std::move(channel2_start_observer));
    control->reset();
    auto device = std::make_shared<MmioMemoryDevice>(
        system_bus_control_register_size,
        [control](const auto offset, const auto width) {
            if (width != MemoryAccessWidth::Word)
                throw std::runtime_error("Systembus-Steuerregister erfordern 32-Bit-MMIO.");
            return control->read(offset);
        },
        [control](const auto offset, const auto value, const auto width) {
            if (width != MemoryAccessWidth::Word)
                throw std::runtime_error("Systembus-Steuerregister erfordern 32-Bit-MMIO.");
            control->write(offset, value);
        });
    for (const auto segment : dreamcast_direct_segment_bases)
        memory.map_region("dreamcast-system-bus-control-" + std::to_string(segment),
                          segment + system_bus_control_physical_base,
                          device);
    return control;
}
std::shared_ptr<DreamcastSystemAsic>
map_dreamcast_system_asic(Memory& memory,
                          PlatformInterruptRouter& router,
                          std::shared_ptr<MemoryDevice>* const mapped_device_out) {
    auto asic = std::make_shared<DreamcastSystemAsic>(router);
    auto device = std::make_shared<MmioMemoryDevice>(
        system_asic_register_size,
        [asic](const auto offset, const auto width) {
            if (width != MemoryAccessWidth::Word)
                throw std::runtime_error("System-ASIC erfordert 32-Bit-MMIO.");
            return asic->read(offset);
        },
        [asic](const auto offset, const auto value, const auto width) {
            if (width != MemoryAccessWidth::Word)
                throw std::runtime_error("System-ASIC erfordert 32-Bit-MMIO.");
            asic->write(offset, value);
        });
    for (const auto segment : dreamcast_direct_segment_bases)
        memory.map_region("dreamcast-system-asic-" + std::to_string(segment),
                          segment + system_asic_physical_base,
                          device);
    if (mapped_device_out != nullptr) *mapped_device_out = device;
    return asic;
}
} // namespace katana::runtime

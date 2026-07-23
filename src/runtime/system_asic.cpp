#include "katana/runtime/system_asic.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <array>
#include <stdexcept>
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

DreamcastSystemBusSnapshot DreamcastSystemBusControl::snapshot() const noexcept {
    return {
        registers_[Channel2Destination / 4u],
        registers_[Channel2Length / 4u],
        registers_[Channel2Start / 4u],
        system_reset_requests_,
    };
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

DreamcastSystemAsic::DreamcastSystemAsic(PlatformInterruptRouter& router) noexcept
    : router_(router) {}

void DreamcastSystemAsic::synchronize_lines() {
    for (std::size_t line = 0u; line < masks_.size(); ++line) {
        bool asserted = false;
        for (std::size_t bank = 0u; bank < pending_.size(); ++bank)
            asserted = asserted || (pending_[bank] & masks_[line][bank]) != 0u;
        router_.set_external_pending(line, asserted);
    }
}
void DreamcastSystemAsic::raise(const SystemAsicEvent event, const std::uint64_t guest_cycle) {
    if (!events_.empty() && guest_cycle < last_guest_cycle_)
        throw std::invalid_argument("System-ASIC-Ereignisse muessen gastzeitmonoton sein.");
    const auto [bank, bit] = event_bit(event);
    pending_[bank] |= bit;
    events_.push_back({guest_cycle, next_sequence_++, event});
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
    return scheduler.schedule_at_or_now(
        guest_cycle, [this, event](const auto, const auto cycle) { raise(event, cycle); });
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
const std::vector<SystemAsicEventRecord>& DreamcastSystemAsic::events() const noexcept {
    return events_;
}
void DreamcastSystemAsic::set_dma_trigger_observers(DmaTriggerObserver pvr,
                                                     DmaTriggerObserver g2) {
    pvr_dma_trigger_observer_ = std::move(pvr);
    g2_dma_trigger_observer_ = std::move(g2);
}
void DreamcastSystemAsic::reset() noexcept {
    pending_ = {};
    masks_ = {};
    dma_trigger_masks_ = {};
    events_.clear();
    next_sequence_ = 1u;
    last_guest_cycle_ = 0u;
    synchronize_lines();
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
std::shared_ptr<DreamcastSystemAsic> map_dreamcast_system_asic(Memory& memory,
                                                               PlatformInterruptRouter& router) {
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
    return asic;
}
} // namespace katana::runtime

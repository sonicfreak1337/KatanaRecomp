#include "katana/runtime/system_asic.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <stdexcept>

namespace katana::runtime {
namespace {
std::pair<std::size_t, std::uint32_t> event_bit(const SystemAsicEvent event) {
    const auto code = static_cast<std::uint16_t>(event);
    const auto bank = static_cast<std::size_t>((code >> 8u) & 0xFFu);
    const auto bit = static_cast<std::uint32_t>(code & 0xFFu);
    if (bank >= 3u || bit >= 32u) throw std::invalid_argument("Ungueltiges System-ASIC-Ereignis.");
    return {bank, std::uint32_t{1u} << bit};
}
} // namespace

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
}
SchedulerEventId DreamcastSystemAsic::schedule(EventScheduler& scheduler,
                                               const SystemAsicEvent event,
                                               const std::uint64_t guest_cycle) {
    return scheduler.schedule_at_or_now(
        guest_cycle, [this, event](const auto, const auto cycle) { raise(event, cycle); });
}
std::uint32_t DreamcastSystemAsic::read(const std::uint32_t offset) const {
    if (offset <= 0x08u && offset % 4u == 0u) return pending_[offset / 4u];
    if (offset >= 0x10u && offset <= 0x38u && offset % 4u == 0u) {
        const auto linear = (offset - 0x10u) / 4u;
        if (linear / 4u >= masks_.size() || linear % 4u >= 3u)
            throw std::runtime_error("Unbekannter System-ASIC-MMIO-Maskenoffset.");
        return masks_[linear / 4u][linear % 4u];
    }
    throw std::runtime_error("Unbekannter System-ASIC-MMIO-Leseoffset.");
}
void DreamcastSystemAsic::write(const std::uint32_t offset, const std::uint32_t value) {
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
    throw std::runtime_error("Unbekannter System-ASIC-MMIO-Schreiboffset.");
}
const std::vector<SystemAsicEventRecord>& DreamcastSystemAsic::events() const noexcept {
    return events_;
}
void DreamcastSystemAsic::reset() noexcept {
    pending_ = {};
    masks_ = {};
    events_.clear();
    next_sequence_ = 1u;
    last_guest_cycle_ = 0u;
    synchronize_lines();
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

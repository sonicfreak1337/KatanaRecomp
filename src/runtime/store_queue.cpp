#include "katana/runtime/store_queue.hpp"

#include <cstddef>
#include <utility>

namespace katana::runtime {

Sh4StoreQueues::Sh4StoreQueues(Memory& memory,
                               StoreQueueSink sink,
                               ExecutableCodeTracker* code_tracker,
                               const OperandCacheRamProfile ocram_profile)
    : memory_(memory), sink_(std::move(sink)), code_tracker_(code_tracker),
      ocram_profile_(ocram_profile) {}

std::size_t Sh4StoreQueues::queue_index(const std::uint32_t address) noexcept {
    return (address >> 5u) & 1u;
}

void Sh4StoreQueues::write_qacr(const std::size_t queue, const std::uint32_t value) {
    if (queue >= qacr_.size() || (value & ~qacr_mask) != 0u) {
        throw std::invalid_argument("QACR akzeptiert nur Queue 0/1 und Zielbits 2..4.");
    }
    qacr_[queue] = value;
}
std::uint32_t Sh4StoreQueues::qacr(const std::size_t queue) const {
    if (queue >= qacr_.size()) {
        throw std::out_of_range("Ungueltige Store Queue.");
    }
    return qacr_[queue];
}

std::uint32_t Sh4StoreQueues::read_p4(const std::uint32_t address,
                                      const MemoryAccessWidth width) const {
    if (address < read_window_start || address > read_window_end ||
        width != MemoryAccessWidth::Word || (address & 3u) != 0u) {
        throw std::invalid_argument(
            "Store-Queue-Lesezugriff braucht ein ausgerichtetes Longword im P4-Lesefenster.");
    }
    const auto& queue_bytes = queues_[queue_index(address)];
    const auto offset = static_cast<std::size_t>(address & 31u);
    std::uint32_t value = 0u;
    for (std::size_t index = 0u; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(queue_bytes[offset + index]) << (index * 8u);
    }
    return value;
}

void Sh4StoreQueues::write_p4(const std::uint32_t address,
                              const std::uint32_t value,
                              const MemoryAccessWidth width) {
    if (address < window_start || address > window_end) {
        throw std::out_of_range("Store-Queue-Schreibzugriff liegt ausserhalb des P4-Fensters.");
    }
    const auto bytes = static_cast<std::size_t>(width);
    const auto offset = static_cast<std::size_t>(address & 31u);
    if (offset + bytes > 32u || (address & (bytes - 1u)) != 0u) {
        throw std::invalid_argument(
            "Store-Queue-Schreibzugriff ist falsch ausgerichtet oder kreuzt die Queue.");
    }
    auto& queue_bytes = queues_[queue_index(address)];
    for (std::size_t index = 0u; index < bytes; ++index) {
        queue_bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
    }
}

std::uint32_t Sh4StoreQueues::transfer_target(const std::uint32_t address,
                                              const std::size_t queue) const noexcept {
    return (address & 0x03FFFFE0u) | ((qacr_[queue] & qacr_mask) << 24u);
}

bool Sh4StoreQueues::prefetch(const std::uint32_t address,
                              const GuestInstructionOrigin instruction,
                              const std::uint64_t retired_guest_instructions) {
    if (address < window_start || address > window_end) {
        return false;
    }
    if ((address & 3u) != 0u) {
        throw MemoryAccessError(MemoryAccessErrorReason::Unmapped,
                                MemoryAccessOperation::Write,
                                address,
                                MemoryAccessWidth::Word,
                                "sh4-store-queue-prefetch");
    }
    const auto translated = address_translator_
                                ? address_translator_(address)
                                : StoreQueuePrefetchTranslation{
                                      address, 0u, StoreQueueAddressingMode::Qacr};
    const auto selected = queue_index(address);
    StoreQueueTransfer transfer;
    transfer.queue = static_cast<std::uint8_t>(selected);
    transfer.source_address = address & ~31u;
    transfer.target_address = translated.addressing == StoreQueueAddressingMode::Utlb
                                  ? translated.target_address
                                  : transfer_target(address, selected);
    const auto ta_input =
        (transfer.target_address >= 0x10000000u && transfer.target_address <= 0x107FFFFFu) ||
        (transfer.target_address >= 0x12000000u && transfer.target_address <= 0x127FFFFFu);
    transfer.target = ta_input ? StoreQueueTarget::TileAccelerator : StoreQueueTarget::Ram;
    transfer.instruction = instruction;
    transfer.retired_guest_instructions = retired_guest_instructions;
    transfer.bytes = queues_[selected];
    if (sink_) {
        sink_(transfer);
    } else {
        memory_.write_bytes_at(
            transfer.target_address,
            transfer.bytes,
            GuestMemoryAccessContext{
                transfer.target_address, instruction, retired_guest_instructions},
            CodeWriteSource::StoreQueue);
    }
    ++transfer_count_;
    return true;
}

void Sh4StoreQueues::set_prefetch_address_translator(StoreQueueAddressTranslator translator) {
    address_translator_ = std::move(translator);
}

const std::array<std::uint8_t, 32u>& Sh4StoreQueues::queue(const std::size_t index) const {
    if (index >= queues_.size()) {
        throw std::out_of_range("Ungueltige Store Queue.");
    }
    return queues_[index];
}
std::uint64_t Sh4StoreQueues::transfer_count() const noexcept {
    return transfer_count_;
}

Sh4StoreQueueSnapshot Sh4StoreQueues::snapshot() const noexcept {
    return {
        queues_,
        qacr_,
        operand_cache_ram_,
        ocram_profile_,
        operand_cache_ram_enabled_,
        static_cast<bool>(sink_),
        static_cast<bool>(address_translator_),
        code_tracker_ != nullptr,
        transfer_count_,
    };
}

CacheMaintenanceResult Sh4StoreQueues::maintain(const CacheMaintenanceOperation operation,
                                                const std::uint32_t address,
                                                const std::uint32_t movca_value) {
    CacheMaintenanceResult result{operation, address, false, false};
    if (operation == CacheMaintenanceOperation::MovcaLong) {
        const auto before = code_tracker_ != nullptr ? code_tracker_->invalidation_count() : 0u;
        memory_.write_u32(address, movca_value, CodeWriteSource::StoreQueue);
        result.wrote_memory = true;
        result.invalidated_code =
            code_tracker_ != nullptr && code_tracker_->invalidation_count() != before;
    } else if (operation == CacheMaintenanceOperation::Icbi && code_tracker_) {
        const auto invalidation =
            code_tracker_->observe_write(address & ~31u, 32u, CodeWriteSource::Cpu);
        result.invalidated_code = !invalidation.invalidated_blocks.empty();
    } else if (operation == CacheMaintenanceOperation::Ocbi ||
               operation == CacheMaintenanceOperation::Ocbp ||
               operation == CacheMaintenanceOperation::Ocbwb) {
        throw std::runtime_error(
            "Operand-Cache-Tags, Dirty-Zustand und Write-back sind nicht modelliert.");
    }
    return result;
}

void Sh4StoreQueues::set_operand_cache_ram_enabled(const bool enabled) {
    if (enabled && ocram_profile_ == OperandCacheRamProfile::Reject) {
        throw std::runtime_error(
            "CCR-Operand-Cache-RAM ist im aktiven LLE-Profil nicht modelliert.");
    }
    operand_cache_ram_enabled_ = enabled;
}
bool Sh4StoreQueues::operand_cache_ram_enabled() const noexcept {
    return operand_cache_ram_enabled_;
}
std::uint32_t Sh4StoreQueues::read_operand_cache_ram(const std::uint32_t offset,
                                                     const MemoryAccessWidth width) const {
    const auto bytes = static_cast<std::size_t>(width);
    if (!operand_cache_ram_enabled_ || offset > operand_cache_ram_.size() ||
        bytes > operand_cache_ram_.size() - offset || (offset & (bytes - 1u)) != 0u) {
        throw std::out_of_range("Operand-Cache-RAM ist inaktiv oder der Offset ist ungueltig.");
    }
    std::uint32_t value = 0u;
    for (std::size_t index = 0u; index < bytes; ++index) {
        value |= static_cast<std::uint32_t>(operand_cache_ram_[offset + index]) << (index * 8u);
    }
    return value;
}
void Sh4StoreQueues::write_operand_cache_ram(const std::uint32_t offset,
                                             const std::uint32_t value,
                                             const MemoryAccessWidth width) {
    const auto bytes = static_cast<std::size_t>(width);
    if (!operand_cache_ram_enabled_ || offset > operand_cache_ram_.size() ||
        bytes > operand_cache_ram_.size() - offset || (offset & (bytes - 1u)) != 0u) {
        throw std::out_of_range("Operand-Cache-RAM ist inaktiv oder der Offset ist ungueltig.");
    }
    for (std::size_t index = 0u; index < bytes; ++index) {
        operand_cache_ram_[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
    }
}

} // namespace katana::runtime

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
    return (address >> 25u) & 1u;
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

bool Sh4StoreQueues::prefetch(const std::uint32_t address) {
    if (address < window_start || address > window_end) {
        return false;
    }
    const auto selected = queue_index(address);
    StoreQueueTransfer transfer;
    transfer.queue = static_cast<std::uint8_t>(selected);
    transfer.source_address = address & ~31u;
    transfer.target_address = transfer_target(address, selected);
    transfer.target =
        transfer.target_address >= 0x10000000u && transfer.target_address <= 0x13FFFFFFu
            ? StoreQueueTarget::TileAccelerator
            : StoreQueueTarget::Ram;
    transfer.bytes = queues_[selected];
    if (sink_) {
        sink_(transfer);
    } else {
        for (std::size_t index = 0u; index < transfer.bytes.size(); ++index) {
            memory_.write_u8(transfer.target_address + static_cast<std::uint32_t>(index),
                             transfer.bytes[index]);
        }
    }
    if (code_tracker_ != nullptr && transfer.target == StoreQueueTarget::Ram) {
        static_cast<void>(code_tracker_->observe_write(
            transfer.target_address, transfer.bytes.size(), CodeWriteSource::Cpu));
    }
    ++transfer_count_;
    return true;
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

CacheMaintenanceResult Sh4StoreQueues::maintain(const CacheMaintenanceOperation operation,
                                                const std::uint32_t address,
                                                const std::uint32_t movca_value) {
    CacheMaintenanceResult result{operation, address, false, false};
    if (operation == CacheMaintenanceOperation::MovcaLong) {
        memory_.write_u32(address, movca_value);
        result.wrote_memory = true;
        if (code_tracker_) {
            static_cast<void>(code_tracker_->observe_write(address, 4u, CodeWriteSource::Cpu));
            result.invalidated_code = true;
        }
    } else if (operation == CacheMaintenanceOperation::Icbi && code_tracker_) {
        static_cast<void>(code_tracker_->observe_write(address, 32u, CodeWriteSource::Cpu));
        result.invalidated_code = true;
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
std::uint8_t Sh4StoreQueues::read_operand_cache_ram(const std::uint32_t offset) const {
    if (!operand_cache_ram_enabled_ || offset >= operand_cache_ram_.size()) {
        throw std::out_of_range("Operand-Cache-RAM ist inaktiv oder der Offset ist ungueltig.");
    }
    return operand_cache_ram_[offset];
}
void Sh4StoreQueues::write_operand_cache_ram(const std::uint32_t offset, const std::uint8_t value) {
    if (!operand_cache_ram_enabled_ || offset >= operand_cache_ram_.size()) {
        throw std::out_of_range("Operand-Cache-RAM ist inaktiv oder der Offset ist ungueltig.");
    }
    operand_cache_ram_[offset] = value;
}

} // namespace katana::runtime

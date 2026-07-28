#include "katana/runtime/store_queue.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace katana::runtime {

StoreQueueSinkError::StoreQueueSinkError(const StoreQueueSinkErrorReason reason,
                                         std::string detail,
                                         std::string packet_class)
    : std::runtime_error(std::move(detail)), reason_(reason),
      packet_class_(std::move(packet_class)) {}

StoreQueueSinkErrorReason StoreQueueSinkError::reason() const noexcept {
    return reason_;
}

const std::string& StoreQueueSinkError::packet_class() const noexcept {
    return packet_class_;
}

StoreQueuePrefetchRejected::StoreQueuePrefetchRejected(StoreQueueSinkFault fault)
    : std::runtime_error("store-queue-prefetch-rejected: " + fault.detail),
      fault_(std::move(fault)) {}

const StoreQueueSinkFault& StoreQueuePrefetchRejected::fault() const noexcept {
    return fault_;
}

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
                              const std::uint64_t retired_guest_instructions,
                              const std::uint64_t attempted_guest_instructions) {
    const auto result = prefetch_result(
        address, instruction, retired_guest_instructions, attempted_guest_instructions);
    if (result == StoreQueuePrefetchResult::Rejected) {
        if (last_sink_fault_) throw StoreQueuePrefetchRejected(*last_sink_fault_);
        throw StoreQueuePrefetchRejected(
            {StoreQueueSinkErrorReason::DeviceRejected,
             address & ~31u,
             0u,
             "Store-Queue-Sink lehnte PREF ohne Fehlerdetail ab.",
             {},
             instruction});
    }
    return result == StoreQueuePrefetchResult::Transferred;
}

StoreQueuePrefetchResult
Sh4StoreQueues::prefetch_result(const std::uint32_t address,
                                const GuestInstructionOrigin instruction,
                                const std::uint64_t retired_guest_instructions,
                                const std::uint64_t attempted_guest_instructions) {
    if (address < window_start || address > window_end) {
        return StoreQueuePrefetchResult::NotStoreQueueAddress;
    }
    if ((address & 3u) != 0u) {
        throw MemoryAccessError(MemoryAccessErrorReason::Misaligned,
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
    transfer.attempted_guest_instructions = attempted_guest_instructions;
    transfer.bytes = queues_[selected];
    if (sink_) {
        try {
            sink_(transfer);
        } catch (const StoreQueueSinkError& error) {
            ++rejected_transfer_count_;
            last_sink_fault_.reset();
            try {
                last_sink_fault_ = StoreQueueSinkFault{
                    error.reason(),
                    transfer.source_address,
                    transfer.target_address,
                    error.what(),
                    error.packet_class(),
                    instruction};
            } catch (...) {
                try {
                    auto& fallback = last_sink_fault_.emplace();
                    fallback.reason = error.reason();
                    fallback.source_address = transfer.source_address;
                    fallback.target_address = transfer.target_address;
                    fallback.instruction = instruction;
                } catch (...) {
                    last_sink_fault_.reset();
                }
            }
            return StoreQueuePrefetchResult::Rejected;
        }
    } else {
        memory_.write_bytes_at(
            transfer.target_address,
            transfer.bytes,
            GuestMemoryAccessContext{transfer.target_address,
                                     instruction,
                                     retired_guest_instructions,
                                     GuestMemoryAccessOrigin::Memory,
                                     attempted_guest_instructions},
            CodeWriteSource::StoreQueue);
    }
    ++transfer_count_;
    return StoreQueuePrefetchResult::Transferred;
}

void Sh4StoreQueues::set_prefetch_address_translator(StoreQueueAddressTranslator translator) {
    address_translator_ = std::move(translator);
}

void Sh4StoreQueues::bind_runtime_block_table(RuntimeBlockTable* const blocks) noexcept {
    runtime_blocks_ = blocks;
    shared_code_invalidation_bound_ = false;
    shared_code_tracker_.reset();
    shared_runtime_blocks_.reset();
}

void Sh4StoreQueues::bind_runtime_code_invalidation(
    const std::shared_ptr<ExecutableCodeTracker>& tracker,
    const std::shared_ptr<RuntimeBlockTable>& blocks) noexcept {
    shared_code_tracker_ = tracker;
    shared_runtime_blocks_ = blocks;
    shared_code_invalidation_bound_ = true;
    code_tracker_ = nullptr;
    runtime_blocks_ = nullptr;
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

std::uint64_t Sh4StoreQueues::rejected_transfer_count() const noexcept {
    return rejected_transfer_count_;
}

const std::optional<StoreQueueSinkFault>& Sh4StoreQueues::last_sink_fault() const noexcept {
    return last_sink_fault_;
}

Sh4StoreQueueSnapshot Sh4StoreQueues::snapshot() const {
    const bool code_tracker_bound =
        shared_code_invalidation_bound_ ? !shared_code_tracker_.expired()
                                        : code_tracker_ != nullptr;
    return {
        queues_,
        qacr_,
        operand_cache_ram_,
        ocram_profile_,
        operand_cache_ram_enabled_,
        static_cast<bool>(sink_),
        static_cast<bool>(address_translator_),
        code_tracker_bound,
        transfer_count_,
        rejected_transfer_count_,
        last_sink_fault_,
    };
}

void Sh4StoreQueues::validate_state_restore(
    const Sh4StoreQueueSnapshot& state) const {
    const auto live = snapshot();
    if (state.operand_cache_ram_profile != ocram_profile_ ||
        state.external_sink_bound != live.external_sink_bound ||
        state.address_translator_bound != live.address_translator_bound ||
        state.code_tracker_bound != live.code_tracker_bound ||
        (state.operand_cache_ram_enabled &&
         state.operand_cache_ram_profile == OperandCacheRamProfile::Reject) ||
        std::any_of(state.qacr.begin(), state.qacr.end(), [](const auto value) {
            return (value & ~qacr_mask) != 0u;
        }))
        throw std::invalid_argument(
            "Store-Queue-Handoff passt nicht zur Runtime-Topologie.");
    if (state.last_sink_fault &&
        state.last_sink_fault->reason !=
            StoreQueueSinkErrorReason::DeviceRejected &&
        state.last_sink_fault->reason !=
            StoreQueueSinkErrorReason::UnsupportedInput)
        throw std::invalid_argument(
            "Store-Queue-Handoff besitzt einen ungueltigen Fehlergrund.");
}

void Sh4StoreQueues::restore_state_passive(
    const Sh4StoreQueueSnapshot& state) {
    validate_state_restore(state);
    commit_validated_state_restore(state);
}

void Sh4StoreQueues::commit_validated_state_restore(
    Sh4StoreQueueSnapshot state) noexcept {
    queues_ = state.queues;
    qacr_ = state.qacr;
    operand_cache_ram_ = state.operand_cache_ram;
    operand_cache_ram_enabled_ = state.operand_cache_ram_enabled;
    transfer_count_ = state.transfer_count;
    rejected_transfer_count_ = state.rejected_transfer_count;
    last_sink_fault_ = std::move(state.last_sink_fault);
}

void Sh4StoreQueues::reset() noexcept {
    queues_.fill({});
    qacr_.fill(0u);
    operand_cache_ram_.fill(0u);
    operand_cache_ram_enabled_ = false;
    transfer_count_ = 0u;
    rejected_transfer_count_ = 0u;
    last_sink_fault_.reset();
}

CacheMaintenanceResult Sh4StoreQueues::maintain(const CacheMaintenanceOperation operation,
                                                const std::uint32_t address,
                                                const std::uint32_t movca_value) {
    auto shared_tracker = shared_code_invalidation_bound_
                              ? shared_code_tracker_.lock()
                              : std::shared_ptr<ExecutableCodeTracker>{};
    auto shared_blocks = shared_code_invalidation_bound_
                             ? shared_runtime_blocks_.lock()
                             : std::shared_ptr<RuntimeBlockTable>{};
    auto* const code_tracker =
        shared_code_invalidation_bound_ ? shared_tracker.get() : code_tracker_;
    auto* const runtime_blocks =
        shared_code_invalidation_bound_ ? shared_blocks.get() : runtime_blocks_;
    CacheMaintenanceResult result{operation, address, false, false};
    if (operation == CacheMaintenanceOperation::MovcaLong) {
        const auto before =
            code_tracker != nullptr ? code_tracker->invalidation_count() : 0u;
        memory_.write_u32(address, movca_value, CodeWriteSource::StoreQueue);
        result.wrote_memory = true;
        result.invalidated_code =
            code_tracker != nullptr && code_tracker->invalidation_count() != before;
    } else if (operation == CacheMaintenanceOperation::Icbi) {
        const auto line = address & ~31u;
        if (code_tracker != nullptr) {
            const auto invalidation =
                code_tracker->observe_write(line, 32u, CodeWriteSource::Cpu);
            result.invalidated_code = !invalidation.invalidated_blocks.empty();
        }
        if (runtime_blocks != nullptr) {
            result.invalidated_code =
                runtime_blocks->erase_overlapping_physical(line, 32u) != 0u ||
                result.invalidated_code;
        }
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

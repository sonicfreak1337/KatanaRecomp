#pragma once

#include "katana/runtime/code_invalidation.hpp"
#include "katana/runtime/memory.hpp"
#include "katana/runtime/runtime.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

namespace katana::runtime {

class RuntimeBlockTable;

enum class StoreQueueTarget : std::uint8_t { Ram, TileAccelerator };
enum class StoreQueuePrefetchResult : std::uint8_t {
    NotStoreQueueAddress,
    Transferred,
    Rejected,
};
enum class CacheMaintenanceOperation : std::uint8_t { Ocbi, Ocbp, Ocbwb, Icbi, MovcaLong };
enum class OperandCacheRamProfile : std::uint8_t { Reject, Modeled };
enum class StoreQueueSinkErrorReason : std::uint8_t {
    DeviceRejected,
    UnsupportedInput,
};

class StoreQueueSinkError final : public std::runtime_error {
  public:
    StoreQueueSinkError(StoreQueueSinkErrorReason reason,
                        std::string detail,
                        std::string packet_class = {});
    [[nodiscard]] StoreQueueSinkErrorReason reason() const noexcept;
    [[nodiscard]] const std::string& packet_class() const noexcept;

  private:
    StoreQueueSinkErrorReason reason_;
    std::string packet_class_;
};

struct StoreQueueSinkFault {
    StoreQueueSinkErrorReason reason = StoreQueueSinkErrorReason::DeviceRejected;
    std::uint32_t source_address = 0u;
    std::uint32_t target_address = 0u;
    std::string detail;
    std::string packet_class;
    GuestInstructionOrigin instruction;

    [[nodiscard]] bool operator==(const StoreQueueSinkFault& other) const {
        return reason == other.reason && source_address == other.source_address &&
               target_address == other.target_address && detail == other.detail &&
               packet_class == other.packet_class &&
               instruction.source_pc == other.instruction.source_pc &&
               instruction.runtime_pc == other.instruction.runtime_pc &&
               instruction.valid == other.instruction.valid;
    }
};

class StoreQueuePrefetchRejected final : public std::runtime_error {
  public:
    explicit StoreQueuePrefetchRejected(StoreQueueSinkFault fault);
    [[nodiscard]] const StoreQueueSinkFault& fault() const noexcept;

  private:
    StoreQueueSinkFault fault_;
};

struct StoreQueueTransfer {
    std::uint8_t queue = 0u;
    std::uint32_t source_address = 0u;
    std::uint32_t target_address = 0u;
    StoreQueueTarget target = StoreQueueTarget::Ram;
    GuestInstructionOrigin instruction;
    std::uint64_t retired_guest_instructions = 0u;
    std::uint64_t attempted_guest_instructions = 0u;
    std::array<std::uint8_t, 32u> bytes{};
};

struct CacheMaintenanceResult {
    CacheMaintenanceOperation operation = CacheMaintenanceOperation::Ocbi;
    std::uint32_t address = 0u;
    bool wrote_memory = false;
    bool invalidated_code = false;
};

struct Sh4StoreQueueSnapshot {
    std::array<std::array<std::uint8_t, 32u>, 2u> queues{};
    std::array<std::uint32_t, 2u> qacr{};
    std::array<std::uint8_t, 8192u> operand_cache_ram{};
    OperandCacheRamProfile operand_cache_ram_profile = OperandCacheRamProfile::Reject;
    bool operand_cache_ram_enabled = false;
    bool external_sink_bound = false;
    bool address_translator_bound = false;
    bool code_tracker_bound = false;
    std::uint64_t transfer_count = 0u;
    std::uint64_t rejected_transfer_count = 0u;
    std::optional<StoreQueueSinkFault> last_sink_fault;

    [[nodiscard]] bool operator==(const Sh4StoreQueueSnapshot&) const = default;
};

using StoreQueueSink = std::function<void(const StoreQueueTransfer&)>;
using StoreQueueAddressTranslator =
    std::function<StoreQueuePrefetchTranslation(std::uint32_t)>;

class Sh4StoreQueues {
  public:
    static constexpr std::uint32_t window_start = 0xE0000000u;
    static constexpr std::uint32_t window_end = 0xE3FFFFFFu;
    static constexpr std::uint32_t read_window_start = 0xFF001000u;
    static constexpr std::uint32_t read_window_end = 0xFF00103Fu;
    static constexpr std::uint32_t qacr_mask = 0x0000001Cu;

    explicit Sh4StoreQueues(Memory& memory,
                            StoreQueueSink sink = {},
                            ExecutableCodeTracker* code_tracker = nullptr,
                            OperandCacheRamProfile ocram_profile = OperandCacheRamProfile::Reject);
    void write_qacr(std::size_t queue, std::uint32_t value);
    [[nodiscard]] std::uint32_t qacr(std::size_t queue) const;
    [[nodiscard]] std::uint32_t read_p4(std::uint32_t address, MemoryAccessWidth width) const;
    void write_p4(std::uint32_t address, std::uint32_t value, MemoryAccessWidth width);
    [[nodiscard]] bool prefetch(std::uint32_t address,
                                GuestInstructionOrigin instruction = {},
                                std::uint64_t retired_guest_instructions = 0u,
                                std::uint64_t attempted_guest_instructions = 0u);
    [[nodiscard]] StoreQueuePrefetchResult
    prefetch_result(std::uint32_t address,
                    GuestInstructionOrigin instruction = {},
                    std::uint64_t retired_guest_instructions = 0u,
                    std::uint64_t attempted_guest_instructions = 0u);
    void set_prefetch_address_translator(StoreQueueAddressTranslator translator);
    // Scoped integrations may bind a raw table. Product runtimes should bind
    // shared invalidation owners so Memory-held MMIO callbacks cannot outlive
    // their code tracker or block table.
    void bind_runtime_block_table(RuntimeBlockTable* blocks) noexcept;
    void bind_runtime_code_invalidation(
        const std::shared_ptr<ExecutableCodeTracker>& tracker,
        const std::shared_ptr<RuntimeBlockTable>& blocks) noexcept;
    [[nodiscard]] const std::array<std::uint8_t, 32u>& queue(std::size_t index) const;
    [[nodiscard]] std::uint64_t transfer_count() const noexcept;
    [[nodiscard]] std::uint64_t rejected_transfer_count() const noexcept;
    [[nodiscard]] const std::optional<StoreQueueSinkFault>& last_sink_fault() const noexcept;
    [[nodiscard]] Sh4StoreQueueSnapshot snapshot() const;
    void validate_state_restore(const Sh4StoreQueueSnapshot& state) const;
    // Restores only guest-visible state and counters; host sinks, address
    // translators and code-invalidation ownership remain bound in place.
    void restore_state_passive(const Sh4StoreQueueSnapshot& state);
    void commit_validated_state_restore(
        Sh4StoreQueueSnapshot state) noexcept;
    void reset() noexcept;
    [[nodiscard]] CacheMaintenanceResult maintain(CacheMaintenanceOperation operation,
                                                  std::uint32_t address,
                                                  std::uint32_t movca_value = 0u);
    void set_operand_cache_ram_enabled(bool enabled);
    [[nodiscard]] bool operand_cache_ram_enabled() const noexcept;
    [[nodiscard]] std::uint32_t
    read_operand_cache_ram(std::uint32_t offset,
                           MemoryAccessWidth width = MemoryAccessWidth::Byte) const;
    void write_operand_cache_ram(std::uint32_t offset,
                                 std::uint32_t value,
                                 MemoryAccessWidth width = MemoryAccessWidth::Byte);

  private:
    [[nodiscard]] static std::size_t queue_index(std::uint32_t address) noexcept;
    [[nodiscard]] std::uint32_t transfer_target(std::uint32_t address,
                                                std::size_t queue) const noexcept;
    Memory& memory_;
    StoreQueueSink sink_;
    StoreQueueAddressTranslator address_translator_;
    ExecutableCodeTracker* code_tracker_;
    RuntimeBlockTable* runtime_blocks_ = nullptr;
    std::weak_ptr<ExecutableCodeTracker> shared_code_tracker_;
    std::weak_ptr<RuntimeBlockTable> shared_runtime_blocks_;
    bool shared_code_invalidation_bound_ = false;
    OperandCacheRamProfile ocram_profile_;
    std::array<std::array<std::uint8_t, 32u>, 2u> queues_{};
    std::array<std::uint32_t, 2u> qacr_{};
    std::array<std::uint8_t, 8192u> operand_cache_ram_{};
    bool operand_cache_ram_enabled_ = false;
    std::uint64_t transfer_count_ = 0u;
    std::uint64_t rejected_transfer_count_ = 0u;
    std::optional<StoreQueueSinkFault> last_sink_fault_;
};

} // namespace katana::runtime

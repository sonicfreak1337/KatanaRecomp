#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace katana::runtime {

struct CrashCapsule;
class LinearMemoryDevice;
class ExecutableDiscLoadTransactionCoordinator;

enum class MemoryRegionAccess { ReadOnly, ReadWrite };

enum class MemoryAccessWidth : std::uint8_t { Byte = 1u, Halfword = 2u, Word = 4u };

enum class MemoryAccessOperation { Read, Write };

enum class CodeWriteSource : std::uint8_t { Cpu, Fpu, Dma, StoreQueue, Copy, Fallback };

enum class GuestMemoryAccessOrigin : std::uint8_t { Memory, PvrRender, PvrYuv };

enum class MemoryAlignmentPolicy { Strict, Permissive };

enum class MemoryLookupMode { Indexed, Reference };

inline constexpr std::size_t guest_memory_access_change_tracking_limit =
    1024u * 1024u;

enum class MemoryAccessErrorReason {
    Misaligned,
    Unmapped,
    CrossRegion,
    ReadOnly,
    AddressOverflow,
    DeviceRejected,
    TlbMiss,
    TlbMultipleHit,
    InitialPageWrite,
    TlbProtection
};

struct GuestInstructionOrigin {
    std::uint32_t source_pc = 0u;
    std::uint32_t runtime_pc = 0u;
    bool valid = false;
};

struct GuestMemoryAccessContext {
    std::uint32_t virtual_address = 0u;
    GuestInstructionOrigin instruction;
    std::uint64_t retired_guest_instructions = 0u;
    GuestMemoryAccessOrigin access_origin = GuestMemoryAccessOrigin::Memory;
    std::uint64_t attempted_guest_instructions = 0u;
};

struct LinearMemoryProjection {
    const LinearMemoryDevice* backing = nullptr;
    std::array<std::uint32_t, 4u> byte_offsets{};
    std::uint8_t byte_count = 0u;
    bool contiguous = false;

    [[nodiscard]] explicit operator bool() const noexcept {
        return backing != nullptr && byte_count != 0u &&
               byte_count <= byte_offsets.size();
    }
};

struct PreparedDeviceU32Write {
    using CommitCallback = void (*)(void* context,
                                    const std::array<std::uint32_t, 4u>& byte_offsets,
                                    std::uint32_t value) noexcept;

    void* context = nullptr;
    std::array<std::uint32_t, 4u> byte_offsets{};
    CommitCallback commit_callback = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr && commit_callback != nullptr;
    }

    void commit(const std::uint32_t value) const noexcept {
        commit_callback(context, byte_offsets, value);
    }
};

class MemoryAccessError final : public std::runtime_error {
  public:
    MemoryAccessError(MemoryAccessErrorReason reason,
                      MemoryAccessOperation operation,
                      std::uint32_t address,
                      MemoryAccessWidth width,
                      std::string region_name = {});

    [[nodiscard]] MemoryAccessErrorReason reason() const noexcept;
    [[nodiscard]] MemoryAccessOperation operation() const noexcept;
    [[nodiscard]] std::uint32_t address() const noexcept;
    [[nodiscard]] MemoryAccessWidth width() const noexcept;
    [[nodiscard]] const std::string& region_name() const noexcept;

  private:
    MemoryAccessErrorReason reason_;
    MemoryAccessOperation operation_;
    std::uint32_t address_ = 0u;
    MemoryAccessWidth width_ = MemoryAccessWidth::Byte;
    std::string region_name_;
};

class MmioDeviceError final : public std::runtime_error {
  public:
    explicit MmioDeviceError(std::string message) : std::runtime_error(std::move(message)) {}
};

class MemoryDevice {
  public:
    virtual ~MemoryDevice() = default;

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual std::uint8_t read_u8(std::uint32_t offset) const = 0;
    [[nodiscard]] virtual std::uint16_t read_u16(std::uint32_t offset) const;
    [[nodiscard]] virtual std::uint32_t read_u32(std::uint32_t offset) const;
    virtual void write_u8(std::uint32_t offset, std::uint8_t value) = 0;
    virtual void write_u16(std::uint32_t offset, std::uint16_t value);
    virtual void write_u32(std::uint32_t offset, std::uint32_t value);
    [[nodiscard]] virtual LinearMemoryProjection
    linear_projection(std::uint32_t offset, MemoryAccessWidth width) const noexcept;
    [[nodiscard]] virtual PreparedDeviceU32Write
    prepare_prevalidated_u32_write(std::uint32_t offset) noexcept;
};

class LinearMemoryDevice final : public MemoryDevice {
  public:
    explicit LinearMemoryDevice(std::size_t size);

    [[nodiscard]] std::size_t size() const noexcept override;
    [[nodiscard]] std::uint8_t read_u8(std::uint32_t offset) const override;
    [[nodiscard]] std::uint16_t read_u16(std::uint32_t offset) const override;
    [[nodiscard]] std::uint32_t read_u32(std::uint32_t offset) const override;
    void write_u8(std::uint32_t offset, std::uint8_t value) override;
    void write_u16(std::uint32_t offset, std::uint16_t value) override;
    void write_u32(std::uint32_t offset, std::uint32_t value) override;
    [[nodiscard]] LinearMemoryProjection
    linear_projection(std::uint32_t offset, MemoryAccessWidth width) const noexcept override;
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;
    [[nodiscard]] std::span<std::uint8_t> writable_bytes() noexcept;

  private:
    void check(std::uint32_t offset) const;

    std::vector<std::uint8_t> bytes_;
};

using MmioReadHandler = std::function<std::uint32_t(std::uint32_t offset, MemoryAccessWidth width)>;
using MmioWriteHandler =
    std::function<void(std::uint32_t offset, std::uint32_t value, MemoryAccessWidth width)>;
using MmioPrepareU32WriteHandler =
    std::function<PreparedDeviceU32Write(std::uint32_t offset)>;

class MmioMemoryDevice final : public MemoryDevice {
  public:
    MmioMemoryDevice(std::size_t size,
                     MmioReadHandler read_handler,
                     MmioWriteHandler write_handler,
                     MmioPrepareU32WriteHandler prepare_u32_write_handler = {});

    [[nodiscard]] std::size_t size() const noexcept override;
    [[nodiscard]] std::uint8_t read_u8(std::uint32_t offset) const override;
    [[nodiscard]] std::uint16_t read_u16(std::uint32_t offset) const override;
    [[nodiscard]] std::uint32_t read_u32(std::uint32_t offset) const override;
    void write_u8(std::uint32_t offset, std::uint8_t value) override;
    void write_u16(std::uint32_t offset, std::uint16_t value) override;
    void write_u32(std::uint32_t offset, std::uint32_t value) override;
    [[nodiscard]] PreparedDeviceU32Write
    prepare_prevalidated_u32_write(std::uint32_t offset) noexcept override;

  private:
    void check(std::uint32_t offset, MemoryAccessWidth width) const;
    [[nodiscard]] std::uint32_t read(std::uint32_t offset, MemoryAccessWidth width) const;
    void write(std::uint32_t offset, std::uint32_t value, MemoryAccessWidth width);

    std::size_t size_ = 0u;
    MmioReadHandler read_handler_;
    MmioWriteHandler write_handler_;
    MmioPrepareU32WriteHandler prepare_u32_write_handler_;
};

struct MemoryRegionInfo {
    std::string name;
    std::uint32_t base_address = 0u;
    std::size_t size = 0u;
    MemoryRegionAccess access = MemoryRegionAccess::ReadWrite;
};

enum class MemoryWatchpointAccess { Read, Write, ReadWrite };

struct MemoryAccessEvent {
    MemoryAccessOperation operation = MemoryAccessOperation::Read;
    std::uint32_t address = 0u;
    MemoryAccessWidth width = MemoryAccessWidth::Byte;
    std::uint32_t value = 0u;
    std::string region_name;
};

using MemoryAccessObserver = std::function<void(const MemoryAccessEvent&)>;
using MemoryWatchpointId = std::uint64_t;

struct GuestWriteEvent {
    std::uint32_t address = 0u;
    std::size_t size = 0u;
    CodeWriteSource source = CodeWriteSource::Cpu;
    bool bytes_changed = true;
};

using GuestWriteObserver = std::function<void(const GuestWriteEvent&)>;

enum class GuestWriteObserverContract : std::uint8_t {
    General,
    // The observer may decide only from the event and update its own code/module
    // bookkeeping. It must be nonthrowing; it must neither inspect nor mutate CPU,
    // scheduler, memory metrics, mappings, backing bytes, or diagnostic/observer state.
    StableForPrevalidatedLinearWrites,
};

struct GuestMemoryAccessEvent {
    MemoryAccessOperation operation = MemoryAccessOperation::Read;
    GuestMemoryAccessOrigin access_origin = GuestMemoryAccessOrigin::Memory;
    GuestInstructionOrigin instruction;
    std::uint32_t virtual_address = 0u;
    std::uint32_t physical_address = 0u;
    MemoryAccessWidth width = MemoryAccessWidth::Byte;
    std::uint32_t value = 0u;
    std::size_t size = 0u;
    CodeWriteSource write_source = CodeWriteSource::Cpu;
    bool scalar_value_valid = false;
    bool bytes_changed = true;
    std::uint64_t retired_guest_instructions = 0u;
    std::uint64_t attempted_guest_instructions = 0u;
    const LinearMemoryDevice* linear_backing = nullptr;
    std::uint32_t linear_offset = 0u;
    std::size_t linear_size = 0u;
    bool linear_contiguous = false;
    std::array<std::uint32_t, 4u> linear_byte_offsets{};
    std::uint8_t linear_byte_count = 0u;
};

using GuestMemoryAccessCallback =
    void (*)(void* context, const GuestMemoryAccessEvent& event) noexcept;

struct GuestMemoryAccessSink {
    void* context = nullptr;
    GuestMemoryAccessCallback callback = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept { return callback != nullptr; }
};

using MmioInterruptStateDirtyCallback = void (*)(void* context) noexcept;

struct MmioInterruptStateSink {
    void* context = nullptr;
    MmioInterruptStateDirtyCallback mark_dirty = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept { return mark_dirty != nullptr; }
};

struct MemoryPerformanceCounters {
    std::uint64_t indexed_region_hits = 0u;
    std::uint64_t reference_region_probes = 0u;
    std::uint64_t unobserved_accesses = 0u;
    std::uint64_t observed_accesses = 0u;
};

// A short-lived proof that one complete physical alias window still has a stable,
// directly addressable linear backing. Generated code may acquire this once at a
// native function or block boundary and reuse it until the next architectural or
// host-service boundary. A null byte pointer means that the corresponding access
// kind must use the general Memory path.
//
// The guard never owns the backing and must not outlive its Memory object.
struct DirectLinearMemoryGuard {
    const std::uint8_t* read_bytes = nullptr;
    std::uint8_t* write_bytes = nullptr;
    std::uint32_t physical_base = 0u;
    std::uint32_t physical_span = 0u;
    std::uint32_t backing_mask = 0u;
    std::uint64_t generation = 0u;

    [[nodiscard]] explicit operator bool() const noexcept {
        return read_bytes != nullptr && physical_span != 0u;
    }
};

[[nodiscard]] inline bool direct_linear_guard_offset(
    const DirectLinearMemoryGuard& guard,
    const std::uint32_t virtual_address,
    const std::size_t width,
    std::uint32_t& offset) noexcept {
    if (!guard || width == 0u ||
        (virtual_address & 0xC0000000u) != 0x80000000u ||
        (virtual_address & static_cast<std::uint32_t>(width - 1u)) != 0u)
        return false;
    const auto physical_address = virtual_address & 0x1FFFFFFFu;
    if (physical_address < guard.physical_base) return false;
    const auto relative = physical_address - guard.physical_base;
    if (relative >= guard.physical_span || width > guard.physical_span - relative)
        return false;
    offset = relative & guard.backing_mask;
    return width <= static_cast<std::size_t>(guard.backing_mask) + 1u - offset;
}

[[nodiscard]] inline bool direct_linear_guard_read_u8(
    const DirectLinearMemoryGuard& guard,
    const std::uint32_t virtual_address,
    std::uint8_t& value) noexcept {
    std::uint32_t offset = 0u;
    if (!direct_linear_guard_offset(guard, virtual_address, sizeof(value), offset))
        return false;
    value = guard.read_bytes[offset];
    return true;
}

[[nodiscard]] inline bool direct_linear_guard_read_u16(
    const DirectLinearMemoryGuard& guard,
    const std::uint32_t virtual_address,
    std::uint16_t& value) noexcept {
    std::uint32_t offset = 0u;
    if (!direct_linear_guard_offset(guard, virtual_address, sizeof(value), offset))
        return false;
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(&value, guard.read_bytes + offset, sizeof(value));
    } else {
        value = static_cast<std::uint16_t>(guard.read_bytes[offset]) |
                static_cast<std::uint16_t>(guard.read_bytes[offset + 1u]) << 8u;
    }
    return true;
}

[[nodiscard]] inline bool direct_linear_guard_read_u32(
    const DirectLinearMemoryGuard& guard,
    const std::uint32_t virtual_address,
    std::uint32_t& value) noexcept {
    std::uint32_t offset = 0u;
    if (!direct_linear_guard_offset(guard, virtual_address, sizeof(value), offset))
        return false;
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(&value, guard.read_bytes + offset, sizeof(value));
    } else {
        value = static_cast<std::uint32_t>(guard.read_bytes[offset]) |
                static_cast<std::uint32_t>(guard.read_bytes[offset + 1u]) << 8u |
                static_cast<std::uint32_t>(guard.read_bytes[offset + 2u]) << 16u |
                static_cast<std::uint32_t>(guard.read_bytes[offset + 3u]) << 24u;
    }
    return true;
}

struct LinearMemoryTransactionWrite {
    std::uint32_t address = 0u;
    std::span<const std::uint8_t> bytes;
};

class Memory {
  public:
    class PreparedLinearTransactionBatch final {
      public:
        PreparedLinearTransactionBatch(
            const PreparedLinearTransactionBatch&) = delete;
        PreparedLinearTransactionBatch& operator=(
            const PreparedLinearTransactionBatch&) = delete;
        PreparedLinearTransactionBatch(
            PreparedLinearTransactionBatch&&) noexcept;
        PreparedLinearTransactionBatch& operator=(
            PreparedLinearTransactionBatch&&) noexcept;
        ~PreparedLinearTransactionBatch();

        // The events exactly mirror the stable observer notifications that
        // commit would emit after making every byte visible.
        [[nodiscard]] std::span<const GuestWriteEvent>
        guest_write_events() const noexcept;
        // Transfers responsibility for those notifications to an already
        // admitted external, allocation-free commit plan. This must only be
        // called after that plan has been built successfully.
        void suppress_guest_write_observer() noexcept;

      private:
        friend class Memory;
        struct Data;

        PreparedLinearTransactionBatch();
        std::unique_ptr<Data> data_;
    };

    class PreparedLinearFill final {
      public:
        PreparedLinearFill(const PreparedLinearFill&) = delete;
        PreparedLinearFill& operator=(const PreparedLinearFill&) = delete;
        PreparedLinearFill(PreparedLinearFill&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              device_lifetime_(std::move(other.device_lifetime_)),
              linear_(std::exchange(other.linear_, nullptr)),
              offset_(std::exchange(other.offset_, 0u)),
              address_(std::exchange(other.address_, 0u)),
              size_(std::exchange(other.size_, 0u)),
              value_(std::exchange(other.value_, std::uint8_t{0u})),
              source_(other.source_),
              additional_unobserved_accesses_(
                  std::exchange(other.additional_unobserved_accesses_, 0u)),
              additional_indexed_region_hits_(
                  std::exchange(other.additional_indexed_region_hits_, 0u)),
              observer_(std::move(other.observer_)),
              changed_bytes_(std::move(other.changed_bytes_)) {}
        PreparedLinearFill& operator=(PreparedLinearFill&&) noexcept = delete;
        ~PreparedLinearFill() = default;

      private:
        friend class Memory;
        PreparedLinearFill() = default;

        Memory* owner_ = nullptr;
        std::shared_ptr<MemoryDevice> device_lifetime_;
        LinearMemoryDevice* linear_ = nullptr;
        std::size_t offset_ = 0u;
        std::uint32_t address_ = 0u;
        std::size_t size_ = 0u;
        std::uint8_t value_ = 0u;
        CodeWriteSource source_ = CodeWriteSource::Copy;
        std::size_t additional_unobserved_accesses_ = 0u;
        std::size_t additional_indexed_region_hits_ = 0u;
        GuestWriteObserver observer_;
        std::vector<std::uint8_t> changed_bytes_;
    };

    class PreparedLinearU32Pattern final {
      public:
        PreparedLinearU32Pattern(const PreparedLinearU32Pattern&) = delete;
        PreparedLinearU32Pattern& operator=(const PreparedLinearU32Pattern&) = delete;
        PreparedLinearU32Pattern(PreparedLinearU32Pattern&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              device_lifetime_(std::move(other.device_lifetime_)),
              linear_(std::exchange(other.linear_, nullptr)),
              offset_(std::exchange(other.offset_, 0u)),
              address_(std::exchange(other.address_, 0u)),
              word_count_(std::exchange(other.word_count_, 0u)),
              value_(std::exchange(other.value_, 0u)),
              source_(other.source_),
              additional_unobserved_accesses_(
                  std::exchange(other.additional_unobserved_accesses_, 0u)),
              additional_indexed_region_hits_(
                  std::exchange(other.additional_indexed_region_hits_, 0u)),
              observer_(std::move(other.observer_)),
              changed_words_(std::move(other.changed_words_)) {}
        PreparedLinearU32Pattern&
        operator=(PreparedLinearU32Pattern&&) noexcept = delete;
        ~PreparedLinearU32Pattern() = default;

      private:
        friend class Memory;
        PreparedLinearU32Pattern() = default;

        Memory* owner_ = nullptr;
        std::shared_ptr<MemoryDevice> device_lifetime_;
        LinearMemoryDevice* linear_ = nullptr;
        std::size_t offset_ = 0u;
        std::uint32_t address_ = 0u;
        std::size_t word_count_ = 0u;
        std::uint32_t value_ = 0u;
        CodeWriteSource source_ = CodeWriteSource::Copy;
        std::size_t additional_unobserved_accesses_ = 0u;
        std::size_t additional_indexed_region_hits_ = 0u;
        GuestWriteObserver observer_;
        std::vector<std::uint8_t> changed_words_;
    };

    class PreparedRepeatedU32Sequence final {
      public:
        PreparedRepeatedU32Sequence(const PreparedRepeatedU32Sequence&) = delete;
        PreparedRepeatedU32Sequence& operator=(const PreparedRepeatedU32Sequence&) = delete;
        PreparedRepeatedU32Sequence(PreparedRepeatedU32Sequence&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              device_lifetime_(std::move(other.device_lifetime_)),
              linear_(std::exchange(other.linear_, nullptr)),
              offset_(std::exchange(other.offset_, 0u)),
              address_(std::exchange(other.address_, 0u)),
              word_count_(std::exchange(other.word_count_, 0u)),
              final_value_(std::exchange(other.final_value_, 0u)),
              source_(other.source_),
              additional_unobserved_accesses_(
                  std::exchange(other.additional_unobserved_accesses_, 0u)),
              additional_indexed_region_hits_(
                  std::exchange(other.additional_indexed_region_hits_, 0u)),
              observer_(std::move(other.observer_)),
              changed_words_(std::move(other.changed_words_)),
              device_write_(std::exchange(other.device_write_, {})) {}
        PreparedRepeatedU32Sequence&
        operator=(PreparedRepeatedU32Sequence&&) noexcept = delete;
        ~PreparedRepeatedU32Sequence() = default;

      private:
        friend class Memory;
        PreparedRepeatedU32Sequence() = default;

        Memory* owner_ = nullptr;
        std::shared_ptr<MemoryDevice> device_lifetime_;
        LinearMemoryDevice* linear_ = nullptr;
        std::size_t offset_ = 0u;
        std::uint32_t address_ = 0u;
        std::size_t word_count_ = 0u;
        std::uint32_t final_value_ = 0u;
        CodeWriteSource source_ = CodeWriteSource::Copy;
        std::size_t additional_unobserved_accesses_ = 0u;
        std::size_t additional_indexed_region_hits_ = 0u;
        GuestWriteObserver observer_;
        std::vector<std::uint8_t> changed_words_;
        PreparedDeviceU32Write device_write_;
    };

    explicit Memory(std::size_t legacy_size = 1024u * 1024u,
                    MemoryAlignmentPolicy alignment_policy = MemoryAlignmentPolicy::Strict);

    void map_region(std::string name,
                    std::uint32_t base_address,
                    std::shared_ptr<MemoryDevice> device,
                    MemoryRegionAccess access = MemoryRegionAccess::ReadWrite);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t region_count() const noexcept;
    [[nodiscard]] const MemoryRegionInfo& region(std::size_t index) const;
    [[nodiscard]] bool contains(std::uint32_t address, std::size_t width = 1u) const noexcept;
    [[nodiscard]] bool maps_device(std::uint32_t address,
                                   std::size_t width,
                                   const MemoryDevice* device,
                                   bool record_lookup_metrics = true) const noexcept;
    [[nodiscard]] bool is_readable_linear_range(std::uint32_t address,
                                                std::size_t width,
                                                bool record_lookup_metrics = true) const noexcept;
    [[nodiscard]] bool is_writable_linear_range(std::uint32_t address,
                                                 std::size_t width,
                                                 bool record_lookup_metrics = true) const noexcept;

    [[nodiscard]] MemoryAlignmentPolicy alignment_policy() const noexcept;
    void set_alignment_policy(MemoryAlignmentPolicy policy) noexcept;
    [[nodiscard]] MemoryLookupMode lookup_mode() const noexcept;
    void set_lookup_mode(MemoryLookupMode mode) noexcept;
    [[nodiscard]] const MemoryPerformanceCounters& performance_counters() const noexcept;
    void reset_performance_counters() const noexcept;
    // Accounts accesses whose complete Indexed lookup and observer behavior was proven by the
    // caller before an equivalent batched operation. A false result leaves every counter
    // unchanged. Counter overflow intentionally follows the scalar uint64_t wraparound behavior.
    [[nodiscard]] bool account_prevalidated_unobserved_accesses(
        std::uint64_t accesses,
        std::uint64_t indexed_region_hits) const noexcept;

    // Binds a power-of-two linear backing repeated over one already mapped physical
    // alias window. This is deliberately a single product-RAM contract: VRAM,
    // AICA RAM, OCRAM and MMIO retain their distinct device semantics.
    void bind_direct_linear_alias_window(std::uint32_t physical_base,
                                         std::uint32_t physical_span,
                                         LinearMemoryDevice& backing);
    void clear_direct_linear_alias_window() noexcept;
    [[nodiscard]] DirectLinearMemoryGuard
    direct_linear_memory_guard(bool write) const noexcept;
    [[nodiscard]] bool
    direct_linear_memory_guard_current(const DirectLinearMemoryGuard& guard,
                                       bool write) const noexcept;
    [[nodiscard]] bool
    try_read_direct_linear_u8(std::uint32_t physical_address,
                              std::uint8_t& value) const noexcept;
    [[nodiscard]] bool
    try_read_direct_linear_u16(std::uint32_t physical_address,
                               std::uint16_t& value) const noexcept;
    [[nodiscard]] bool
    try_read_direct_linear_u32(std::uint32_t physical_address,
                               std::uint32_t& value) const noexcept;
    [[nodiscard]] bool
    try_write_direct_linear_u8(std::uint32_t physical_address,
                               std::uint8_t value,
                               CodeWriteSource source = CodeWriteSource::Cpu);
    [[nodiscard]] bool
    try_write_direct_linear_u16(std::uint32_t physical_address,
                                std::uint16_t value,
                                CodeWriteSource source = CodeWriteSource::Cpu);
    [[nodiscard]] bool
    try_write_direct_linear_u32(std::uint32_t physical_address,
                                std::uint32_t value,
                                CodeWriteSource source = CodeWriteSource::Cpu);

    [[nodiscard]] MemoryWatchpointId add_watchpoint(std::uint32_t address,
                                                    std::size_t size,
                                                    MemoryWatchpointAccess access,
                                                    MemoryAccessObserver observer);
    [[nodiscard]] bool remove_watchpoint(MemoryWatchpointId id);
    void clear_watchpoints() noexcept;
    [[nodiscard]] std::size_t watchpoint_count() const noexcept;

    void set_trace_handler(MemoryAccessObserver observer);
    void clear_trace_handler() noexcept;
    [[nodiscard]] bool has_trace_handler() const noexcept;

    void set_mmio_access_tracking(bool enabled) noexcept;
    [[nodiscard]] bool mmio_access_tracking_enabled() const noexcept;
    [[nodiscard]] std::optional<MemoryAccessEvent> last_mmio_access() const;
    void clear_last_mmio_access() const noexcept;
    void attach_crash_capsule(CrashCapsule& capsule) noexcept;
    void detach_crash_capsule(const CrashCapsule& capsule) noexcept;
    // Compatibility token for generated runtimes. Interrupt delivery is driven by
    // PlatformInterruptRouter::source_epoch(); ordinary MMIO traffic must not advance this
    // value and force a complete interrupt-router walk.
    [[nodiscard]] std::uint64_t mmio_access_epoch() const noexcept;
    void set_mmio_interrupt_state_sink(MmioInterruptStateSink sink) noexcept;
    void clear_mmio_interrupt_state_sink() noexcept;
    // Direct device paths which can change a router-polled source outside MMIO or a scheduler
    // callback must call this after committing the state change.
    void notify_interrupt_source_state_maybe_changed() const noexcept;
    void set_mmio_trace_handler(MemoryAccessObserver observer);
    void clear_mmio_trace_handler() noexcept;
    [[nodiscard]] bool has_mmio_trace_handler() const noexcept;

    void set_guest_write_observer(
        GuestWriteObserver observer,
        GuestWriteObserverContract contract = GuestWriteObserverContract::General);
    void clear_guest_write_observer() noexcept;
    [[nodiscard]] bool has_guest_write_observer() const noexcept;
    [[nodiscard]] bool
    guest_write_observer_allows_prevalidated_linear_writes() const noexcept;

    void set_guest_memory_access_sink(GuestMemoryAccessSink sink) noexcept;
    void clear_guest_memory_access_sink() noexcept;
    [[nodiscard]] bool has_guest_memory_access_sink() const noexcept {
        return static_cast<bool>(guest_memory_access_sink_);
    }
    [[nodiscard]] GuestMemoryAccessSink guest_memory_access_sink() const noexcept;
    void notify_external_guest_memory_access(const GuestMemoryAccessEvent& event) const noexcept;

    [[nodiscard]] std::uint8_t read_u8(std::uint32_t address) const;
    [[nodiscard]] std::uint16_t read_u16(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t read_u32(std::uint32_t address) const;
    [[nodiscard]] std::uint8_t
    read_u8_at(std::uint32_t address, const GuestMemoryAccessContext& context) const;
    [[nodiscard]] std::uint16_t
    read_u16_at(std::uint32_t address, const GuestMemoryAccessContext& context) const;
    [[nodiscard]] std::uint32_t
    read_u32_at(std::uint32_t address, const GuestMemoryAccessContext& context) const;
    [[nodiscard]] std::uint32_t
    peek_u32(std::uint32_t address,
             std::span<const MemoryDevice* const> permitted_devices) const;
    [[nodiscard]] std::uint32_t read_s8(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t read_s16(std::uint32_t address) const;

    void write_u8(std::uint32_t address,
                  std::uint8_t value,
                  CodeWriteSource source = CodeWriteSource::Cpu);
    void write_u16(std::uint32_t address,
                   std::uint16_t value,
                   CodeWriteSource source = CodeWriteSource::Cpu);
    void write_u32(std::uint32_t address,
                   std::uint32_t value,
                   CodeWriteSource source = CodeWriteSource::Cpu);
    void write_u8_at(std::uint32_t address,
                     std::uint8_t value,
                     const GuestMemoryAccessContext& context,
                     CodeWriteSource source = CodeWriteSource::Cpu);
    void write_u16_at(std::uint32_t address,
                      std::uint16_t value,
                      const GuestMemoryAccessContext& context,
                      CodeWriteSource source = CodeWriteSource::Cpu);
    void write_u32_at(std::uint32_t address,
                      std::uint32_t value,
                      const GuestMemoryAccessContext& context,
                      CodeWriteSource source = CodeWriteSource::Cpu);
    void write_bytes(std::uint32_t address,
                     std::span<const std::uint8_t> bytes,
                     CodeWriteSource source = CodeWriteSource::Copy);
    [[nodiscard]] bool commit_linear_transaction_bytes(
        std::uint32_t address,
        std::span<const std::uint8_t> bytes,
        CodeWriteSource source = CodeWriteSource::Copy) noexcept;
    [[nodiscard]] bool commit_linear_transaction_batch(
        std::span<const LinearMemoryTransactionWrite> writes,
        CodeWriteSource source = CodeWriteSource::Copy) noexcept;
    // Resolves, copies and admits the complete batch without changing guest
    // memory. The returned move-only plan owns every source byte and backing
    // lifetime needed by commit. No guest execution, mapping change or host
    // observer mutation may occur between prepare and commit.
    [[nodiscard]] std::optional<PreparedLinearTransactionBatch>
    prepare_linear_transaction_batch(
        std::span<const LinearMemoryTransactionWrite> writes,
        CodeWriteSource source = CodeWriteSource::Copy) noexcept;
    // Allocation-free and non-rejecting. All backing bytes become visible
    // before best-effort diagnostics and invalidation callbacks are emitted.
    void commit_prepared_linear_transaction_batch(
        PreparedLinearTransactionBatch prepared) noexcept;
    // The two-phase forms below must be used whenever guest time is accepted between admission
    // and the RAM write. Prepare resolves and retains the linear backing, snapshots the stable
    // observer, computes every changed flag, and performs every allocation. A missing plan leaves
    // memory and observer-visible state unchanged. Committing a valid move-only plan is
    // allocation-free, performs no validation, cannot be rejected, and is noexcept. The backing
    // and owning Memory must remain live until commit; callers must also prove that no host action
    // changes the target bytes between prepare and commit.
    [[nodiscard]] std::optional<PreparedLinearFill>
    prepare_prevalidated_linear_fill(
        std::uint32_t address,
        std::size_t size,
        std::uint8_t value,
        CodeWriteSource source = CodeWriteSource::Copy,
        std::size_t additional_unobserved_accesses = 0u,
        std::size_t additional_indexed_region_hits = 0u) noexcept;
    void commit_prepared_linear_fill(PreparedLinearFill prepared) noexcept;

    // Compatibility wrapper for callers that prepare and commit without accepting guest time
    // between the two operations.
    [[nodiscard]] bool
    commit_prevalidated_linear_fill(std::uint32_t address,
                                    std::size_t size,
                                    std::uint8_t value,
                                    CodeWriteSource source = CodeWriteSource::Copy,
                                    std::size_t additional_unobserved_accesses = 0u,
                                    std::size_t additional_indexed_region_hits = 0u) noexcept;
    [[nodiscard]] std::optional<PreparedLinearU32Pattern>
    prepare_prevalidated_linear_u32_pattern(
        std::uint32_t address,
        std::size_t word_count,
        std::uint32_t value,
        CodeWriteSource source = CodeWriteSource::Copy,
        std::size_t additional_unobserved_accesses = 0u,
        std::size_t additional_indexed_region_hits = 0u) noexcept;
    void
    commit_prepared_linear_u32_pattern(PreparedLinearU32Pattern prepared) noexcept;

    // Compatibility wrapper for an immediate prepare/commit. Stable observers receive one
    // word-sized event per guest store, including its exact bytes_changed state.
    [[nodiscard]] bool
    commit_prevalidated_linear_u32_pattern(
        std::uint32_t address,
        std::size_t word_count,
        std::uint32_t value,
        CodeWriteSource source = CodeWriteSource::Copy,
        std::size_t additional_unobserved_accesses = 0u,
        std::size_t additional_indexed_region_hits = 0u) noexcept;
    [[nodiscard]] std::optional<PreparedRepeatedU32Sequence>
    prepare_prevalidated_repeated_u32_sequence(
        std::uint32_t address,
        std::size_t word_count,
        std::uint32_t first_value,
        std::uint32_t step,
        CodeWriteSource source = CodeWriteSource::Copy,
        std::size_t additional_unobserved_accesses = 0u,
        std::size_t additional_indexed_region_hits = 0u) noexcept;
    void commit_prepared_repeated_u32_sequence(
        PreparedRepeatedU32Sequence prepared) noexcept;

    // Compatibility wrapper for an immediate prepare/commit. The first store writes
    // `first_value`; every later value advances by `step` with 32-bit wraparound.
    [[nodiscard]] bool
    commit_prevalidated_repeated_u32_sequence(
        std::uint32_t address,
        std::size_t word_count,
        std::uint32_t first_value,
        std::uint32_t step,
        CodeWriteSource source = CodeWriteSource::Copy,
        std::size_t additional_unobserved_accesses = 0u,
        std::size_t additional_indexed_region_hits = 0u) noexcept;
    void write_bytes_at(std::uint32_t address,
                        std::span<const std::uint8_t> bytes,
                        const GuestMemoryAccessContext& context,
                        CodeWriteSource source = CodeWriteSource::Copy);
    void copy_bytes(std::uint32_t destination,
                    std::uint32_t source_address,
                    std::size_t size,
                    CodeWriteSource source = CodeWriteSource::Dma);

  private:
    friend class ExecutableDiscLoadTransactionCoordinator;
    [[nodiscard]] bool commit_prevalidated_linear_transaction_bytes(
        std::uint32_t address,
        std::span<const std::uint8_t> bytes,
        std::span<const std::uint8_t> changed_bytes,
        CodeWriteSource source) noexcept;

    struct MappedRegion {
        MemoryRegionInfo info;
        std::shared_ptr<MemoryDevice> device;
        LinearMemoryDevice* linear = nullptr;
        bool mmio = false;
    };

    struct Watchpoint {
        MemoryWatchpointId id = 0u;
        std::uint32_t address = 0u;
        std::size_t size = 0u;
        MemoryWatchpointAccess access = MemoryWatchpointAccess::ReadWrite;
        MemoryAccessObserver observer;
    };

    struct LastMmioAccessRecord {
        MemoryAccessOperation operation = MemoryAccessOperation::Read;
        std::uint32_t address = 0u;
        MemoryAccessWidth width = MemoryAccessWidth::Byte;
        std::uint32_t value = 0u;
        std::uint32_t region_base_address = 0u;
    };

    [[nodiscard]] const MappedRegion&
    resolve(std::uint32_t address,
            MemoryAccessWidth width,
            MemoryAccessOperation operation,
            bool record_lookup_metrics = true) const;
    [[nodiscard]] const MappedRegion& resolve_writable(std::uint32_t address,
                                                       MemoryAccessWidth width) const;
    [[nodiscard]] const MappedRegion* indexed_region(std::uint32_t address,
                                                     std::size_t width,
                                                     bool record_lookup_metrics = true) const noexcept;
    [[nodiscard]] const MappedRegion*
    prevalidated_writable_region(std::uint32_t address,
                                 std::size_t size) const noexcept;
    [[nodiscard]] const MappedRegion*
    prevalidated_writable_linear_region(std::uint32_t address,
                                        std::size_t size) const noexcept;
    void rebuild_region_index();
    [[nodiscard]] bool access_observers_active() const noexcept;
    void refresh_direct_linear_access_state() noexcept;
    [[nodiscard]] bool direct_linear_offset(std::uint32_t physical_address,
                                            std::size_t width,
                                            std::uint32_t& offset) const noexcept;
    void require_alignment(std::uint32_t address,
                           MemoryAccessWidth width,
                           MemoryAccessOperation operation) const;
    void notify_access(const MemoryAccessEvent& event) const;
    void record_mmio_access(const MappedRegion& mapped,
                            MemoryAccessOperation operation,
                            std::uint32_t address,
                            MemoryAccessWidth width,
                            std::uint32_t value) const noexcept;
    void notify_guest_write(const GuestWriteEvent& event) const;
    void notify_guest_memory_access(const MappedRegion& mapped,
                                    MemoryAccessOperation operation,
                                    std::uint32_t physical_address,
                                    std::uint32_t value,
                                    MemoryAccessWidth width,
                                    std::size_t size,
                                    CodeWriteSource source,
                                    bool scalar_value_valid,
                                    bool bytes_changed,
                                    const GuestMemoryAccessContext* context = nullptr) const noexcept;
    void notify_guest_memory_write_range(std::uint32_t address,
                                         std::size_t size,
                                         CodeWriteSource source,
                                         std::span<const std::uint8_t> changed_bytes,
                                         const GuestMemoryAccessContext* context = nullptr) const
        noexcept;
    void notify_guest_memory_access_loss(
        const GuestMemoryAccessContext* context = nullptr) const noexcept;

    MemoryAlignmentPolicy alignment_policy_ = MemoryAlignmentPolicy::Strict;
    MemoryLookupMode lookup_mode_ = MemoryLookupMode::Indexed;
    std::vector<MappedRegion> regions_;
    std::vector<std::int32_t> region_page_index_;
    std::vector<Watchpoint> watchpoints_;
    MemoryAccessObserver trace_handler_;
    MemoryAccessObserver mmio_trace_handler_;
    GuestWriteObserver guest_write_observer_;
    GuestWriteObserverContract guest_write_observer_contract_ =
        GuestWriteObserverContract::General;
    std::uint64_t guest_write_observer_generation_ = 1u;
    GuestMemoryAccessSink guest_memory_access_sink_;
    MmioInterruptStateSink mmio_interrupt_state_sink_;
    CrashCapsule* crash_capsule_ = nullptr;
    bool mmio_access_tracking_enabled_ = false;
    mutable std::optional<LastMmioAccessRecord> last_mmio_access_;
    MemoryWatchpointId next_watchpoint_id_ = 1u;
    mutable MemoryPerformanceCounters performance_counters_;
    LinearMemoryDevice* direct_linear_backing_ = nullptr;
    std::uint8_t* direct_linear_bytes_ = nullptr;
    std::uint32_t direct_linear_physical_base_ = 0u;
    std::uint32_t direct_linear_physical_span_ = 0u;
    std::uint32_t direct_linear_backing_mask_ = 0u;
    std::uint64_t direct_linear_generation_ = 1u;
    bool direct_linear_reads_enabled_ = false;
    bool direct_linear_writes_enabled_ = false;
};

} // namespace katana::runtime

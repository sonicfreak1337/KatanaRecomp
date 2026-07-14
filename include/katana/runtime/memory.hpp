#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace katana::runtime {

enum class MemoryRegionAccess {
    ReadOnly,
    ReadWrite
};

enum class MemoryAccessWidth : std::uint8_t {
    Byte = 1u,
    Halfword = 2u,
    Word = 4u
};

enum class MemoryAccessOperation {
    Read,
    Write
};

enum class MemoryAlignmentPolicy {
    Strict,
    Permissive
};

enum class MemoryAccessErrorReason {
    Misaligned,
    Unmapped,
    CrossRegion,
    ReadOnly,
    AddressOverflow
};

class MemoryAccessError final : public std::runtime_error {
public:
    MemoryAccessError(
        MemoryAccessErrorReason reason,
        MemoryAccessOperation operation,
        std::uint32_t address,
        MemoryAccessWidth width,
        std::string region_name = {}
    );

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

class MemoryDevice {
public:
    virtual ~MemoryDevice() = default;

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual std::uint8_t read_u8(
        std::uint32_t offset
    ) const = 0;
    [[nodiscard]] virtual std::uint16_t read_u16(
        std::uint32_t offset
    ) const;
    [[nodiscard]] virtual std::uint32_t read_u32(
        std::uint32_t offset
    ) const;
    virtual void write_u8(
        std::uint32_t offset,
        std::uint8_t value
    ) = 0;
    virtual void write_u16(
        std::uint32_t offset,
        std::uint16_t value
    );
    virtual void write_u32(
        std::uint32_t offset,
        std::uint32_t value
    );
};

class LinearMemoryDevice final : public MemoryDevice {
public:
    explicit LinearMemoryDevice(std::size_t size);

    [[nodiscard]] std::size_t size() const noexcept override;
    [[nodiscard]] std::uint8_t read_u8(
        std::uint32_t offset
    ) const override;
    void write_u8(
        std::uint32_t offset,
        std::uint8_t value
    ) override;

private:
    void check(std::uint32_t offset) const;

    std::vector<std::uint8_t> bytes_;
};

using MmioReadHandler = std::function<std::uint32_t(
    std::uint32_t offset,
    MemoryAccessWidth width
)>;
using MmioWriteHandler = std::function<void(
    std::uint32_t offset,
    std::uint32_t value,
    MemoryAccessWidth width
)>;

class MmioMemoryDevice final : public MemoryDevice {
public:
    MmioMemoryDevice(
        std::size_t size,
        MmioReadHandler read_handler,
        MmioWriteHandler write_handler
    );

    [[nodiscard]] std::size_t size() const noexcept override;
    [[nodiscard]] std::uint8_t read_u8(
        std::uint32_t offset
    ) const override;
    [[nodiscard]] std::uint16_t read_u16(
        std::uint32_t offset
    ) const override;
    [[nodiscard]] std::uint32_t read_u32(
        std::uint32_t offset
    ) const override;
    void write_u8(
        std::uint32_t offset,
        std::uint8_t value
    ) override;
    void write_u16(
        std::uint32_t offset,
        std::uint16_t value
    ) override;
    void write_u32(
        std::uint32_t offset,
        std::uint32_t value
    ) override;

private:
    void check(
        std::uint32_t offset,
        MemoryAccessWidth width
    ) const;
    [[nodiscard]] std::uint32_t read(
        std::uint32_t offset,
        MemoryAccessWidth width
    ) const;
    void write(
        std::uint32_t offset,
        std::uint32_t value,
        MemoryAccessWidth width
    );

    std::size_t size_ = 0u;
    MmioReadHandler read_handler_;
    MmioWriteHandler write_handler_;
};

struct MemoryRegionInfo {
    std::string name;
    std::uint32_t base_address = 0u;
    std::size_t size = 0u;
    MemoryRegionAccess access = MemoryRegionAccess::ReadWrite;
};

enum class MemoryWatchpointAccess {
    Read,
    Write,
    ReadWrite
};

struct MemoryAccessEvent {
    MemoryAccessOperation operation = MemoryAccessOperation::Read;
    std::uint32_t address = 0u;
    MemoryAccessWidth width = MemoryAccessWidth::Byte;
    std::uint32_t value = 0u;
    std::string region_name;
};

using MemoryAccessObserver =
    std::function<void(const MemoryAccessEvent&)>;
using MemoryWatchpointId = std::uint64_t;

class Memory {
public:
    explicit Memory(
        std::size_t legacy_size = 1024u * 1024u,
        MemoryAlignmentPolicy alignment_policy =
            MemoryAlignmentPolicy::Strict
    );

    void map_region(
        std::string name,
        std::uint32_t base_address,
        std::shared_ptr<MemoryDevice> device,
        MemoryRegionAccess access = MemoryRegionAccess::ReadWrite
    );

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t region_count() const noexcept;
    [[nodiscard]] const MemoryRegionInfo& region(
        std::size_t index
    ) const;
    [[nodiscard]] bool contains(
        std::uint32_t address,
        std::size_t width = 1u
    ) const noexcept;

    [[nodiscard]] MemoryAlignmentPolicy alignment_policy() const noexcept;
    void set_alignment_policy(MemoryAlignmentPolicy policy) noexcept;

    [[nodiscard]] MemoryWatchpointId add_watchpoint(
        std::uint32_t address,
        std::size_t size,
        MemoryWatchpointAccess access,
        MemoryAccessObserver observer
    );
    [[nodiscard]] bool remove_watchpoint(MemoryWatchpointId id);
    void clear_watchpoints() noexcept;
    [[nodiscard]] std::size_t watchpoint_count() const noexcept;

    void set_trace_handler(MemoryAccessObserver observer);
    void clear_trace_handler() noexcept;
    [[nodiscard]] bool has_trace_handler() const noexcept;

    [[nodiscard]] std::uint8_t read_u8(std::uint32_t address) const;
    [[nodiscard]] std::uint16_t read_u16(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t read_u32(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t read_s8(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t read_s16(std::uint32_t address) const;

    void write_u8(std::uint32_t address, std::uint8_t value);
    void write_u16(std::uint32_t address, std::uint16_t value);
    void write_u32(std::uint32_t address, std::uint32_t value);

private:
    struct MappedRegion {
        MemoryRegionInfo info;
        std::shared_ptr<MemoryDevice> device;
    };

    struct Watchpoint {
        MemoryWatchpointId id = 0u;
        std::uint32_t address = 0u;
        std::size_t size = 0u;
        MemoryWatchpointAccess access =
            MemoryWatchpointAccess::ReadWrite;
        MemoryAccessObserver observer;
    };

    [[nodiscard]] const MappedRegion& resolve(
        std::uint32_t address,
        MemoryAccessWidth width,
        MemoryAccessOperation operation
    ) const;
    [[nodiscard]] const MappedRegion& resolve_writable(
        std::uint32_t address,
        MemoryAccessWidth width
    ) const;
    void require_alignment(
        std::uint32_t address,
        MemoryAccessWidth width,
        MemoryAccessOperation operation
    ) const;
    void notify_access(const MemoryAccessEvent& event) const;

    MemoryAlignmentPolicy alignment_policy_ =
        MemoryAlignmentPolicy::Strict;
    std::vector<MappedRegion> regions_;
    std::vector<Watchpoint> watchpoints_;
    MemoryAccessObserver trace_handler_;
    MemoryWatchpointId next_watchpoint_id_ = 1u;
};

} // namespace katana::runtime
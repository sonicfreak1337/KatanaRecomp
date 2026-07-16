#include "katana/runtime/memory.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace katana::runtime {
namespace {

constexpr std::uint64_t address_space_size = 0x100000000ull;

std::string hex_address(const std::uint32_t address) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << address;
    return output.str();
}

std::uint32_t region_offset(const MemoryRegionInfo& info, const std::uint32_t address) {
    return address - info.base_address;
}

std::size_t width_bytes(const MemoryAccessWidth width) {
    return static_cast<std::size_t>(width);
}

const char* operation_name(const MemoryAccessOperation operation) {
    switch (operation) {
    case MemoryAccessOperation::Read:
        return "Lesezugriff";
    case MemoryAccessOperation::Write:
        return "Schreibzugriff";
    }
    return "Speicherzugriff";
}

std::string access_error_message(const MemoryAccessErrorReason reason,
                                 const MemoryAccessOperation operation,
                                 const std::uint32_t address,
                                 const MemoryAccessWidth width,
                                 const std::string& region_name) {
    const auto bytes = width_bytes(width);

    std::ostringstream output;
    output << operation_name(operation) << " mit " << bytes << " Byte bei " << hex_address(address)
           << ": ";

    switch (reason) {
    case MemoryAccessErrorReason::Misaligned:
        output << "Adresse ist nicht natuerlich auf " << bytes << " Byte ausgerichtet.";
        break;
    case MemoryAccessErrorReason::Unmapped:
        output << "keine Speicherregion ist zugeordnet.";
        break;
    case MemoryAccessErrorReason::CrossRegion:
        output << "Zugriff ueberschreitet die Region '" << region_name << "'.";
        break;
    case MemoryAccessErrorReason::ReadOnly:
        output << "Region '" << region_name << "' ist schreibgeschuetzt.";
        break;
    case MemoryAccessErrorReason::AddressOverflow:
        output << "Zugriff ueberschreitet den 32-Bit-Adressraum.";
        break;
    }

    return output.str();
}

void require_device_access(const std::size_t device_size,
                           const std::uint32_t offset,
                           const MemoryAccessWidth width) {
    const auto access_size = width_bytes(width);
    const auto start = static_cast<std::size_t>(offset);
    if (access_size > device_size || start > device_size - access_size) {
        throw std::out_of_range("Speichergeraetezugriff ausserhalb der Region.");
    }
}

std::uint32_t width_mask(const MemoryAccessWidth width) {
    switch (width) {
    case MemoryAccessWidth::Byte:
        return 0x000000FFu;
    case MemoryAccessWidth::Halfword:
        return 0x0000FFFFu;
    case MemoryAccessWidth::Word:
        return 0xFFFFFFFFu;
    }
    throw std::invalid_argument("Ungueltige Speicherzugriffsbreite.");
}

bool watchpoint_accepts(const MemoryWatchpointAccess watchpoint_access,
                        const MemoryAccessOperation operation) {
    switch (watchpoint_access) {
    case MemoryWatchpointAccess::Read:
        return operation == MemoryAccessOperation::Read;
    case MemoryWatchpointAccess::Write:
        return operation == MemoryAccessOperation::Write;
    case MemoryWatchpointAccess::ReadWrite:
        return true;
    }
    return false;
}

bool ranges_overlap(const std::uint32_t left_address,
                    const std::size_t left_size,
                    const std::uint32_t right_address,
                    const std::size_t right_size) {
    const std::uint64_t left_start = left_address;
    const std::uint64_t left_end = left_start + left_size;
    const std::uint64_t right_start = right_address;
    const std::uint64_t right_end = right_start + right_size;

    return left_start < right_end && right_start < left_end;
}

} // namespace

MemoryAccessError::MemoryAccessError(const MemoryAccessErrorReason reason,
                                     const MemoryAccessOperation operation,
                                     const std::uint32_t address,
                                     const MemoryAccessWidth width,
                                     std::string region_name)
    : std::runtime_error(access_error_message(reason, operation, address, width, region_name)),
      reason_(reason), operation_(operation), address_(address), width_(width),
      region_name_(std::move(region_name)) {}

MemoryAccessErrorReason MemoryAccessError::reason() const noexcept {
    return reason_;
}

MemoryAccessOperation MemoryAccessError::operation() const noexcept {
    return operation_;
}

std::uint32_t MemoryAccessError::address() const noexcept {
    return address_;
}

MemoryAccessWidth MemoryAccessError::width() const noexcept {
    return width_;
}

const std::string& MemoryAccessError::region_name() const noexcept {
    return region_name_;
}

std::uint16_t MemoryDevice::read_u16(const std::uint32_t offset) const {
    require_device_access(size(), offset, MemoryAccessWidth::Halfword);
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(read_u8(offset)) |
                                      (static_cast<std::uint16_t>(read_u8(offset + 1u)) << 8u));
}

std::uint32_t MemoryDevice::read_u32(const std::uint32_t offset) const {
    require_device_access(size(), offset, MemoryAccessWidth::Word);
    return static_cast<std::uint32_t>(read_u8(offset)) |
           (static_cast<std::uint32_t>(read_u8(offset + 1u)) << 8u) |
           (static_cast<std::uint32_t>(read_u8(offset + 2u)) << 16u) |
           (static_cast<std::uint32_t>(read_u8(offset + 3u)) << 24u);
}

void MemoryDevice::write_u16(const std::uint32_t offset, const std::uint16_t value) {
    require_device_access(size(), offset, MemoryAccessWidth::Halfword);
    write_u8(offset, static_cast<std::uint8_t>(value & 0xFFu));
    write_u8(offset + 1u, static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
}

void MemoryDevice::write_u32(const std::uint32_t offset, const std::uint32_t value) {
    require_device_access(size(), offset, MemoryAccessWidth::Word);
    write_u8(offset, static_cast<std::uint8_t>(value & 0xFFu));
    write_u8(offset + 1u, static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
    write_u8(offset + 2u, static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
    write_u8(offset + 3u, static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
}

LinearMemoryDevice::LinearMemoryDevice(const std::size_t size) : bytes_(size, 0u) {
    if (size == 0u) {
        throw std::invalid_argument("Eine lineare Speicherregion darf nicht leer sein.");
    }
}

std::size_t LinearMemoryDevice::size() const noexcept {
    return bytes_.size();
}

std::uint8_t LinearMemoryDevice::read_u8(const std::uint32_t offset) const {
    check(offset);
    return bytes_[static_cast<std::size_t>(offset)];
}

void LinearMemoryDevice::write_u8(const std::uint32_t offset, const std::uint8_t value) {
    check(offset);
    bytes_[static_cast<std::size_t>(offset)] = value;
}

std::span<const std::uint8_t> LinearMemoryDevice::bytes() const noexcept {
    return bytes_;
}

std::span<std::uint8_t> LinearMemoryDevice::writable_bytes() noexcept {
    return bytes_;
}

void LinearMemoryDevice::check(const std::uint32_t offset) const {
    if (static_cast<std::size_t>(offset) >= bytes_.size()) {
        throw std::out_of_range("Speichergeraetezugriff ausserhalb der Region.");
    }
}

MmioMemoryDevice::MmioMemoryDevice(const std::size_t size,
                                   MmioReadHandler read_handler,
                                   MmioWriteHandler write_handler)
    : size_(size), read_handler_(std::move(read_handler)),
      write_handler_(std::move(write_handler)) {
    if (size_ == 0u) {
        throw std::invalid_argument("Eine MMIO-Region darf nicht leer sein.");
    }
    if (!read_handler_ && !write_handler_) {
        throw std::invalid_argument("Eine MMIO-Region braucht mindestens einen Handler.");
    }
}

std::size_t MmioMemoryDevice::size() const noexcept {
    return size_;
}

std::uint8_t MmioMemoryDevice::read_u8(const std::uint32_t offset) const {
    return static_cast<std::uint8_t>(read(offset, MemoryAccessWidth::Byte));
}

std::uint16_t MmioMemoryDevice::read_u16(const std::uint32_t offset) const {
    return static_cast<std::uint16_t>(read(offset, MemoryAccessWidth::Halfword));
}

std::uint32_t MmioMemoryDevice::read_u32(const std::uint32_t offset) const {
    return read(offset, MemoryAccessWidth::Word);
}

void MmioMemoryDevice::write_u8(const std::uint32_t offset, const std::uint8_t value) {
    write(offset, value, MemoryAccessWidth::Byte);
}

void MmioMemoryDevice::write_u16(const std::uint32_t offset, const std::uint16_t value) {
    write(offset, value, MemoryAccessWidth::Halfword);
}

void MmioMemoryDevice::write_u32(const std::uint32_t offset, const std::uint32_t value) {
    write(offset, value, MemoryAccessWidth::Word);
}

void MmioMemoryDevice::check(const std::uint32_t offset, const MemoryAccessWidth width) const {
    require_device_access(size_, offset, width);
}

std::uint32_t MmioMemoryDevice::read(const std::uint32_t offset,
                                     const MemoryAccessWidth width) const {
    check(offset, width);
    if (!read_handler_) {
        throw std::runtime_error("MMIO-Lesezugriff ohne registrierten Lesehandler.");
    }
    return read_handler_(offset, width) & width_mask(width);
}

void MmioMemoryDevice::write(const std::uint32_t offset,
                             const std::uint32_t value,
                             const MemoryAccessWidth width) {
    check(offset, width);
    if (!write_handler_) {
        throw std::runtime_error("MMIO-Schreibzugriff ohne registrierten Schreibhandler.");
    }
    write_handler_(offset, value & width_mask(width), width);
}

Memory::Memory(const std::size_t legacy_size, const MemoryAlignmentPolicy alignment_policy)
    : alignment_policy_(alignment_policy) {
    if (legacy_size != 0u) {
        map_region("legacy-linear-memory", 0u, std::make_shared<LinearMemoryDevice>(legacy_size));
    }
}

void Memory::map_region(std::string name,
                        const std::uint32_t base_address,
                        std::shared_ptr<MemoryDevice> device,
                        const MemoryRegionAccess access) {
    if (name.empty()) {
        throw std::invalid_argument("Eine Speicherregion braucht einen Namen.");
    }
    if (!device) {
        throw std::invalid_argument("Eine Speicherregion braucht ein Speichergeraet.");
    }
    if (device->size() == 0u) {
        throw std::invalid_argument("Eine Speicherregion darf nicht leer sein.");
    }

    const std::uint64_t start = base_address;
    if (device->size() > address_space_size - start) {
        throw std::invalid_argument("Eine Speicherregion ueberschreitet den 32-Bit-Adressraum.");
    }
    const std::uint64_t end = start + device->size();

    for (const auto& existing : regions_) {
        const std::uint64_t existing_start = existing.info.base_address;
        const std::uint64_t existing_end = existing_start + existing.info.size;
        if (start < existing_end && existing_start < end) {
            throw std::invalid_argument("Speicherregionen duerfen sich nicht ueberlappen.");
        }
    }

    regions_.push_back(
        MappedRegion{MemoryRegionInfo{std::move(name), base_address, device->size(), access},
                     std::move(device)});

    std::sort(
        regions_.begin(), regions_.end(), [](const MappedRegion& left, const MappedRegion& right) {
            return left.info.base_address < right.info.base_address;
        });
}

std::size_t Memory::size() const noexcept {
    std::size_t total = 0u;
    for (const auto& mapped : regions_) {
        total += mapped.info.size;
    }
    return total;
}

std::size_t Memory::region_count() const noexcept {
    return regions_.size();
}

const MemoryRegionInfo& Memory::region(const std::size_t index) const {
    if (index >= regions_.size()) {
        throw std::out_of_range("Ungueltiger Speicherregionsindex.");
    }
    return regions_[index].info;
}

bool Memory::contains(const std::uint32_t address, const std::size_t width) const noexcept {
    if (width == 0u) {
        return false;
    }

    const std::uint64_t start = address;
    if (width > address_space_size - start) {
        return false;
    }
    const std::uint64_t end = start + width;

    for (const auto& mapped : regions_) {
        const std::uint64_t region_start = mapped.info.base_address;
        const std::uint64_t region_end = region_start + mapped.info.size;
        if (start >= region_start && end <= region_end) {
            return true;
        }
    }
    return false;
}

bool Memory::maps_device(const std::uint32_t address,
                         const std::size_t width,
                         const MemoryDevice* const device) const noexcept {
    if (device == nullptr || width == 0u) return false;
    const auto end = static_cast<std::uint64_t>(address) + width;
    for (const auto& region : regions_) {
        const auto region_end =
            static_cast<std::uint64_t>(region.info.base_address) + region.info.size;
        if (address >= region.info.base_address && end <= region_end) {
            return region.device.get() == device;
        }
    }
    return false;
}

MemoryAlignmentPolicy Memory::alignment_policy() const noexcept {
    return alignment_policy_;
}

void Memory::set_alignment_policy(const MemoryAlignmentPolicy policy) noexcept {
    alignment_policy_ = policy;
}

MemoryWatchpointId Memory::add_watchpoint(const std::uint32_t address,
                                          const std::size_t size,
                                          const MemoryWatchpointAccess access,
                                          MemoryAccessObserver observer) {
    if (size == 0u) {
        throw std::invalid_argument("Ein Watchpoint braucht eine positive Groesse.");
    }
    const std::uint64_t start = address;
    if (size > address_space_size - start) {
        throw std::invalid_argument("Ein Watchpoint ueberschreitet den 32-Bit-Adressraum.");
    }
    if (!observer) {
        throw std::invalid_argument("Ein Watchpoint braucht einen Observer.");
    }
    if (next_watchpoint_id_ == std::numeric_limits<MemoryWatchpointId>::max()) {
        throw std::overflow_error("Es koennen keine weiteren Watchpoints registriert werden.");
    }

    const auto id = next_watchpoint_id_++;
    watchpoints_.push_back(Watchpoint{id, address, size, access, std::move(observer)});
    return id;
}

bool Memory::remove_watchpoint(const MemoryWatchpointId id) {
    const auto iterator =
        std::find_if(watchpoints_.begin(), watchpoints_.end(), [id](const Watchpoint& watchpoint) {
            return watchpoint.id == id;
        });
    if (iterator == watchpoints_.end()) {
        return false;
    }

    watchpoints_.erase(iterator);
    return true;
}

void Memory::clear_watchpoints() noexcept {
    watchpoints_.clear();
}

std::size_t Memory::watchpoint_count() const noexcept {
    return watchpoints_.size();
}

void Memory::set_trace_handler(MemoryAccessObserver observer) {
    trace_handler_ = std::move(observer);
}

void Memory::clear_trace_handler() noexcept {
    trace_handler_ = {};
}

bool Memory::has_trace_handler() const noexcept {
    return static_cast<bool>(trace_handler_);
}

std::uint8_t Memory::read_u8(const std::uint32_t address) const {
    const auto& mapped = resolve(address, MemoryAccessWidth::Byte, MemoryAccessOperation::Read);
    const auto value = mapped.device->read_u8(region_offset(mapped.info, address));
    notify_access(MemoryAccessEvent{
        MemoryAccessOperation::Read, address, MemoryAccessWidth::Byte, value, mapped.info.name});
    return value;
}

std::uint16_t Memory::read_u16(const std::uint32_t address) const {
    const auto& mapped = resolve(address, MemoryAccessWidth::Halfword, MemoryAccessOperation::Read);
    const auto value = mapped.device->read_u16(region_offset(mapped.info, address));
    notify_access(MemoryAccessEvent{MemoryAccessOperation::Read,
                                    address,
                                    MemoryAccessWidth::Halfword,
                                    value,
                                    mapped.info.name});
    return value;
}

std::uint32_t Memory::read_u32(const std::uint32_t address) const {
    const auto& mapped = resolve(address, MemoryAccessWidth::Word, MemoryAccessOperation::Read);
    const auto value = mapped.device->read_u32(region_offset(mapped.info, address));
    notify_access(MemoryAccessEvent{
        MemoryAccessOperation::Read, address, MemoryAccessWidth::Word, value, mapped.info.name});
    return value;
}

std::uint32_t Memory::read_s8(const std::uint32_t address) const {
    const auto value = read_u8(address);
    return (value & 0x80u) != 0u ? 0xFFFFFF00u | static_cast<std::uint32_t>(value)
                                 : static_cast<std::uint32_t>(value);
}

std::uint32_t Memory::read_s16(const std::uint32_t address) const {
    const auto value = read_u16(address);
    return (value & 0x8000u) != 0u ? 0xFFFF0000u | static_cast<std::uint32_t>(value)
                                   : static_cast<std::uint32_t>(value);
}

void Memory::write_u8(const std::uint32_t address, const std::uint8_t value) {
    const auto& mapped = resolve_writable(address, MemoryAccessWidth::Byte);
    mapped.device->write_u8(region_offset(mapped.info, address), value);
    notify_access(MemoryAccessEvent{
        MemoryAccessOperation::Write, address, MemoryAccessWidth::Byte, value, mapped.info.name});
}

void Memory::write_u16(const std::uint32_t address, const std::uint16_t value) {
    const auto& mapped = resolve_writable(address, MemoryAccessWidth::Halfword);
    mapped.device->write_u16(region_offset(mapped.info, address), value);
    notify_access(MemoryAccessEvent{MemoryAccessOperation::Write,
                                    address,
                                    MemoryAccessWidth::Halfword,
                                    value,
                                    mapped.info.name});
}

void Memory::write_u32(const std::uint32_t address, const std::uint32_t value) {
    const auto& mapped = resolve_writable(address, MemoryAccessWidth::Word);
    mapped.device->write_u32(region_offset(mapped.info, address), value);
    notify_access(MemoryAccessEvent{
        MemoryAccessOperation::Write, address, MemoryAccessWidth::Word, value, mapped.info.name});
}

const Memory::MappedRegion& Memory::resolve(const std::uint32_t address,
                                            const MemoryAccessWidth width,
                                            const MemoryAccessOperation operation) const {
    require_alignment(address, width, operation);

    const auto access_size = width_bytes(width);
    const std::uint64_t start = address;
    if (access_size > address_space_size - start) {
        throw MemoryAccessError(
            MemoryAccessErrorReason::AddressOverflow, operation, address, width);
    }
    const std::uint64_t end = start + access_size;

    for (const auto& mapped : regions_) {
        const std::uint64_t region_start = mapped.info.base_address;
        const std::uint64_t region_end = region_start + mapped.info.size;
        if (start >= region_start && end <= region_end) {
            return mapped;
        }
        if (start >= region_start && start < region_end) {
            throw MemoryAccessError(
                MemoryAccessErrorReason::CrossRegion, operation, address, width, mapped.info.name);
        }
    }

    throw MemoryAccessError(MemoryAccessErrorReason::Unmapped, operation, address, width);
}

const Memory::MappedRegion& Memory::resolve_writable(const std::uint32_t address,
                                                     const MemoryAccessWidth width) const {
    const auto& mapped = resolve(address, width, MemoryAccessOperation::Write);
    if (mapped.info.access != MemoryRegionAccess::ReadWrite) {
        throw MemoryAccessError(MemoryAccessErrorReason::ReadOnly,
                                MemoryAccessOperation::Write,
                                address,
                                width,
                                mapped.info.name);
    }
    return mapped;
}

void Memory::require_alignment(const std::uint32_t address,
                               const MemoryAccessWidth width,
                               const MemoryAccessOperation operation) const {
    if (alignment_policy_ == MemoryAlignmentPolicy::Permissive) {
        return;
    }

    const auto access_size = width_bytes(width);
    if ((static_cast<std::size_t>(address) % access_size) != 0u) {
        throw MemoryAccessError(MemoryAccessErrorReason::Misaligned, operation, address, width);
    }
}

void Memory::notify_access(const MemoryAccessEvent& event) const {
    std::vector<MemoryAccessObserver> observers;
    observers.reserve(watchpoints_.size() + (trace_handler_ ? 1u : 0u));

    if (trace_handler_) {
        observers.push_back(trace_handler_);
    }

    const auto access_size = width_bytes(event.width);
    for (const auto& watchpoint : watchpoints_) {
        if (watchpoint_accepts(watchpoint.access, event.operation) &&
            ranges_overlap(watchpoint.address, watchpoint.size, event.address, access_size)) {
            observers.push_back(watchpoint.observer);
        }
    }

    for (const auto& observer : observers) {
        observer(event);
    }
}

} // namespace katana::runtime

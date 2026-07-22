#include "katana/runtime/memory.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace katana::runtime {
namespace {

constexpr std::uint64_t address_space_size = 0x100000000ull;
constexpr unsigned region_page_shift = 16u;
constexpr std::size_t region_page_count = 1u << (32u - region_page_shift);
constexpr std::int32_t unmapped_region = -1;
constexpr std::int32_t ambiguous_region = -2;

std::uint16_t little_u16(std::uint16_t value) noexcept {
    if constexpr (std::endian::native == std::endian::big)
        value = static_cast<std::uint16_t>((value >> 8u) | (value << 8u));
    return value;
}

std::uint32_t little_u32(std::uint32_t value) noexcept {
    if constexpr (std::endian::native == std::endian::big) {
        value = ((value & 0x000000FFu) << 24u) | ((value & 0x0000FF00u) << 8u) |
                ((value & 0x00FF0000u) >> 8u) | ((value & 0xFF000000u) >> 24u);
    }
    return value;
}

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
    case MemoryAccessErrorReason::DeviceRejected:
        output << "MMIO-Geraet hat den Gastzugriff strukturiert abgewiesen.";
        break;
    case MemoryAccessErrorReason::TlbMiss:
        output << "keine passende gueltige TLB-Abbildung.";
        break;
    case MemoryAccessErrorReason::TlbMultipleHit:
        output << "mehrere gueltige TLB-Abbildungen treffen dieselbe Adresse.";
        break;
    case MemoryAccessErrorReason::InitialPageWrite:
        output << "TLB-Seite ist noch nicht als dirty markiert.";
        break;
    case MemoryAccessErrorReason::TlbProtection:
        output << "TLB-Schutzrechte weisen den Gastzugriff ab.";
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

template <typename Operation>
decltype(auto) mmio_boundary(const MemoryRegionInfo& region,
                             const std::uint32_t address,
                             const MemoryAccessWidth width,
                             const MemoryAccessOperation access,
                             Operation&& operation) {
    try {
        return std::forward<Operation>(operation)();
    } catch (const MemoryAccessError&) {
        throw;
    } catch (const std::exception&) {
        throw MemoryAccessError(
            MemoryAccessErrorReason::DeviceRejected, access, address, width, region.name);
    }
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

std::uint16_t LinearMemoryDevice::read_u16(const std::uint32_t offset) const {
    require_device_access(bytes_.size(), offset, MemoryAccessWidth::Halfword);
    std::uint16_t value = 0u;
    std::memcpy(&value, bytes_.data() + offset, sizeof(value));
    return little_u16(value);
}

std::uint32_t LinearMemoryDevice::read_u32(const std::uint32_t offset) const {
    require_device_access(bytes_.size(), offset, MemoryAccessWidth::Word);
    std::uint32_t value = 0u;
    std::memcpy(&value, bytes_.data() + offset, sizeof(value));
    return little_u32(value);
}

void LinearMemoryDevice::write_u8(const std::uint32_t offset, const std::uint8_t value) {
    check(offset);
    bytes_[static_cast<std::size_t>(offset)] = value;
}

void LinearMemoryDevice::write_u16(const std::uint32_t offset, const std::uint16_t value) {
    require_device_access(bytes_.size(), offset, MemoryAccessWidth::Halfword);
    const auto stored = little_u16(value);
    std::memcpy(bytes_.data() + offset, &stored, sizeof(stored));
}

void LinearMemoryDevice::write_u32(const std::uint32_t offset, const std::uint32_t value) {
    require_device_access(bytes_.size(), offset, MemoryAccessWidth::Word);
    const auto stored = little_u32(value);
    std::memcpy(bytes_.data() + offset, &stored, sizeof(stored));
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
    try {
        return read_handler_(offset, width) & width_mask(width);
    } catch (const MemoryAccessError&) {
        throw;
    } catch (const std::exception& error) {
        throw MmioDeviceError(error.what());
    }
}

void MmioMemoryDevice::write(const std::uint32_t offset,
                             const std::uint32_t value,
                             const MemoryAccessWidth width) {
    check(offset, width);
    if (!write_handler_) {
        throw std::runtime_error("MMIO-Schreibzugriff ohne registrierten Schreibhandler.");
    }
    try {
        write_handler_(offset, value & width_mask(width), width);
    } catch (const MemoryAccessError&) {
        throw;
    } catch (const std::exception& error) {
        throw MmioDeviceError(error.what());
    }
}

Memory::Memory(const std::size_t legacy_size, const MemoryAlignmentPolicy alignment_policy)
    : alignment_policy_(alignment_policy), region_page_index_(region_page_count, unmapped_region) {
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

    auto* linear = dynamic_cast<LinearMemoryDevice*>(device.get());
    const bool mmio = dynamic_cast<MmioMemoryDevice*>(device.get()) != nullptr;
    regions_.push_back(
        MappedRegion{MemoryRegionInfo{std::move(name), base_address, device->size(), access},
                     std::move(device),
                     linear,
                     mmio});

    std::sort(
        regions_.begin(), regions_.end(), [](const MappedRegion& left, const MappedRegion& right) {
            return left.info.base_address < right.info.base_address;
        });
    rebuild_region_index();
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

    if (indexed_region(address, width) != nullptr) {
        return true;
    }

    for (const auto& mapped : regions_) {
        ++performance_counters_.reference_region_probes;
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
    if (width > address_space_size - static_cast<std::uint64_t>(address)) return false;
    const auto end = static_cast<std::uint64_t>(address) + width;
    if (const auto* mapped = indexed_region(address, width); mapped != nullptr) {
        return mapped->device.get() == device;
    }
    for (const auto& region : regions_) {
        ++performance_counters_.reference_region_probes;
        const auto region_end =
            static_cast<std::uint64_t>(region.info.base_address) + region.info.size;
        if (address >= region.info.base_address && end <= region_end) {
            return region.device.get() == device;
        }
    }
    return false;
}

bool Memory::is_writable_linear_range(const std::uint32_t address,
                                      const std::size_t width) const noexcept {
    if (width == 0u || width > address_space_size - static_cast<std::uint64_t>(address))
        return false;
    const auto writable_linear = [](const MappedRegion& mapped) {
        return mapped.info.access == MemoryRegionAccess::ReadWrite && mapped.linear != nullptr;
    };
    if (const auto* mapped = indexed_region(address, width); mapped != nullptr)
        return writable_linear(*mapped);
    const auto end = static_cast<std::uint64_t>(address) + width;
    for (const auto& mapped : regions_) {
        ++performance_counters_.reference_region_probes;
        const auto region_end =
            static_cast<std::uint64_t>(mapped.info.base_address) + mapped.info.size;
        if (address >= mapped.info.base_address && end <= region_end)
            return writable_linear(mapped);
    }
    return false;
}

MemoryAlignmentPolicy Memory::alignment_policy() const noexcept {
    return alignment_policy_;
}

void Memory::set_alignment_policy(const MemoryAlignmentPolicy policy) noexcept {
    alignment_policy_ = policy;
}

MemoryLookupMode Memory::lookup_mode() const noexcept {
    return lookup_mode_;
}

void Memory::set_lookup_mode(const MemoryLookupMode mode) noexcept {
    lookup_mode_ = mode;
}

const MemoryPerformanceCounters& Memory::performance_counters() const noexcept {
    return performance_counters_;
}

void Memory::reset_performance_counters() const noexcept {
    performance_counters_ = {};
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

void Memory::set_mmio_access_tracking(const bool enabled) noexcept {
    mmio_access_tracking_enabled_ = enabled;
    if (!enabled) last_mmio_access_.reset();
}

bool Memory::mmio_access_tracking_enabled() const noexcept {
    return mmio_access_tracking_enabled_;
}

const std::optional<MemoryAccessEvent>& Memory::last_mmio_access() const noexcept {
    return last_mmio_access_;
}

void Memory::clear_last_mmio_access() const noexcept {
    last_mmio_access_.reset();
}

void Memory::set_guest_write_observer(GuestWriteObserver observer) {
    guest_write_observer_ = std::move(observer);
}

void Memory::clear_guest_write_observer() noexcept {
    guest_write_observer_ = {};
}

bool Memory::has_guest_write_observer() const noexcept {
    return static_cast<bool>(guest_write_observer_);
}

std::uint8_t Memory::read_u8(const std::uint32_t address) const {
    const auto& mapped = resolve(address, MemoryAccessWidth::Byte, MemoryAccessOperation::Read);
    const auto offset = region_offset(mapped.info, address);
    const auto value = mapped.linear != nullptr
                           ? mapped.linear->read_u8(offset)
                           : mmio_boundary(mapped.info,
                                           address,
                                           MemoryAccessWidth::Byte,
                                           MemoryAccessOperation::Read,
                                           [&] { return mapped.device->read_u8(offset); });
    record_mmio_access(
        mapped, MemoryAccessOperation::Read, address, MemoryAccessWidth::Byte, value);
    if (access_observers_active()) {
        ++performance_counters_.observed_accesses;
        notify_access(MemoryAccessEvent{MemoryAccessOperation::Read,
                                        address,
                                        MemoryAccessWidth::Byte,
                                        value,
                                        mapped.info.name});
    } else {
        ++performance_counters_.unobserved_accesses;
    }
    return value;
}

std::uint16_t Memory::read_u16(const std::uint32_t address) const {
    const auto& mapped = resolve(address, MemoryAccessWidth::Halfword, MemoryAccessOperation::Read);
    const auto offset = region_offset(mapped.info, address);
    const auto value = mapped.linear != nullptr
                           ? mapped.linear->read_u16(offset)
                           : mmio_boundary(mapped.info,
                                           address,
                                           MemoryAccessWidth::Halfword,
                                           MemoryAccessOperation::Read,
                                           [&] { return mapped.device->read_u16(offset); });
    record_mmio_access(
        mapped, MemoryAccessOperation::Read, address, MemoryAccessWidth::Halfword, value);
    if (access_observers_active()) {
        ++performance_counters_.observed_accesses;
        notify_access(MemoryAccessEvent{MemoryAccessOperation::Read,
                                        address,
                                        MemoryAccessWidth::Halfword,
                                        value,
                                        mapped.info.name});
    } else {
        ++performance_counters_.unobserved_accesses;
    }
    return value;
}

std::uint32_t Memory::read_u32(const std::uint32_t address) const {
    const auto& mapped = resolve(address, MemoryAccessWidth::Word, MemoryAccessOperation::Read);
    const auto offset = region_offset(mapped.info, address);
    const auto value = mapped.linear != nullptr
                           ? mapped.linear->read_u32(offset)
                           : mmio_boundary(mapped.info,
                                           address,
                                           MemoryAccessWidth::Word,
                                           MemoryAccessOperation::Read,
                                           [&] { return mapped.device->read_u32(offset); });
    record_mmio_access(
        mapped, MemoryAccessOperation::Read, address, MemoryAccessWidth::Word, value);
    if (access_observers_active()) {
        ++performance_counters_.observed_accesses;
        notify_access(MemoryAccessEvent{MemoryAccessOperation::Read,
                                        address,
                                        MemoryAccessWidth::Word,
                                        value,
                                        mapped.info.name});
    } else {
        ++performance_counters_.unobserved_accesses;
    }
    return value;
}

std::uint32_t
Memory::peek_u32(const std::uint32_t address,
                 const std::span<const MemoryDevice* const> permitted_devices) const {
    const auto& mapped = resolve(address, MemoryAccessWidth::Word, MemoryAccessOperation::Read);
    if (std::find(permitted_devices.begin(), permitted_devices.end(), mapped.device.get()) ==
        permitted_devices.end())
        throw MemoryAccessError(MemoryAccessErrorReason::Unmapped,
                                MemoryAccessOperation::Read,
                                address,
                                MemoryAccessWidth::Word,
                                "diagnostic-peek-denied");
    const auto offset = region_offset(mapped.info, address);
    return mapped.linear != nullptr ? mapped.linear->read_u32(offset)
                                    : mapped.device->read_u32(offset);
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

void Memory::write_u8(const std::uint32_t address,
                      const std::uint8_t value,
                      const CodeWriteSource source) {
    const auto& mapped = resolve_writable(address, MemoryAccessWidth::Byte);
    const auto offset = region_offset(mapped.info, address);
    const bool changed = !guest_write_observer_ || mapped.linear == nullptr ||
                         mapped.linear->read_u8(offset) != value;
    if (mapped.linear != nullptr)
        mapped.linear->write_u8(offset, value);
    else
        mmio_boundary(mapped.info,
                      address,
                      MemoryAccessWidth::Byte,
                      MemoryAccessOperation::Write,
                      [&] { mapped.device->write_u8(offset, value); });
    record_mmio_access(
        mapped, MemoryAccessOperation::Write, address, MemoryAccessWidth::Byte, value);
    if (access_observers_active()) {
        ++performance_counters_.observed_accesses;
        notify_access(MemoryAccessEvent{MemoryAccessOperation::Write,
                                        address,
                                        MemoryAccessWidth::Byte,
                                        value,
                                        mapped.info.name});
    } else {
        ++performance_counters_.unobserved_accesses;
    }
    notify_guest_write({address, 1u, source, changed});
}

void Memory::write_u16(const std::uint32_t address,
                       const std::uint16_t value,
                       const CodeWriteSource source) {
    const auto& mapped = resolve_writable(address, MemoryAccessWidth::Halfword);
    const auto offset = region_offset(mapped.info, address);
    const bool changed = !guest_write_observer_ || mapped.linear == nullptr ||
                         mapped.linear->read_u16(offset) != value;
    if (mapped.linear != nullptr)
        mapped.linear->write_u16(offset, value);
    else
        mmio_boundary(mapped.info,
                      address,
                      MemoryAccessWidth::Halfword,
                      MemoryAccessOperation::Write,
                      [&] { mapped.device->write_u16(offset, value); });
    record_mmio_access(
        mapped, MemoryAccessOperation::Write, address, MemoryAccessWidth::Halfword, value);
    if (access_observers_active()) {
        ++performance_counters_.observed_accesses;
        notify_access(MemoryAccessEvent{MemoryAccessOperation::Write,
                                        address,
                                        MemoryAccessWidth::Halfword,
                                        value,
                                        mapped.info.name});
    } else {
        ++performance_counters_.unobserved_accesses;
    }
    notify_guest_write({address, 2u, source, changed});
}

void Memory::write_u32(const std::uint32_t address,
                       const std::uint32_t value,
                       const CodeWriteSource source) {
    const auto& mapped = resolve_writable(address, MemoryAccessWidth::Word);
    const auto offset = region_offset(mapped.info, address);
    const bool changed = !guest_write_observer_ || mapped.linear == nullptr ||
                         mapped.linear->read_u32(offset) != value;
    if (mapped.linear != nullptr)
        mapped.linear->write_u32(offset, value);
    else
        mmio_boundary(mapped.info,
                      address,
                      MemoryAccessWidth::Word,
                      MemoryAccessOperation::Write,
                      [&] { mapped.device->write_u32(offset, value); });
    record_mmio_access(
        mapped, MemoryAccessOperation::Write, address, MemoryAccessWidth::Word, value);
    if (access_observers_active()) {
        ++performance_counters_.observed_accesses;
        notify_access(MemoryAccessEvent{MemoryAccessOperation::Write,
                                        address,
                                        MemoryAccessWidth::Word,
                                        value,
                                        mapped.info.name});
    } else {
        ++performance_counters_.unobserved_accesses;
    }
    notify_guest_write({address, 4u, source, changed});
}

void Memory::write_bytes(const std::uint32_t address,
                         const std::span<const std::uint8_t> bytes,
                         const CodeWriteSource source) {
    if (bytes.empty()) return;
    if (bytes.size() > address_space_size - static_cast<std::uint64_t>(address)) {
        throw MemoryAccessError(MemoryAccessErrorReason::AddressOverflow,
                                MemoryAccessOperation::Write,
                                address,
                                MemoryAccessWidth::Byte);
    }

    struct PendingByteWrite {
        const MappedRegion* mapped{};
        std::uint32_t offset{};
        std::uint8_t previous{};
        bool comparable = false;
    };

    std::vector<PendingByteWrite> pending;
    pending.reserve(bytes.size());
    bool changed = false;
    for (std::size_t index = 0u; index < bytes.size(); ++index) {
        const auto current = address + static_cast<std::uint32_t>(index);
        const auto& mapped = resolve_writable(current, MemoryAccessWidth::Byte);
        const auto offset = region_offset(mapped.info, current);
        const bool comparable = mapped.linear != nullptr;
        const std::uint8_t previous =
            comparable ? mapped.linear->read_u8(offset) : std::uint8_t{0u};
        pending.push_back({&mapped, offset, previous, comparable});
        changed = changed || !comparable || previous != bytes[index];
    }

    std::size_t committed = 0u;
    try {
        while (committed < bytes.size()) {
            const auto index = committed;
            const auto current = address + static_cast<std::uint32_t>(index);
            const auto& write = pending[index];
            if (write.mapped->linear != nullptr)
                write.mapped->linear->write_u8(write.offset, bytes[index]);
            else
                mmio_boundary(write.mapped->info,
                              current,
                              MemoryAccessWidth::Byte,
                              MemoryAccessOperation::Write,
                              [&] { write.mapped->device->write_u8(write.offset, bytes[index]); });
            ++committed;
            if (access_observers_active()) {
                ++performance_counters_.observed_accesses;
                notify_access(MemoryAccessEvent{MemoryAccessOperation::Write,
                                                current,
                                                MemoryAccessWidth::Byte,
                                                bytes[index],
                                                write.mapped->info.name});
            } else {
                ++performance_counters_.unobserved_accesses;
            }
        }
    } catch (...) {
        if (committed != 0u) {
            bool committed_changed = false;
            for (std::size_t index = 0u; index < committed; ++index) {
                committed_changed = committed_changed || !pending[index].comparable ||
                                    pending[index].previous != bytes[index];
            }
            notify_guest_write({address, committed, source, committed_changed});
        }
        throw;
    }
    notify_guest_write({address, bytes.size(), source, changed});
}

void Memory::copy_bytes(const std::uint32_t destination,
                        const std::uint32_t source_address,
                        const std::size_t size,
                        const CodeWriteSource source) {
    if (size == 0u) return;
    const auto require_range = [size](const std::uint32_t address,
                                      const MemoryAccessOperation operation) {
        if (size > address_space_size - static_cast<std::uint64_t>(address))
            throw MemoryAccessError(MemoryAccessErrorReason::AddressOverflow,
                                    operation,
                                    address,
                                    MemoryAccessWidth::Byte);
    };
    require_range(source_address, MemoryAccessOperation::Read);
    require_range(destination, MemoryAccessOperation::Write);

    const auto& source_region =
        resolve(source_address, MemoryAccessWidth::Byte, MemoryAccessOperation::Read);
    const auto& source_end = resolve(source_address + static_cast<std::uint32_t>(size - 1u),
                                     MemoryAccessWidth::Byte,
                                     MemoryAccessOperation::Read);
    const auto& destination_region = resolve_writable(destination, MemoryAccessWidth::Byte);
    const auto& destination_end = resolve_writable(
        destination + static_cast<std::uint32_t>(size - 1u), MemoryAccessWidth::Byte);
    if (&source_region != &source_end || &destination_region != &destination_end)
        throw MemoryAccessError(MemoryAccessErrorReason::CrossRegion,
                                MemoryAccessOperation::Write,
                                destination,
                                MemoryAccessWidth::Byte,
                                destination_region.info.name);

    if (!access_observers_active() && source_region.linear != nullptr &&
        destination_region.linear != nullptr) {
        const auto source_offset = region_offset(source_region.info, source_address);
        const auto destination_offset = region_offset(destination_region.info, destination);
        const auto source_bytes = source_region.linear->bytes();
        auto destination_bytes = destination_region.linear->writable_bytes();
        const bool changed =
            !guest_write_observer_ || std::memcmp(destination_bytes.data() + destination_offset,
                                                  source_bytes.data() + source_offset,
                                                  size) != 0;
        std::memmove(destination_bytes.data() + destination_offset,
                     source_bytes.data() + source_offset,
                     size);
        performance_counters_.unobserved_accesses += static_cast<std::uint64_t>(size) * 2u;
        notify_guest_write({destination, size, source, changed});
        return;
    }

    std::vector<std::uint8_t> payload(size);
    for (std::size_t index = 0u; index < size; ++index)
        payload[index] = read_u8(source_address + static_cast<std::uint32_t>(index));
    write_bytes(destination, payload, source);
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

    if (const auto* mapped = indexed_region(address, access_size); mapped != nullptr) {
        return *mapped;
    }

    for (const auto& mapped : regions_) {
        ++performance_counters_.reference_region_probes;
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

const Memory::MappedRegion* Memory::indexed_region(const std::uint32_t address,
                                                   const std::size_t width) const noexcept {
    if (lookup_mode_ != MemoryLookupMode::Indexed || width == 0u) return nullptr;
    const auto end = static_cast<std::uint64_t>(address) + width;
    if (end > address_space_size) return nullptr;

    const auto slot = region_page_index_[address >> region_page_shift];
    if (slot < 0) return nullptr;
    const auto& mapped = regions_[static_cast<std::size_t>(slot)];
    const auto region_start = static_cast<std::uint64_t>(mapped.info.base_address);
    const auto region_end = region_start + mapped.info.size;
    if (address < region_start || end > region_end) return nullptr;
    ++performance_counters_.indexed_region_hits;
    return &mapped;
}

void Memory::rebuild_region_index() {
    std::fill(region_page_index_.begin(), region_page_index_.end(), unmapped_region);
    for (std::size_t index = 0u; index < regions_.size(); ++index) {
        const auto& info = regions_[index].info;
        const auto first_page = info.base_address >> region_page_shift;
        const auto last_address = static_cast<std::uint64_t>(info.base_address) + info.size - 1u;
        const auto last_page = static_cast<std::uint32_t>(last_address) >> region_page_shift;
        for (auto page = first_page;; ++page) {
            auto& slot = region_page_index_[page];
            if (slot == unmapped_region)
                slot = static_cast<std::int32_t>(index);
            else if (slot != static_cast<std::int32_t>(index))
                slot = ambiguous_region;
            if (page == last_page) break;
        }
    }
}

bool Memory::access_observers_active() const noexcept {
    return static_cast<bool>(trace_handler_) || !watchpoints_.empty();
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

void Memory::record_mmio_access(const MappedRegion& mapped,
                                const MemoryAccessOperation operation,
                                const std::uint32_t address,
                                const MemoryAccessWidth width,
                                const std::uint32_t value) const {
    if (!mmio_access_tracking_enabled_ || !mapped.mmio) return;
    last_mmio_access_ = MemoryAccessEvent{operation, address, width, value, mapped.info.name};
}

void Memory::notify_guest_write(const GuestWriteEvent& event) const {
    if (guest_write_observer_) guest_write_observer_(event);
}

} // namespace katana::runtime

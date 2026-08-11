#include "katana/runtime/memory.hpp"

#include "katana/runtime/crash_capsule.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <exception>
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

class DiagnosticChangedBytes final {
  public:
    static constexpr std::size_t inline_capacity = 256u;

    explicit DiagnosticChangedBytes(const std::size_t size) noexcept {
        if (size > guest_memory_access_change_tracking_limit) return;
        if (size <= inline_.size()) {
            bytes_ = std::span<std::uint8_t>(inline_.data(), size);
            return;
        }
        try {
            dynamic_.resize(size);
            bytes_ = dynamic_;
        } catch (...) {
            bytes_ = {};
        }
    }

    [[nodiscard]] bool available(const std::size_t expected_size) const noexcept {
        return bytes_.size() == expected_size;
    }

    void set(const std::size_t index, const bool changed) noexcept {
        if (index < bytes_.size()) bytes_[index] = changed ? 1u : 0u;
    }

    [[nodiscard]] std::span<const std::uint8_t>
    first(const std::size_t size) const noexcept {
        return size <= bytes_.size() ? std::span<const std::uint8_t>(bytes_.data(), size)
                                     : std::span<const std::uint8_t>{};
    }

  private:
    std::array<std::uint8_t, inline_capacity> inline_;
    std::vector<std::uint8_t> dynamic_;
    std::span<std::uint8_t> bytes_;
};

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

bool valid_projection(const LinearMemoryProjection& projection,
                      const std::size_t expected_byte_count) noexcept {
    if (!projection || projection.byte_count != expected_byte_count) return false;
    for (std::uint8_t index = 0u; index < projection.byte_count; ++index) {
        if (projection.byte_offsets[index] >= projection.backing->size()) return false;
    }
    return true;
}

std::optional<std::uint32_t>
projected_value(const LinearMemoryProjection& projection,
                const std::size_t expected_byte_count) {
    if (!valid_projection(projection, expected_byte_count)) return std::nullopt;
    std::uint32_t value = 0u;
    for (std::uint8_t index = 0u; index < projection.byte_count; ++index) {
        value |= static_cast<std::uint32_t>(
                     projection.backing->read_u8(projection.byte_offsets[index]))
                 << (static_cast<unsigned>(index) * 8u);
    }
    return value;
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

void MemoryDevice::validate_write(const std::uint32_t,
                                  const std::size_t,
                                  const CodeWriteSource) const {}

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

LinearMemoryProjection
MemoryDevice::linear_projection(const std::uint32_t,
                                const MemoryAccessWidth) const noexcept {
    return {};
}

PreparedDeviceU32Write
MemoryDevice::prepare_prevalidated_u32_write(const std::uint32_t) noexcept {
    return {};
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

LinearMemoryProjection
LinearMemoryDevice::linear_projection(const std::uint32_t offset,
                                      const MemoryAccessWidth width) const noexcept {
    const auto count = width_bytes(width);
    if (count > bytes_.size() || static_cast<std::size_t>(offset) > bytes_.size() - count)
        return {};
    LinearMemoryProjection projection;
    projection.backing = this;
    projection.byte_count = static_cast<std::uint8_t>(count);
    projection.contiguous = true;
    for (std::size_t index = 0u; index < count; ++index)
        projection.byte_offsets[index] = offset + static_cast<std::uint32_t>(index);
    return projection;
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
                                   MmioWriteHandler write_handler,
                                   MmioPrepareU32WriteHandler prepare_u32_write_handler)
    : size_(size), read_handler_(std::move(read_handler)),
      write_handler_(std::move(write_handler)),
      prepare_u32_write_handler_(std::move(prepare_u32_write_handler)) {
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

PreparedDeviceU32Write
MmioMemoryDevice::prepare_prevalidated_u32_write(
    const std::uint32_t offset) noexcept {
    if (!prepare_u32_write_handler_) return {};
    try {
        check(offset, MemoryAccessWidth::Word);
        return prepare_u32_write_handler_(offset);
    } catch (...) {
        return {};
    }
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
    refresh_direct_linear_access_state();
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
                         const MemoryDevice* const device,
                         const bool record_lookup_metrics) const noexcept {
    if (device == nullptr || width == 0u) return false;
    if (width > address_space_size - static_cast<std::uint64_t>(address)) return false;
    const auto end = static_cast<std::uint64_t>(address) + width;
    if (const auto* mapped = indexed_region(address, width, record_lookup_metrics);
        mapped != nullptr) {
        return mapped->device.get() == device;
    }
    for (const auto& region : regions_) {
        if (record_lookup_metrics) ++performance_counters_.reference_region_probes;
        const auto region_end =
            static_cast<std::uint64_t>(region.info.base_address) + region.info.size;
        if (address >= region.info.base_address && end <= region_end) {
            return region.device.get() == device;
        }
    }
    return false;
}

bool Memory::is_writable_linear_range(const std::uint32_t address,
                                      const std::size_t width,
                                      const bool record_lookup_metrics) const noexcept {
    if (width == 0u || width > address_space_size - static_cast<std::uint64_t>(address))
        return false;
    const auto writable_linear = [](const MappedRegion& mapped) {
        return mapped.info.access == MemoryRegionAccess::ReadWrite && mapped.linear != nullptr;
    };
    if (const auto* mapped = indexed_region(address, width, record_lookup_metrics);
        mapped != nullptr)
        return writable_linear(*mapped);
    const auto end = static_cast<std::uint64_t>(address) + width;
    for (const auto& mapped : regions_) {
        if (record_lookup_metrics) ++performance_counters_.reference_region_probes;
        const auto region_end =
            static_cast<std::uint64_t>(mapped.info.base_address) + mapped.info.size;
        if (address >= mapped.info.base_address && end <= region_end)
            return writable_linear(mapped);
    }
    return false;
}

bool Memory::is_readable_linear_range(const std::uint32_t address,
                                      const std::size_t width,
                                      const bool record_lookup_metrics) const noexcept {
    if (width == 0u || width > address_space_size - static_cast<std::uint64_t>(address))
        return false;
    const auto readable_linear = [](const MappedRegion& mapped) {
        return mapped.linear != nullptr;
    };
    if (const auto* mapped = indexed_region(address, width, record_lookup_metrics);
        mapped != nullptr)
        return readable_linear(*mapped);
    const auto end = static_cast<std::uint64_t>(address) + width;
    for (const auto& mapped : regions_) {
        if (record_lookup_metrics) ++performance_counters_.reference_region_probes;
        const auto region_end =
            static_cast<std::uint64_t>(mapped.info.base_address) + mapped.info.size;
        if (address >= mapped.info.base_address && end <= region_end)
            return readable_linear(mapped);
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
    refresh_direct_linear_access_state();
}

const MemoryPerformanceCounters& Memory::performance_counters() const noexcept {
    return performance_counters_;
}

void Memory::reset_performance_counters() const noexcept {
    performance_counters_ = {};
}

bool Memory::account_prevalidated_unobserved_accesses(
    const std::uint64_t accesses,
    const std::uint64_t indexed_region_hits) const noexcept {
    if (lookup_mode_ != MemoryLookupMode::Indexed || access_observers_active() ||
        mmio_access_tracking_enabled_ || mmio_trace_handler_ || guest_memory_access_sink_)
        return false;
    performance_counters_.unobserved_accesses += accesses;
    performance_counters_.indexed_region_hits += indexed_region_hits;
    return true;
}

void Memory::bind_direct_linear_alias_window(const std::uint32_t physical_base,
                                             const std::uint32_t physical_span,
                                             LinearMemoryDevice& backing) {
    const auto backing_size = backing.size();
    if (backing_size == 0u || backing_size > std::numeric_limits<std::uint32_t>::max() ||
        !std::has_single_bit(backing_size)) {
        throw std::invalid_argument(
            "Das direkte lineare Aliasfenster braucht ein Zweierpotenz-Backing.");
    }
    if (physical_span == 0u || physical_span % backing_size != 0u ||
        static_cast<std::uint64_t>(physical_base) + physical_span > address_space_size) {
        throw std::invalid_argument(
            "Das direkte lineare Aliasfenster besitzt eine ungueltige Ausdehnung.");
    }

    for (std::uint64_t alias = physical_base;
         alias < static_cast<std::uint64_t>(physical_base) + physical_span;
         alias += backing_size) {
        const auto alias_end = alias + backing_size;
        const auto mapped = std::find_if(
            regions_.begin(), regions_.end(), [&](const MappedRegion& candidate) {
                const auto region_start =
                    static_cast<std::uint64_t>(candidate.info.base_address);
                const auto region_end = region_start + candidate.info.size;
                return alias >= region_start && alias_end <= region_end;
            });
        if (mapped == regions_.end() || mapped->linear != &backing ||
            mapped->info.access != MemoryRegionAccess::ReadWrite) {
            throw std::invalid_argument(
                "Das direkte lineare Aliasfenster ist nicht vollstaendig auf dasselbe "
                "schreibbare Backing abgebildet.");
        }
    }

    direct_linear_backing_ = &backing;
    direct_linear_bytes_ = backing.writable_bytes().data();
    direct_linear_physical_base_ = physical_base;
    direct_linear_physical_span_ = physical_span;
    direct_linear_backing_mask_ = static_cast<std::uint32_t>(backing_size - 1u);
    refresh_direct_linear_access_state();
}

void Memory::clear_direct_linear_alias_window() noexcept {
    direct_linear_backing_ = nullptr;
    direct_linear_bytes_ = nullptr;
    direct_linear_physical_base_ = 0u;
    direct_linear_physical_span_ = 0u;
    direct_linear_backing_mask_ = 0u;
    refresh_direct_linear_access_state();
}

DirectLinearMemoryGuard
Memory::direct_linear_memory_guard(const bool write) const noexcept {
    if ((write && !direct_linear_writes_enabled_) ||
        (!write && !direct_linear_reads_enabled_))
        return {};
    // A raw writable pointer is deliberately unavailable while code/module
    // invalidation observes guest stores. Such stores use try_write_direct_* so
    // the stable observer still sees the exact bytes_changed edge.
    if (write && guest_write_observer_) return {};
    const auto* const read_bytes = direct_linear_bytes_;
    auto* const write_bytes = write ? direct_linear_bytes_ : nullptr;
    DirectLinearMemoryGuard guard;
    guard.read_bytes = read_bytes;
    guard.write_bytes = write_bytes;
    guard.physical_base = direct_linear_physical_base_;
    guard.physical_span = direct_linear_physical_span_;
    guard.backing_mask = direct_linear_backing_mask_;
    guard.generation = direct_linear_generation_;
    guard.generation_source_ = &direct_linear_generation_;
    guard.performance_counters_ = &performance_counters_;
    return guard;
}

bool Memory::direct_linear_memory_guard_current(
    const DirectLinearMemoryGuard& guard,
    const bool write) const noexcept {
    if ((write && !direct_linear_writes_enabled_) ||
        (!write && !direct_linear_reads_enabled_))
        return false;
    if (guard.generation_source_ != &direct_linear_generation_ ||
        guard.performance_counters_ != &performance_counters_ ||
        guard.generation != direct_linear_generation_ ||
        guard.read_bytes != direct_linear_bytes_ ||
        guard.physical_base != direct_linear_physical_base_ ||
        guard.physical_span != direct_linear_physical_span_ ||
        guard.backing_mask != direct_linear_backing_mask_)
        return false;
    return !write ||
           (guard.write_bytes != nullptr && guard.write_bytes == direct_linear_bytes_ &&
            !guest_write_observer_);
}

bool Memory::direct_linear_offset(const std::uint32_t physical_address,
                                  const std::size_t width,
                                  std::uint32_t& offset) const noexcept {
    if (direct_linear_backing_ == nullptr || direct_linear_bytes_ == nullptr || width == 0u ||
        width > direct_linear_backing_->size() ||
        (physical_address & static_cast<std::uint32_t>(width - 1u)) != 0u)
        return false;
    if (physical_address < direct_linear_physical_base_) return false;
    const auto relative = physical_address - direct_linear_physical_base_;
    if (relative >= direct_linear_physical_span_ ||
        width > direct_linear_physical_span_ - relative)
        return false;
    offset = relative & direct_linear_backing_mask_;
    return width <= direct_linear_backing_->size() - offset;
}

bool Memory::try_read_direct_linear_u8(const std::uint32_t physical_address,
                                       std::uint8_t& value) const noexcept {
    std::uint32_t offset = 0u;
    if (!direct_linear_reads_enabled_ ||
        !direct_linear_offset(physical_address, sizeof(value), offset))
        return false;
    value = direct_linear_bytes_[offset];
    ++performance_counters_.indexed_region_hits;
    ++performance_counters_.unobserved_accesses;
    return true;
}

bool Memory::try_read_direct_linear_u16(const std::uint32_t physical_address,
                                        std::uint16_t& value) const noexcept {
    std::uint32_t offset = 0u;
    if (!direct_linear_reads_enabled_ ||
        !direct_linear_offset(physical_address, sizeof(value), offset))
        return false;
    std::memcpy(&value, direct_linear_bytes_ + offset, sizeof(value));
    value = little_u16(value);
    ++performance_counters_.indexed_region_hits;
    ++performance_counters_.unobserved_accesses;
    return true;
}

bool Memory::try_read_direct_linear_u32(const std::uint32_t physical_address,
                                        std::uint32_t& value) const noexcept {
    std::uint32_t offset = 0u;
    if (!direct_linear_reads_enabled_ ||
        !direct_linear_offset(physical_address, sizeof(value), offset))
        return false;
    std::memcpy(&value, direct_linear_bytes_ + offset, sizeof(value));
    value = little_u32(value);
    ++performance_counters_.indexed_region_hits;
    ++performance_counters_.unobserved_accesses;
    return true;
}

bool Memory::try_write_direct_linear_u8(const std::uint32_t physical_address,
                                        const std::uint8_t value,
                                        const CodeWriteSource source) {
    std::uint32_t offset = 0u;
    if (!direct_linear_writes_enabled_ ||
        !direct_linear_offset(physical_address, sizeof(value), offset))
        return false;
    const bool changed = direct_linear_bytes_[offset] != value;
    direct_linear_bytes_[offset] = value;
    ++performance_counters_.indexed_region_hits;
    ++performance_counters_.unobserved_accesses;
    notify_guest_write({physical_address, sizeof(value), source, changed});
    return true;
}

bool Memory::try_write_direct_linear_u16(const std::uint32_t physical_address,
                                         const std::uint16_t value,
                                         const CodeWriteSource source) {
    std::uint32_t offset = 0u;
    if (!direct_linear_writes_enabled_ ||
        !direct_linear_offset(physical_address, sizeof(value), offset))
        return false;
    std::uint16_t previous = 0u;
    std::memcpy(&previous, direct_linear_bytes_ + offset, sizeof(previous));
    previous = little_u16(previous);
    const auto stored = little_u16(value);
    std::memcpy(direct_linear_bytes_ + offset, &stored, sizeof(stored));
    ++performance_counters_.indexed_region_hits;
    ++performance_counters_.unobserved_accesses;
    notify_guest_write({physical_address, sizeof(value), source, previous != value});
    return true;
}

bool Memory::try_write_direct_linear_u32(const std::uint32_t physical_address,
                                         const std::uint32_t value,
                                         const CodeWriteSource source) {
    std::uint32_t offset = 0u;
    if (!direct_linear_writes_enabled_ ||
        !direct_linear_offset(physical_address, sizeof(value), offset))
        return false;
    std::uint32_t previous = 0u;
    std::memcpy(&previous, direct_linear_bytes_ + offset, sizeof(previous));
    previous = little_u32(previous);
    const auto stored = little_u32(value);
    std::memcpy(direct_linear_bytes_ + offset, &stored, sizeof(stored));
    ++performance_counters_.indexed_region_hits;
    ++performance_counters_.unobserved_accesses;
    notify_guest_write({physical_address, sizeof(value), source, previous != value});
    return true;
}

Memory::DirectLinearWriteBatch::DirectLinearWriteBatch(
    Memory& owner,
    const std::uint64_t direct_generation,
    const std::uint64_t observer_generation) noexcept
    : owner_(&owner),
      direct_generation_(direct_generation),
      observer_generation_(observer_generation) {}

Memory::DirectLinearWriteBatch::DirectLinearWriteBatch(
    DirectLinearWriteBatch&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      entries_(other.entries_),
      events_(other.events_),
      size_(std::exchange(other.size_, 0u)),
      direct_generation_(std::exchange(other.direct_generation_, 0u)),
      observer_generation_(std::exchange(other.observer_generation_, 0u)) {}

Memory::DirectLinearWriteBatch::~DirectLinearWriteBatch() noexcept {
    flush();
}

bool Memory::DirectLinearWriteBatch::try_stage_u8(
    const std::uint32_t physical_address,
    const std::uint8_t value,
    const CodeWriteSource source) noexcept {
    return try_stage(physical_address, value, sizeof(value), source);
}

bool Memory::DirectLinearWriteBatch::try_stage_u16(
    const std::uint32_t physical_address,
    const std::uint16_t value,
    const CodeWriteSource source) noexcept {
    return try_stage(physical_address, value, sizeof(value), source);
}

bool Memory::DirectLinearWriteBatch::try_stage_u32(
    const std::uint32_t physical_address,
    const std::uint32_t value,
    const CodeWriteSource source) noexcept {
    return try_stage(physical_address, value, sizeof(value), source);
}

bool Memory::DirectLinearWriteBatch::try_stage(
    const std::uint32_t physical_address,
    const std::uint32_t value,
    const std::uint8_t width,
    const CodeWriteSource source) noexcept {
    if (owner_ == nullptr || size_ == entries_.size() ||
        !owner_->direct_linear_write_batch_current(*this))
        return false;
    std::uint32_t backing_offset = 0u;
    if (!owner_->direct_linear_offset(physical_address, width, backing_offset))
        return false;

    bool changed = false;
    for (std::uint32_t byte = 0u; byte < width; ++byte) {
        auto previous = owner_->direct_linear_bytes_[backing_offset + byte];
        for (std::size_t index = 0u; index < size_; ++index) {
            const auto& staged = entries_[index];
            if (backing_offset + byte < staged.backing_offset ||
                backing_offset + byte >=
                    staged.backing_offset + staged.width)
                continue;
            const auto staged_byte =
                backing_offset + byte - staged.backing_offset;
            previous = static_cast<std::uint8_t>(
                staged.value >> (staged_byte * 8u));
        }
        const auto next =
            static_cast<std::uint8_t>(value >> (byte * 8u));
        changed = changed || previous != next;
    }

    entries_[size_] = {
        physical_address, backing_offset, value, width, source, changed};
    events_[size_] = {physical_address, width, source, changed};
    ++size_;
    return true;
}

void Memory::DirectLinearWriteBatch::flush() noexcept {
    if (owner_ == nullptr) return;
    auto* const owner = owner_;
    owner->flush_direct_linear_write_batch(*this);
    owner_ = nullptr;
    size_ = 0u;
}

Memory::DirectLinearWriteBatch
Memory::begin_direct_linear_write_batch() noexcept {
    const auto scalar_observer = static_cast<bool>(guest_write_observer_);
    const auto batch_observer = static_cast<bool>(guest_write_batch_observer_);
    if (!direct_linear_writes_enabled_ || mmio_access_tracking_enabled_ ||
        mmio_trace_handler_ || scalar_observer != batch_observer)
        return {};
    return DirectLinearWriteBatch(
        *this, direct_linear_generation_, guest_write_observer_generation_);
}

bool Memory::direct_linear_write_batch_current(
    const DirectLinearWriteBatch& batch) const noexcept {
    const auto scalar_observer = static_cast<bool>(guest_write_observer_);
    const auto batch_observer = static_cast<bool>(guest_write_batch_observer_);
    return batch.owner_ == this &&
           batch.direct_generation_ == direct_linear_generation_ &&
           batch.observer_generation_ == guest_write_observer_generation_ &&
           direct_linear_writes_enabled_ &&
           !mmio_access_tracking_enabled_ && !mmio_trace_handler_ &&
           scalar_observer == batch_observer;
}

void Memory::flush_direct_linear_write_batch(
    DirectLinearWriteBatch& batch) noexcept {
    if (batch.size_ == 0u) return;
    const auto events = std::span<const GuestWriteEvent>(
        batch.events_.data(), batch.size_);
    const auto direct_commit =
        direct_linear_write_batch_current(batch) &&
        (!guest_write_batch_observer_ ||
         guest_write_batch_observer_.admit(
             guest_write_batch_observer_.context, events));

    if (direct_commit) {
        for (std::size_t index = 0u; index < batch.size_; ++index) {
            const auto& entry = batch.entries_[index];
            for (std::uint32_t byte = 0u; byte < entry.width; ++byte) {
                direct_linear_bytes_[entry.backing_offset + byte] =
                    static_cast<std::uint8_t>(
                        entry.value >> (byte * 8u));
            }
        }
        performance_counters_.indexed_region_hits += batch.size_;
        performance_counters_.unobserved_accesses += batch.size_;
        if (guest_write_batch_observer_) {
            guest_write_batch_observer_.commit(
                guest_write_batch_observer_.context, events);
        }
        return;
    }

    // Admission and guard expiry are ordinary fallbacks, not late failures.
    // Replay retains guest-store order, overlapping-store bytes_changed edges,
    // watchpoints/tracing and the scalar observer contract.
    try {
        for (std::size_t index = 0u; index < batch.size_; ++index) {
            const auto& entry = batch.entries_[index];
            bool written = false;
            switch (entry.width) {
            case sizeof(std::uint8_t):
                written = try_write_direct_linear_u8(
                    entry.physical_address,
                    static_cast<std::uint8_t>(entry.value),
                    entry.source);
                if (!written)
                    write_u8(entry.physical_address,
                             static_cast<std::uint8_t>(entry.value),
                             entry.source);
                break;
            case sizeof(std::uint16_t):
                written = try_write_direct_linear_u16(
                    entry.physical_address,
                    static_cast<std::uint16_t>(entry.value),
                    entry.source);
                if (!written)
                    write_u16(entry.physical_address,
                              static_cast<std::uint16_t>(entry.value),
                              entry.source);
                break;
            case sizeof(std::uint32_t):
                written = try_write_direct_linear_u32(
                    entry.physical_address, entry.value, entry.source);
                if (!written)
                    write_u32(
                        entry.physical_address, entry.value, entry.source);
                break;
            default:
                std::terminate();
            }
        }
    } catch (...) {
        // A staged guest store cannot be rolled back safely. Stable product
        // observers are nonthrowing by contract; violating that contract is
        // terminal rather than silently dropping a suffix of the batch.
        std::terminate();
    }
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
    refresh_direct_linear_access_state();
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
    refresh_direct_linear_access_state();
    return true;
}

void Memory::clear_watchpoints() noexcept {
    watchpoints_.clear();
    refresh_direct_linear_access_state();
}

std::size_t Memory::watchpoint_count() const noexcept {
    return watchpoints_.size();
}

void Memory::set_trace_handler(MemoryAccessObserver observer) {
    trace_handler_ = std::move(observer);
    refresh_direct_linear_access_state();
}

void Memory::clear_trace_handler() noexcept {
    trace_handler_ = {};
    refresh_direct_linear_access_state();
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

std::optional<MemoryAccessEvent> Memory::last_mmio_access() const {
    if (!last_mmio_access_) return std::nullopt;
    const auto region =
        std::find_if(regions_.begin(), regions_.end(), [&](const MappedRegion& mapped) {
            return mapped.info.base_address == last_mmio_access_->region_base_address;
        });
    return MemoryAccessEvent{
        last_mmio_access_->operation,
        last_mmio_access_->address,
        last_mmio_access_->width,
        last_mmio_access_->value,
        region == regions_.end() ? std::string{} : region->info.name,
    };
}

void Memory::clear_last_mmio_access() const noexcept {
    last_mmio_access_.reset();
}

void Memory::attach_crash_capsule(CrashCapsule& capsule) noexcept {
    crash_capsule_ = &capsule;
}

void Memory::detach_crash_capsule(const CrashCapsule& capsule) noexcept {
    if (crash_capsule_ == &capsule) crash_capsule_ = nullptr;
}

std::uint64_t Memory::mmio_access_epoch() const noexcept {
    return 0u;
}

std::uint64_t Memory::mmio_boundary_epoch() const noexcept {
    return mmio_boundary_epoch_;
}

void Memory::set_mmio_interrupt_state_sink(const MmioInterruptStateSink sink) noexcept {
    mmio_interrupt_state_sink_ = sink;
}

void Memory::clear_mmio_interrupt_state_sink() noexcept {
    mmio_interrupt_state_sink_ = {};
}

void Memory::notify_interrupt_source_state_maybe_changed() const noexcept {
    if (mmio_interrupt_state_sink_)
        mmio_interrupt_state_sink_.mark_dirty(mmio_interrupt_state_sink_.context);
}

void Memory::set_mmio_trace_handler(MemoryAccessObserver observer) {
    mmio_trace_handler_ = std::move(observer);
}

void Memory::clear_mmio_trace_handler() noexcept {
    mmio_trace_handler_ = {};
}

bool Memory::has_mmio_trace_handler() const noexcept {
    return static_cast<bool>(mmio_trace_handler_);
}

void Memory::set_guest_write_observer(GuestWriteObserver observer,
                                      const GuestWriteObserverContract contract) {
    // A batch observer's raw context is tied to the scalar observer contract.
    // Replacing either half invalidates the pair.
    guest_write_batch_observer_ = {};
    guest_write_observer_ = std::move(observer);
    guest_write_observer_contract_ =
        guest_write_observer_ ? contract : GuestWriteObserverContract::General;
    ++guest_write_observer_generation_;
    if (guest_write_observer_generation_ == 0u)
        ++guest_write_observer_generation_;
    refresh_direct_linear_access_state();
}

void Memory::clear_guest_write_observer() noexcept {
    guest_write_batch_observer_ = {};
    guest_write_observer_ = {};
    guest_write_observer_contract_ = GuestWriteObserverContract::General;
    ++guest_write_observer_generation_;
    if (guest_write_observer_generation_ == 0u)
        ++guest_write_observer_generation_;
    refresh_direct_linear_access_state();
}

bool Memory::has_guest_write_observer() const noexcept {
    return static_cast<bool>(guest_write_observer_);
}

bool Memory::guest_write_observer_allows_prevalidated_linear_writes() const noexcept {
    return !guest_write_observer_ ||
           guest_write_observer_contract_ ==
               GuestWriteObserverContract::StableForPrevalidatedLinearWrites;
}

void Memory::set_guest_write_batch_observer(
    const GuestWriteBatchObserver observer) noexcept {
    guest_write_batch_observer_ =
        observer ? observer : GuestWriteBatchObserver{};
    ++guest_write_observer_generation_;
    if (guest_write_observer_generation_ == 0u)
        ++guest_write_observer_generation_;
    refresh_direct_linear_access_state();
}

void Memory::clear_guest_write_batch_observer() noexcept {
    guest_write_batch_observer_ = {};
    ++guest_write_observer_generation_;
    if (guest_write_observer_generation_ == 0u)
        ++guest_write_observer_generation_;
    refresh_direct_linear_access_state();
}

void Memory::set_guest_memory_access_sink(const GuestMemoryAccessSink sink) noexcept {
    guest_memory_access_sink_ = sink;
    refresh_direct_linear_access_state();
}

void Memory::clear_guest_memory_access_sink() noexcept {
    guest_memory_access_sink_ = {};
    refresh_direct_linear_access_state();
}

GuestMemoryAccessSink Memory::guest_memory_access_sink() const noexcept {
    return guest_memory_access_sink_;
}

void Memory::notify_external_guest_memory_access(
    const GuestMemoryAccessEvent& event) const noexcept {
    if (guest_memory_access_sink_) {
        guest_memory_access_sink_.callback(guest_memory_access_sink_.context, event);
    }
}

std::uint8_t Memory::read_u8(const std::uint32_t address) const {
    return read_u8_at(address, GuestMemoryAccessContext{address});
}

std::uint8_t Memory::read_u8_at(const std::uint32_t address,
                                const GuestMemoryAccessContext& context) const {
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
    if (guest_memory_access_sink_) {
        notify_guest_memory_access(mapped,
                                   MemoryAccessOperation::Read,
                                   address,
                                   value,
                                   MemoryAccessWidth::Byte,
                                   1u,
                                   CodeWriteSource::Cpu,
                                   true,
                                   false,
                                   &context);
    }
    return value;
}

std::uint16_t Memory::read_u16(const std::uint32_t address) const {
    return read_u16_at(address, GuestMemoryAccessContext{address});
}

std::uint16_t Memory::read_u16_at(const std::uint32_t address,
                                  const GuestMemoryAccessContext& context) const {
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
    if (guest_memory_access_sink_) {
        notify_guest_memory_access(mapped,
                                   MemoryAccessOperation::Read,
                                   address,
                                   value,
                                   MemoryAccessWidth::Halfword,
                                   2u,
                                   CodeWriteSource::Cpu,
                                   true,
                                   false,
                                   &context);
    }
    return value;
}

std::uint32_t Memory::read_u32(const std::uint32_t address) const {
    return read_u32_at(address, GuestMemoryAccessContext{address});
}

std::uint32_t Memory::read_u32_at(const std::uint32_t address,
                                  const GuestMemoryAccessContext& context) const {
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
    if (guest_memory_access_sink_) {
        notify_guest_memory_access(mapped,
                                   MemoryAccessOperation::Read,
                                   address,
                                   value,
                                   MemoryAccessWidth::Word,
                                   4u,
                                   CodeWriteSource::Cpu,
                                   true,
                                   false,
                                   &context);
    }
    return value;
}

std::uint32_t
Memory::peek_u32(const std::uint32_t address,
                 const std::span<const MemoryDevice* const> permitted_devices) const {
    const auto& mapped =
        resolve(address, MemoryAccessWidth::Word, MemoryAccessOperation::Read, false);
    const auto offset = region_offset(mapped.info, address);
    const auto projection = mapped.device->linear_projection(offset, MemoryAccessWidth::Word);
    if (!projection ||
        std::find(permitted_devices.begin(), permitted_devices.end(), projection.backing) ==
            permitted_devices.end())
        throw MemoryAccessError(MemoryAccessErrorReason::Unmapped,
                                MemoryAccessOperation::Read,
                                address,
                                MemoryAccessWidth::Word,
                                "diagnostic-peek-denied");
    const auto value = projected_value(projection, sizeof(std::uint32_t));
    if (!value)
        throw MemoryAccessError(MemoryAccessErrorReason::Unmapped,
                                MemoryAccessOperation::Read,
                                address,
                                MemoryAccessWidth::Word,
                                "diagnostic-peek-invalid-projection");
    return *value;
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
    write_u8_at(address, value, GuestMemoryAccessContext{address}, source);
}

void Memory::write_u8_at(const std::uint32_t address,
                         const std::uint8_t value,
                         const GuestMemoryAccessContext& context,
                         const CodeWriteSource source) {
    const auto& mapped = resolve_writable(address, MemoryAccessWidth::Byte);
    const auto offset = region_offset(mapped.info, address);
    mmio_boundary(mapped.info,
                  address,
                  MemoryAccessWidth::Byte,
                  MemoryAccessOperation::Write,
                  [&] {
                      mapped.device->validate_write(
                          offset, sizeof(value), source);
                  });
    bool observer_changed = true;
    bool sink_changed = true;
    if (mapped.linear != nullptr &&
        (guest_write_observer_ || guest_memory_access_sink_)) {
        const bool exact_changed = mapped.linear->read_u8(offset) != value;
        observer_changed = exact_changed;
        sink_changed = exact_changed;
    } else if (guest_memory_access_sink_) {
        const auto projection =
            mapped.device->linear_projection(offset, MemoryAccessWidth::Byte);
        const auto previous = projected_value(projection, sizeof(std::uint8_t));
        sink_changed = !previous || *previous != value;
    }
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
    notify_guest_write({address, 1u, source, observer_changed});
    if (guest_memory_access_sink_) {
        notify_guest_memory_access(mapped,
                                   MemoryAccessOperation::Write,
                                   address,
                                   value,
                                   MemoryAccessWidth::Byte,
                                   1u,
                                   source,
                                   true,
                                   sink_changed,
                                   &context);
    }
}

void Memory::write_u16(const std::uint32_t address,
                       const std::uint16_t value,
                       const CodeWriteSource source) {
    write_u16_at(address, value, GuestMemoryAccessContext{address}, source);
}

void Memory::write_u16_at(const std::uint32_t address,
                          const std::uint16_t value,
                          const GuestMemoryAccessContext& context,
                          const CodeWriteSource source) {
    const auto& mapped = resolve_writable(address, MemoryAccessWidth::Halfword);
    const auto offset = region_offset(mapped.info, address);
    mmio_boundary(mapped.info,
                  address,
                  MemoryAccessWidth::Halfword,
                  MemoryAccessOperation::Write,
                  [&] {
                      mapped.device->validate_write(
                          offset, sizeof(value), source);
                  });
    bool observer_changed = true;
    bool sink_changed = true;
    if (mapped.linear != nullptr &&
        (guest_write_observer_ || guest_memory_access_sink_)) {
        const bool exact_changed = mapped.linear->read_u16(offset) != value;
        observer_changed = exact_changed;
        sink_changed = exact_changed;
    } else if (guest_memory_access_sink_) {
        const auto projection =
            mapped.device->linear_projection(offset, MemoryAccessWidth::Halfword);
        const auto previous = projected_value(projection, sizeof(std::uint16_t));
        sink_changed = !previous || *previous != value;
    }
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
    notify_guest_write({address, 2u, source, observer_changed});
    if (guest_memory_access_sink_) {
        notify_guest_memory_access(mapped,
                                   MemoryAccessOperation::Write,
                                   address,
                                   value,
                                   MemoryAccessWidth::Halfword,
                                   2u,
                                   source,
                                   true,
                                   sink_changed,
                                   &context);
    }
}

void Memory::write_u32(const std::uint32_t address,
                       const std::uint32_t value,
                       const CodeWriteSource source) {
    write_u32_at(address, value, GuestMemoryAccessContext{address}, source);
}

void Memory::write_u32_at(const std::uint32_t address,
                          const std::uint32_t value,
                          const GuestMemoryAccessContext& context,
                          const CodeWriteSource source) {
    const auto& mapped = resolve_writable(address, MemoryAccessWidth::Word);
    const auto offset = region_offset(mapped.info, address);
    mmio_boundary(mapped.info,
                  address,
                  MemoryAccessWidth::Word,
                  MemoryAccessOperation::Write,
                  [&] {
                      mapped.device->validate_write(
                          offset, sizeof(value), source);
                  });
    bool observer_changed = true;
    bool sink_changed = true;
    if (mapped.linear != nullptr &&
        (guest_write_observer_ || guest_memory_access_sink_)) {
        const bool exact_changed = mapped.linear->read_u32(offset) != value;
        observer_changed = exact_changed;
        sink_changed = exact_changed;
    } else if (guest_memory_access_sink_) {
        const auto projection =
            mapped.device->linear_projection(offset, MemoryAccessWidth::Word);
        const auto previous = projected_value(projection, sizeof(std::uint32_t));
        sink_changed = !previous || *previous != value;
    }
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
    notify_guest_write({address, 4u, source, observer_changed});
    if (guest_memory_access_sink_) {
        notify_guest_memory_access(mapped,
                                   MemoryAccessOperation::Write,
                                   address,
                                   value,
                                   MemoryAccessWidth::Word,
                                   4u,
                                   source,
                                   true,
                                   sink_changed,
                                   &context);
    }
}

void Memory::write_bytes(const std::uint32_t address,
                         const std::span<const std::uint8_t> bytes,
                         const CodeWriteSource source) {
    write_bytes_at(address, bytes, GuestMemoryAccessContext{address}, source);
}

bool Memory::commit_linear_transaction_bytes(
    const std::uint32_t address,
    const std::span<const std::uint8_t> bytes,
    const CodeWriteSource source) noexcept {
    const LinearMemoryTransactionWrite write{address, bytes};
    return commit_linear_transaction_batch(
        std::span<const LinearMemoryTransactionWrite>(&write, 1u), source);
}

struct Memory::PreparedLinearTransactionBatch::Data {
    struct Write {
        std::uint32_t address = 0u;
        std::vector<std::uint8_t> bytes;
        std::shared_ptr<MemoryDevice> device;
        LinearMemoryDevice* linear = nullptr;
        std::size_t offset = 0u;
        std::string region_name;
        std::vector<std::uint8_t> changed_bytes;
        bool changed = false;
    };

    Memory* owner = nullptr;
    CodeWriteSource source = CodeWriteSource::Copy;
    GuestWriteObserver observer;
    std::uint64_t observer_generation = 0u;
    std::vector<Write> writes;
    std::vector<GuestWriteEvent> guest_write_events;
};

Memory::PreparedLinearTransactionBatch::PreparedLinearTransactionBatch()
    : data_(std::make_unique<Data>()) {}

Memory::PreparedLinearTransactionBatch::PreparedLinearTransactionBatch(
    PreparedLinearTransactionBatch&&) noexcept = default;

Memory::PreparedLinearTransactionBatch&
Memory::PreparedLinearTransactionBatch::operator=(
    PreparedLinearTransactionBatch&&) noexcept = default;

Memory::PreparedLinearTransactionBatch::~PreparedLinearTransactionBatch() =
    default;

std::span<const GuestWriteEvent>
Memory::PreparedLinearTransactionBatch::guest_write_events() const noexcept {
    return data_ ? std::span<const GuestWriteEvent>(
                       data_->guest_write_events)
                 : std::span<const GuestWriteEvent>{};
}

void Memory::PreparedLinearTransactionBatch::
    suppress_guest_write_observer() noexcept {
    if (!data_) std::terminate();
    data_->observer = {};
}

std::optional<Memory::PreparedLinearTransactionBatch>
Memory::prepare_linear_transaction_batch(
    const std::span<const LinearMemoryTransactionWrite> writes,
    const CodeWriteSource source) noexcept {
    if (access_observers_active() || mmio_access_tracking_enabled_ ||
        mmio_trace_handler_ || guest_memory_access_sink_ ||
        !guest_write_observer_allows_prevalidated_linear_writes())
        return std::nullopt;
    try {
        PreparedLinearTransactionBatch prepared;
        prepared.data_->owner = this;
        prepared.data_->source = source;
        prepared.data_->observer = guest_write_observer_;
        prepared.data_->observer_generation =
            guest_write_observer_generation_;
        prepared.data_->writes.reserve(writes.size());
        prepared.data_->guest_write_events.reserve(writes.size());
        for (const auto& write : writes) {
            if (write.bytes.empty()) continue;
            if (write.bytes.size() >
                address_space_size - static_cast<std::uint64_t>(write.address))
                return std::nullopt;
            const auto& mapped = resolve(write.address,
                                         MemoryAccessWidth::Byte,
                                         MemoryAccessOperation::Write,
                                         false);
            const auto mapped_end =
                static_cast<std::uint64_t>(mapped.info.base_address) +
                mapped.info.size;
            const auto write_end =
                static_cast<std::uint64_t>(write.address) + write.bytes.size();
            if (write_end > mapped_end ||
                mapped.info.access != MemoryRegionAccess::ReadWrite ||
                mapped.linear == nullptr)
                return std::nullopt;
            const auto offset = static_cast<std::size_t>(
                region_offset(mapped.info, write.address));
            const auto backing = mapped.linear->bytes();
            if (offset > backing.size() ||
                write.bytes.size() > backing.size() - offset)
                return std::nullopt;

            PreparedLinearTransactionBatch::Data::Write item;
            item.address = write.address;
            item.bytes.assign(write.bytes.begin(), write.bytes.end());
            item.device = mapped.device;
            item.linear = mapped.linear;
            item.offset = offset;
            item.region_name = mapped.info.name;
            item.changed_bytes.assign(backing.begin() + static_cast<std::ptrdiff_t>(offset),
                                      backing.begin() +
                                          static_cast<std::ptrdiff_t>(
                                              offset + write.bytes.size()));
            for (const auto& previous : prepared.data_->writes) {
                if (previous.linear != item.linear) continue;
                const auto overlap_begin = std::max(item.offset, previous.offset);
                const auto overlap_end =
                    std::min(item.offset + item.bytes.size(),
                             previous.offset + previous.bytes.size());
                if (overlap_begin >= overlap_end) continue;
                std::copy(previous.bytes.begin() +
                              static_cast<std::ptrdiff_t>(
                                  overlap_begin - previous.offset),
                          previous.bytes.begin() +
                              static_cast<std::ptrdiff_t>(
                                  overlap_end - previous.offset),
                          item.changed_bytes.begin() +
                              static_cast<std::ptrdiff_t>(
                                  overlap_begin - item.offset));
            }
            for (std::size_t index = 0u; index < write.bytes.size(); ++index) {
                item.changed_bytes[index] =
                    item.changed_bytes[index] != item.bytes[index] ? 1u : 0u;
                item.changed = item.changed ||
                               item.changed_bytes[index] != 0u;
            }
            prepared.data_->guest_write_events.push_back(
                {item.address,
                 item.bytes.size(),
                 source,
                 item.changed});
            prepared.data_->writes.push_back(std::move(item));
        }
        return prepared;
    } catch (...) {
        return std::nullopt;
    }
}

void Memory::commit_prepared_linear_transaction_batch(
    PreparedLinearTransactionBatch prepared) noexcept {
    if (!prepared.data_ || prepared.data_->owner != this ||
        prepared.data_->observer_generation !=
            guest_write_observer_generation_)
        std::terminate();

    // No callbacks are made before every admitted range is visible. Keeping
    // each backing device alive also makes subsequent diagnostic callbacks
    // free to alter the map without invalidating the remainder.
    for (const auto& write : prepared.data_->writes) {
        auto backing = write.linear->writable_bytes();
        std::copy(
            write.bytes.begin(),
            write.bytes.end(),
            backing.begin() +
                static_cast<std::ptrdiff_t>(write.offset));
    }

    for (std::size_t index = 0u;
         index < prepared.data_->writes.size();
         ++index) {
        const auto& write = prepared.data_->writes[index];
        performance_counters_.unobserved_accesses +=
            write.bytes.size();
        if (prepared.data_->observer) {
            try {
                prepared.data_->observer(
                    prepared.data_->guest_write_events[index]);
            } catch (...) {
                // Product provenance is part of the committed transaction.
                // Continuing after a partial observer update is unsound.
                std::terminate();
            }
        }
    }
}

bool Memory::commit_linear_transaction_batch(
    const std::span<const LinearMemoryTransactionWrite> writes,
    const CodeWriteSource source) noexcept {
    struct PreparedWrite {
        std::uint32_t address = 0u;
        std::vector<std::uint8_t> bytes;
        std::shared_ptr<MemoryDevice> device;
        LinearMemoryDevice* linear = nullptr;
        std::size_t offset = 0u;
        std::string region_name;
        std::vector<std::uint8_t> changed_bytes;
        bool changed = false;
    };

    try {
        std::vector<PreparedWrite> prepared;
        prepared.reserve(writes.size());
        for (const auto& write : writes) {
            if (write.bytes.empty()) continue;
            if (write.bytes.size() >
                address_space_size -
                    static_cast<std::uint64_t>(write.address))
                return false;
            const auto& mapped = resolve(
                write.address,
                MemoryAccessWidth::Byte,
                MemoryAccessOperation::Write,
                false);
            const auto mapped_end =
                static_cast<std::uint64_t>(
                    mapped.info.base_address) +
                mapped.info.size;
            const auto write_end =
                static_cast<std::uint64_t>(write.address) +
                write.bytes.size();
            if (write_end > mapped_end ||
                mapped.info.access != MemoryRegionAccess::ReadWrite ||
                mapped.linear == nullptr)
                return false;
            const auto offset = static_cast<std::size_t>(
                region_offset(mapped.info, write.address));
            const auto backing = mapped.linear->bytes();
            if (offset > backing.size() ||
                write.bytes.size() > backing.size() - offset)
                return false;

            PreparedWrite item;
            item.address = write.address;
            item.bytes.assign(
                write.bytes.begin(), write.bytes.end());
            item.device = mapped.device;
            item.linear = mapped.linear;
            item.offset = offset;
            item.region_name = mapped.info.name;
            item.changed_bytes.assign(
                backing.begin() +
                    static_cast<std::ptrdiff_t>(offset),
                backing.begin() +
                    static_cast<std::ptrdiff_t>(
                        offset + write.bytes.size()));
            for (const auto& previous : prepared) {
                if (previous.linear != item.linear) continue;
                const auto overlap_begin =
                    std::max(item.offset, previous.offset);
                const auto overlap_end =
                    std::min(
                        item.offset + item.bytes.size(),
                        previous.offset + previous.bytes.size());
                if (overlap_begin >= overlap_end) continue;
                std::copy(
                    previous.bytes.begin() +
                        static_cast<std::ptrdiff_t>(
                            overlap_begin - previous.offset),
                    previous.bytes.begin() +
                        static_cast<std::ptrdiff_t>(
                            overlap_end - previous.offset),
                    item.changed_bytes.begin() +
                        static_cast<std::ptrdiff_t>(
                            overlap_begin - item.offset));
            }
            for (std::size_t index = 0u;
                 index < write.bytes.size();
                 ++index) {
                item.changed_bytes[index] =
                    item.changed_bytes[index] !=
                            item.bytes[index]
                        ? 1u
                        : 0u;
                item.changed =
                    item.changed ||
                    item.changed_bytes[index] != 0u;
            }
            prepared.push_back(std::move(item));
        }

        for (const auto& write : prepared) {
            auto backing = write.linear->writable_bytes();
            std::copy(
                write.bytes.begin(),
                write.bytes.end(),
                backing.begin() +
                    static_cast<std::ptrdiff_t>(write.offset));
        }

        for (const auto& write : prepared) {
            if (access_observers_active()) {
                for (std::size_t index = 0u;
                     index < write.bytes.size();
                     ++index) {
                    ++performance_counters_.observed_accesses;
                    try {
                        notify_access(
                            {MemoryAccessOperation::Write,
                             write.address +
                                 static_cast<std::uint32_t>(index),
                             MemoryAccessWidth::Byte,
                             write.bytes[index],
                             write.region_name});
                    } catch (...) {
                    }
                }
            } else {
                performance_counters_.unobserved_accesses +=
                    write.bytes.size();
            }
            try {
                notify_guest_write(
                    {write.address,
                     write.bytes.size(),
                     source,
                     write.changed});
            } catch (...) {
            }
            notify_guest_memory_write_range(
                write.address,
                write.bytes.size(),
                source,
                write.changed_bytes,
                nullptr);
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<Memory::PreparedLinearFill>
Memory::prepare_prevalidated_linear_fill(
    const std::uint32_t address,
    const std::size_t size,
    const std::uint8_t value,
    const CodeWriteSource source,
    const std::size_t additional_unobserved_accesses,
    const std::size_t additional_indexed_region_hits) noexcept {
    if (size == 0u || lookup_mode_ != MemoryLookupMode::Indexed ||
        access_observers_active() || mmio_access_tracking_enabled_ ||
        mmio_trace_handler_ || guest_memory_access_sink_ ||
        !guest_write_observer_allows_prevalidated_linear_writes())
        return std::nullopt;
    const auto* const mapped = prevalidated_writable_linear_region(address, size);
    if (mapped == nullptr) return std::nullopt;

    const auto offset = static_cast<std::size_t>(region_offset(mapped->info, address));
    const auto backing = mapped->linear->bytes();
    PreparedLinearFill prepared;
    try {
        prepared.observer_ = guest_write_observer_;
        if (prepared.observer_) {
            prepared.changed_bytes_.reserve(size);
            for (std::size_t index = 0u; index < size; ++index) {
                prepared.changed_bytes_.push_back(
                    static_cast<std::uint8_t>(backing[offset + index] != value));
            }
        }
    } catch (...) {
        return std::nullopt;
    }

    prepared.owner_ = this;
    prepared.device_lifetime_ = mapped->device;
    prepared.linear_ = mapped->linear;
    prepared.offset_ = offset;
    prepared.address_ = address;
    prepared.size_ = size;
    prepared.value_ = value;
    prepared.source_ = source;
    prepared.additional_unobserved_accesses_ = additional_unobserved_accesses;
    prepared.additional_indexed_region_hits_ = additional_indexed_region_hits;
    return prepared;
}

void Memory::commit_prepared_linear_fill(PreparedLinearFill prepared) noexcept {
    auto backing = prepared.linear_->writable_bytes();
    std::fill_n(backing.data() + static_cast<std::ptrdiff_t>(prepared.offset_),
                prepared.size_,
                prepared.value_);
    prepared.owner_->performance_counters_.unobserved_accesses += prepared.size_;
    prepared.owner_->performance_counters_.unobserved_accesses +=
        prepared.additional_unobserved_accesses_;
    prepared.owner_->performance_counters_.indexed_region_hits +=
        prepared.additional_indexed_region_hits_;

    if (prepared.observer_) {
        for (std::size_t index = 0u; index < prepared.changed_bytes_.size(); ++index) {
            try {
                prepared.observer_(
                    {prepared.address_ + static_cast<std::uint32_t>(index),
                     1u,
                     prepared.source_,
                     prepared.changed_bytes_[index] != 0u});
            } catch (...) {
                // The RAM commit is already globally visible. Resuming the guest after only a
                // prefix of product-observer updates would make executable provenance unsound.
                std::terminate();
            }
        }
    }
}

bool Memory::commit_prevalidated_linear_fill(
    const std::uint32_t address,
    const std::size_t size,
    const std::uint8_t value,
    const CodeWriteSource source,
    const std::size_t additional_unobserved_accesses,
    const std::size_t additional_indexed_region_hits) noexcept {
    auto prepared = prepare_prevalidated_linear_fill(address,
                                                     size,
                                                     value,
                                                     source,
                                                     additional_unobserved_accesses,
                                                     additional_indexed_region_hits);
    if (!prepared) return false;
    commit_prepared_linear_fill(std::move(*prepared));
    return true;
}

std::optional<Memory::PreparedLinearU32Pattern>
Memory::prepare_prevalidated_linear_u32_pattern(
    const std::uint32_t address,
    const std::size_t word_count,
    const std::uint32_t value,
    const CodeWriteSource source,
    const std::size_t additional_unobserved_accesses,
    const std::size_t additional_indexed_region_hits) noexcept {
    constexpr std::size_t word_size = sizeof(std::uint32_t);
    if (word_count == 0u || word_count > std::numeric_limits<std::size_t>::max() / word_size ||
        (address & (word_size - 1u)) != 0u || lookup_mode_ != MemoryLookupMode::Indexed ||
        access_observers_active() || mmio_access_tracking_enabled_ || mmio_trace_handler_ ||
        guest_memory_access_sink_ || !guest_write_observer_allows_prevalidated_linear_writes())
        return std::nullopt;

    const auto size = word_count * word_size;
    const auto* const mapped = prevalidated_writable_region(address, size);
    if (mapped == nullptr) return std::nullopt;

    const auto offset = static_cast<std::size_t>(region_offset(mapped->info, address));
    PreparedLinearU32Pattern prepared;
    try {
        prepared.observer_ = guest_write_observer_;
        if (prepared.observer_) prepared.changed_words_.reserve(word_count);
        if (mapped->linear != nullptr) {
            const auto backing = mapped->linear->bytes();
            if (prepared.observer_) {
                const auto word_changed = [&](const std::size_t word) {
                    const auto byte = offset + word * word_size;
                    return backing[byte] != static_cast<std::uint8_t>(value) ||
                           backing[byte + 1u] != static_cast<std::uint8_t>(value >> 8u) ||
                           backing[byte + 2u] != static_cast<std::uint8_t>(value >> 16u) ||
                           backing[byte + 3u] != static_cast<std::uint8_t>(value >> 24u);
                };
                for (std::size_t word = 0u; word < word_count; ++word) {
                    prepared.changed_words_.push_back(
                        static_cast<std::uint8_t>(word_changed(word)));
                }
            }
        } else {
            // The Dreamcast 32-bit VRAM aperture is word-projected rather than one
            // contiguous host span.  Validate every scalar store and projection before
            // guest time is accepted; commit then uses the same device path as write_u32.
            for (std::size_t word = 0u; word < word_count; ++word) {
                const auto word_offset = offset + word * word_size;
                if (word_offset > std::numeric_limits<std::uint32_t>::max())
                    return std::nullopt;
                mapped->device->validate_write(
                    static_cast<std::uint32_t>(word_offset), word_size, source);
                const auto projection = mapped->device->linear_projection(
                    static_cast<std::uint32_t>(word_offset), MemoryAccessWidth::Word);
                if (!projection || !projection.contiguous ||
                    projection.byte_count != word_size || projection.backing == nullptr)
                    return std::nullopt;
                const auto projected_offset =
                    static_cast<std::size_t>(projection.byte_offsets.front());
                const auto backing = projection.backing->bytes();
                if (projected_offset > backing.size() ||
                    word_size > backing.size() - projected_offset ||
                    projection.byte_offsets[1u] != projected_offset + 1u ||
                    projection.byte_offsets[2u] != projected_offset + 2u ||
                    projection.byte_offsets[3u] != projected_offset + 3u)
                    return std::nullopt;
                if (prepared.observer_) {
                    prepared.changed_words_.push_back(static_cast<std::uint8_t>(
                        backing[projected_offset] != static_cast<std::uint8_t>(value) ||
                        backing[projected_offset + 1u] !=
                            static_cast<std::uint8_t>(value >> 8u) ||
                        backing[projected_offset + 2u] !=
                            static_cast<std::uint8_t>(value >> 16u) ||
                        backing[projected_offset + 3u] !=
                            static_cast<std::uint8_t>(value >> 24u)));
                }
            }
        }
    } catch (...) {
        return std::nullopt;
    }

    prepared.owner_ = this;
    prepared.device_lifetime_ = mapped->device;
    prepared.linear_ = mapped->linear;
    prepared.offset_ = offset;
    prepared.address_ = address;
    prepared.word_count_ = word_count;
    prepared.value_ = value;
    prepared.source_ = source;
    prepared.additional_unobserved_accesses_ = additional_unobserved_accesses;
    prepared.additional_indexed_region_hits_ = additional_indexed_region_hits;
    return prepared;
}

void Memory::commit_prepared_linear_u32_pattern(
    PreparedLinearU32Pattern prepared) noexcept {
    constexpr std::size_t word_size = sizeof(std::uint32_t);
    if (prepared.linear_ != nullptr) {
        const std::array pattern{
            static_cast<std::uint8_t>(prepared.value_),
            static_cast<std::uint8_t>(prepared.value_ >> 8u),
            static_cast<std::uint8_t>(prepared.value_ >> 16u),
            static_cast<std::uint8_t>(prepared.value_ >> 24u),
        };
        auto backing = prepared.linear_->writable_bytes();
        auto* const target =
            backing.data() + static_cast<std::ptrdiff_t>(prepared.offset_);
        for (std::size_t word = 0u; word < prepared.word_count_; ++word) {
            std::copy(pattern.begin(),
                      pattern.end(),
                      target + static_cast<std::ptrdiff_t>(word * word_size));
        }
    } else {
        for (std::size_t word = 0u; word < prepared.word_count_; ++word) {
            prepared.device_lifetime_->write_u32(
                static_cast<std::uint32_t>(prepared.offset_ + word * word_size),
                prepared.value_);
        }
    }

    prepared.owner_->performance_counters_.unobserved_accesses +=
        prepared.word_count_;
    prepared.owner_->performance_counters_.unobserved_accesses +=
        prepared.additional_unobserved_accesses_;
    prepared.owner_->performance_counters_.indexed_region_hits +=
        prepared.additional_indexed_region_hits_;

    if (prepared.observer_) {
        for (std::size_t word = 0u; word < prepared.word_count_; ++word) {
            try {
                const auto byte_offset = word * word_size;
                prepared.observer_(
                    {prepared.address_ + static_cast<std::uint32_t>(byte_offset),
                     word_size,
                     prepared.source_,
                     prepared.changed_words_[word] != 0u});
            } catch (...) {
                // The complete RAM commit is already visible. Partial provenance updates may
                // never be followed by resumed guest execution.
                std::terminate();
            }
        }
    }
}

bool Memory::commit_prevalidated_linear_u32_pattern(
    const std::uint32_t address,
    const std::size_t word_count,
    const std::uint32_t value,
    const CodeWriteSource source,
    const std::size_t additional_unobserved_accesses,
    const std::size_t additional_indexed_region_hits) noexcept {
    auto prepared = prepare_prevalidated_linear_u32_pattern(address,
                                                            word_count,
                                                            value,
                                                            source,
                                                            additional_unobserved_accesses,
                                                            additional_indexed_region_hits);
    if (!prepared) return false;
    commit_prepared_linear_u32_pattern(std::move(*prepared));
    return true;
}

std::optional<Memory::PreparedLinearU32Copy>
Memory::prepare_prevalidated_linear_u32_copy(
    const std::uint32_t address,
    const std::span<const std::uint8_t> bytes,
    const CodeWriteSource source,
    const std::size_t additional_unobserved_accesses,
    const std::size_t additional_indexed_region_hits) noexcept {
    constexpr std::size_t word_size = sizeof(std::uint32_t);
    if (bytes.empty() || (bytes.size() % word_size) != 0u ||
        (address & (word_size - 1u)) != 0u ||
        lookup_mode_ != MemoryLookupMode::Indexed || access_observers_active() ||
        mmio_access_tracking_enabled_ || mmio_trace_handler_ ||
        guest_memory_access_sink_ ||
        !guest_write_observer_allows_prevalidated_linear_writes())
        return std::nullopt;

    const auto* const mapped = prevalidated_writable_region(address, bytes.size());
    if (mapped == nullptr) return std::nullopt;

    const auto offset =
        static_cast<std::size_t>(region_offset(mapped->info, address));
    const auto word_count = bytes.size() / word_size;
    PreparedLinearU32Copy prepared;
    try {
        prepared.bytes_.assign(bytes.begin(), bytes.end());
        prepared.observer_ = guest_write_observer_;
        if (prepared.observer_) prepared.changed_words_.reserve(word_count);
        if (mapped->linear != nullptr) {
            const auto backing = mapped->linear->bytes();
            if (offset > backing.size() ||
                prepared.bytes_.size() > backing.size() - offset)
                return std::nullopt;
            if (prepared.observer_) {
                for (std::size_t word = 0u; word < word_count; ++word) {
                    const auto byte = word * word_size;
                    prepared.changed_words_.push_back(
                        static_cast<std::uint8_t>(std::memcmp(
                            backing.data() +
                                static_cast<std::ptrdiff_t>(offset + byte),
                            prepared.bytes_.data() +
                                static_cast<std::ptrdiff_t>(byte),
                            word_size) != 0));
                }
            }
        } else {
            // Word-projected devices do not expose one contiguous host span.
            // Prove every scalar store and exact four-byte projection before
            // the caller accepts any guest time.
            for (std::size_t word = 0u; word < word_count; ++word) {
                const auto word_offset = offset + word * word_size;
                if (word_offset > std::numeric_limits<std::uint32_t>::max())
                    return std::nullopt;
                mapped->device->validate_write(
                    static_cast<std::uint32_t>(word_offset), word_size, source);
                const auto projection = mapped->device->linear_projection(
                    static_cast<std::uint32_t>(word_offset),
                    MemoryAccessWidth::Word);
                if (!projection || !projection.contiguous ||
                    projection.byte_count != word_size ||
                    projection.backing == nullptr)
                    return std::nullopt;
                const auto projected_offset =
                    static_cast<std::size_t>(projection.byte_offsets.front());
                const auto backing = projection.backing->bytes();
                if (projected_offset > backing.size() ||
                    word_size > backing.size() - projected_offset ||
                    projection.byte_offsets[1u] != projected_offset + 1u ||
                    projection.byte_offsets[2u] != projected_offset + 2u ||
                    projection.byte_offsets[3u] != projected_offset + 3u)
                    return std::nullopt;
                if (prepared.observer_) {
                    const auto byte = word * word_size;
                    prepared.changed_words_.push_back(
                        static_cast<std::uint8_t>(std::memcmp(
                            backing.data() + static_cast<std::ptrdiff_t>(
                                                 projected_offset),
                            prepared.bytes_.data() +
                                static_cast<std::ptrdiff_t>(byte),
                            word_size) != 0));
                }
            }
        }
    } catch (...) {
        return std::nullopt;
    }

    prepared.owner_ = this;
    prepared.device_lifetime_ = mapped->device;
    prepared.linear_ = mapped->linear;
    prepared.offset_ = offset;
    prepared.address_ = address;
    prepared.source_ = source;
    prepared.additional_unobserved_accesses_ =
        additional_unobserved_accesses;
    prepared.additional_indexed_region_hits_ =
        additional_indexed_region_hits;
    return prepared;
}

void Memory::commit_prepared_linear_u32_copy(
    PreparedLinearU32Copy prepared) noexcept {
    constexpr std::size_t word_size = sizeof(std::uint32_t);
    if (prepared.owner_ != this || prepared.bytes_.empty() ||
        (prepared.bytes_.size() % word_size) != 0u)
        std::terminate();
    const auto word_count = prepared.bytes_.size() / word_size;
    if (prepared.linear_ != nullptr) {
        auto backing = prepared.linear_->writable_bytes();
        std::copy(prepared.bytes_.begin(),
                  prepared.bytes_.end(),
                  backing.begin() +
                      static_cast<std::ptrdiff_t>(prepared.offset_));
    } else {
        for (std::size_t word = 0u; word < word_count; ++word) {
            const auto byte = word * word_size;
            const auto value =
                static_cast<std::uint32_t>(prepared.bytes_[byte]) |
                (static_cast<std::uint32_t>(prepared.bytes_[byte + 1u]) << 8u) |
                (static_cast<std::uint32_t>(prepared.bytes_[byte + 2u]) << 16u) |
                (static_cast<std::uint32_t>(prepared.bytes_[byte + 3u]) << 24u);
            prepared.device_lifetime_->write_u32(
                static_cast<std::uint32_t>(prepared.offset_ + byte), value);
        }
    }

    performance_counters_.unobserved_accesses += word_count;
    performance_counters_.unobserved_accesses +=
        prepared.additional_unobserved_accesses_;
    performance_counters_.indexed_region_hits +=
        prepared.additional_indexed_region_hits_;

    if (prepared.observer_) {
        for (std::size_t word = 0u; word < word_count; ++word) {
            try {
                prepared.observer_(
                    {prepared.address_ +
                         static_cast<std::uint32_t>(word * word_size),
                     word_size,
                     prepared.source_,
                     prepared.changed_words_[word] != 0u});
            } catch (...) {
                // All stores are already visible. Partial invalidation must
                // never be followed by resumed guest execution.
                std::terminate();
            }
        }
    }
}

bool Memory::commit_prevalidated_linear_u32_copy(
    const std::uint32_t address,
    const std::span<const std::uint8_t> bytes,
    const CodeWriteSource source,
    const std::size_t additional_unobserved_accesses,
    const std::size_t additional_indexed_region_hits) noexcept {
    auto prepared = prepare_prevalidated_linear_u32_copy(
        address,
        bytes,
        source,
        additional_unobserved_accesses,
        additional_indexed_region_hits);
    if (!prepared) return false;
    commit_prepared_linear_u32_copy(std::move(*prepared));
    return true;
}

std::optional<Memory::PreparedRepeatedU32Sequence>
Memory::prepare_prevalidated_repeated_u32_sequence(
    const std::uint32_t address,
    const std::size_t word_count,
    const std::uint32_t first_value,
    const std::uint32_t step,
    const CodeWriteSource source,
    const std::size_t additional_unobserved_accesses,
    const std::size_t additional_indexed_region_hits) noexcept {
    constexpr std::size_t word_size = sizeof(std::uint32_t);
    if (word_count == 0u || (address & (word_size - 1u)) != 0u ||
        lookup_mode_ != MemoryLookupMode::Indexed || access_observers_active() ||
        mmio_access_tracking_enabled_ || mmio_trace_handler_ || guest_memory_access_sink_ ||
        !guest_write_observer_allows_prevalidated_linear_writes())
        return std::nullopt;
    const auto* const mapped = prevalidated_writable_region(address, word_size);
    if (mapped == nullptr) return std::nullopt;

    const auto offset = static_cast<std::size_t>(region_offset(mapped->info, address));
    PreparedRepeatedU32Sequence prepared;
    if (mapped->linear == nullptr) {
        prepared.device_write_ = mapped->device->prepare_prevalidated_u32_write(
            static_cast<std::uint32_t>(offset));
        if (!prepared.device_write_) return std::nullopt;
    }
    std::uint32_t final_value = first_value;
    try {
        prepared.observer_ = guest_write_observer_;
        if (prepared.observer_) {
            prepared.changed_words_.reserve(word_count);
            auto next = first_value;
            if (mapped->linear != nullptr) {
                const auto backing = mapped->linear->bytes();
                std::uint32_t previous =
                    static_cast<std::uint32_t>(backing[offset]) |
                    (static_cast<std::uint32_t>(backing[offset + 1u]) << 8u) |
                    (static_cast<std::uint32_t>(backing[offset + 2u]) << 16u) |
                    (static_cast<std::uint32_t>(backing[offset + 3u]) << 24u);
                for (std::size_t word = 0u; word < word_count; ++word) {
                    prepared.changed_words_.push_back(
                        static_cast<std::uint8_t>(previous != next));
                    previous = next;
                    final_value = next;
                    next += step;
                }
            } else {
                // Scalar non-linear writes conservatively report bytes_changed=true.
                prepared.changed_words_.assign(word_count, std::uint8_t{1u});
                final_value = first_value +
                    static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(word_count - 1u) * step);
            }
        } else {
            final_value = first_value +
                static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(word_count - 1u) * step);
        }
    } catch (...) {
        return std::nullopt;
    }

    prepared.owner_ = this;
    prepared.device_lifetime_ = mapped->device;
    prepared.linear_ = mapped->linear;
    prepared.offset_ = offset;
    prepared.address_ = address;
    prepared.word_count_ = word_count;
    prepared.final_value_ = final_value;
    prepared.source_ = source;
    prepared.additional_unobserved_accesses_ = additional_unobserved_accesses;
    prepared.additional_indexed_region_hits_ = additional_indexed_region_hits;
    return prepared;
}

void Memory::commit_prepared_repeated_u32_sequence(
    PreparedRepeatedU32Sequence prepared) noexcept {
    constexpr std::size_t word_size = sizeof(std::uint32_t);
    if (prepared.linear_ != nullptr) {
        auto backing = prepared.linear_->writable_bytes();
        auto* const target =
            backing.data() + static_cast<std::ptrdiff_t>(prepared.offset_);
        target[0u] = static_cast<std::uint8_t>(prepared.final_value_);
        target[1u] = static_cast<std::uint8_t>(prepared.final_value_ >> 8u);
        target[2u] = static_cast<std::uint8_t>(prepared.final_value_ >> 16u);
        target[3u] = static_cast<std::uint8_t>(prepared.final_value_ >> 24u);
    } else {
        prepared.device_write_.commit(prepared.final_value_);
        prepared.owner_->notify_interrupt_source_state_maybe_changed();
    }
    prepared.owner_->performance_counters_.unobserved_accesses +=
        prepared.word_count_;
    prepared.owner_->performance_counters_.unobserved_accesses +=
        prepared.additional_unobserved_accesses_;
    prepared.owner_->performance_counters_.indexed_region_hits +=
        prepared.additional_indexed_region_hits_;

    if (prepared.observer_) {
        for (std::size_t word = 0u; word < prepared.word_count_; ++word) {
            try {
                prepared.observer_(
                    {prepared.address_,
                     word_size,
                     prepared.source_,
                     prepared.changed_words_[word] != 0u});
            } catch (...) {
                std::terminate();
            }
        }
    }
}

bool Memory::commit_prevalidated_repeated_u32_sequence(
    const std::uint32_t address,
    const std::size_t word_count,
    const std::uint32_t first_value,
    const std::uint32_t step,
    const CodeWriteSource source,
    const std::size_t additional_unobserved_accesses,
    const std::size_t additional_indexed_region_hits) noexcept {
    auto prepared = prepare_prevalidated_repeated_u32_sequence(
        address,
        word_count,
        first_value,
        step,
        source,
        additional_unobserved_accesses,
        additional_indexed_region_hits);
    if (!prepared) return false;
    commit_prepared_repeated_u32_sequence(std::move(*prepared));
    return true;
}

bool Memory::commit_prevalidated_linear_transaction_bytes(
    const std::uint32_t address,
    const std::span<const std::uint8_t> bytes,
    const std::span<const std::uint8_t> changed_bytes,
    const CodeWriteSource source) noexcept {
    if (bytes.empty() || changed_bytes.size() != bytes.size()) return false;
    const auto* mapped = indexed_region(address, bytes.size(), false);
    if (mapped == nullptr || mapped->info.access != MemoryRegionAccess::ReadWrite ||
        mapped->linear == nullptr)
        return false;
    const auto offset = region_offset(mapped->info, address);
    auto backing = mapped->linear->writable_bytes();
    if (offset > backing.size() || bytes.size() > backing.size() - offset) return false;

    std::copy(bytes.begin(), bytes.end(), backing.begin() + offset);
    const auto changed = std::any_of(
        changed_bytes.begin(), changed_bytes.end(), [](const auto value) { return value != 0u; });
    if (access_observers_active()) {
        for (std::size_t index = 0u; index < bytes.size(); ++index) {
            ++performance_counters_.observed_accesses;
            try {
                notify_access({MemoryAccessOperation::Write,
                               address + static_cast<std::uint32_t>(index),
                               MemoryAccessWidth::Byte,
                               bytes[index],
                               mapped->info.name});
            } catch (...) {
                // Diagnostic observers are not allowed to tear an admitted guest transaction.
            }
        }
    } else {
        performance_counters_.unobserved_accesses += bytes.size();
    }
    try {
        notify_guest_write({address, bytes.size(), source, changed});
    } catch (...) {
        // The product observer only consumes the admitted range. A diagnostic observer failure
        // cannot turn a completed linear RAM commit into a partial device write.
    }
    notify_guest_memory_write_range(
        address, bytes.size(), source, changed_bytes, nullptr);
    return true;
}

void Memory::write_bytes_at(const std::uint32_t address,
                            const std::span<const std::uint8_t> bytes,
                            const GuestMemoryAccessContext& context,
                            const CodeWriteSource source) {
    write_bytes_at_impl(address, bytes, context, source, false);
}

void Memory::write_dma_words(const std::uint32_t address,
                             const std::span<const std::uint8_t> bytes,
                             const CodeWriteSource source) {
    write_bytes_at_impl(
        address, bytes, GuestMemoryAccessContext{address}, source, true);
}

void Memory::write_bytes_at_impl(
    const std::uint32_t address,
    const std::span<const std::uint8_t> bytes,
    const GuestMemoryAccessContext& context,
    const CodeWriteSource source,
    const bool prefer_device_word_writes) {
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
        bool observer_changed = true;
    };

    std::vector<PendingByteWrite> pending;
    pending.reserve(bytes.size());
    std::optional<DiagnosticChangedBytes> diagnostic_changed_bytes;
    if (guest_memory_access_sink_)
        diagnostic_changed_bytes.emplace(bytes.size());
    bool changed = false;
    for (std::size_t index = 0u; index < bytes.size(); ++index) {
        const auto current = address + static_cast<std::uint32_t>(index);
        const auto& mapped = resolve_writable(current, MemoryAccessWidth::Byte);
        const auto offset = region_offset(mapped.info, current);
        const bool comparable = mapped.linear != nullptr;
        const std::uint8_t previous =
            comparable ? mapped.linear->read_u8(offset) : std::uint8_t{0u};
        const bool observer_byte_changed = !comparable || previous != bytes[index];
        bool sink_byte_changed = observer_byte_changed;
        if (guest_memory_access_sink_ && !comparable) {
            const auto projection =
                mapped.device->linear_projection(offset, MemoryAccessWidth::Byte);
            const auto projected_previous =
                projected_value(projection, sizeof(std::uint8_t));
            sink_byte_changed =
                !projected_previous || *projected_previous != bytes[index];
        }
        pending.push_back({&mapped, offset, observer_byte_changed});
        if (diagnostic_changed_bytes)
            diagnostic_changed_bytes->set(index, sink_byte_changed);
        changed = changed || observer_byte_changed;
    }

    for (std::size_t begin = 0u; begin < pending.size();) {
        const auto* const mapped = pending[begin].mapped;
        auto end = begin + 1u;
        while (end < pending.size() &&
               pending[end].mapped == mapped &&
               pending[end].offset ==
                   pending[begin].offset +
                       static_cast<std::uint32_t>(end - begin))
            ++end;
        mmio_boundary(mapped->info,
                      address + static_cast<std::uint32_t>(begin),
                      MemoryAccessWidth::Byte,
                      MemoryAccessOperation::Write,
                      [&] {
                          mapped->device->validate_write(
                              pending[begin].offset,
                              end - begin,
                              source);
                      });
        begin = end;
    }

    std::size_t committed = 0u;
    try {
        while (committed < bytes.size()) {
            const auto index = committed;
            const auto current = address + static_cast<std::uint32_t>(index);
            const auto& write = pending[index];
            constexpr std::size_t word_size = sizeof(std::uint32_t);
            const bool word_write =
                prefer_device_word_writes &&
                (current & static_cast<std::uint32_t>(word_size - 1u)) == 0u &&
                word_size <= bytes.size() - committed &&
                pending[index + 1u].mapped == write.mapped &&
                pending[index + 2u].mapped == write.mapped &&
                pending[index + 3u].mapped == write.mapped &&
                pending[index + 1u].offset == write.offset + 1u &&
                pending[index + 2u].offset == write.offset + 2u &&
                pending[index + 3u].offset == write.offset + 3u;
            const auto commit_size = word_write ? word_size : std::size_t{1u};
            if (word_write) {
                const auto value =
                    static_cast<std::uint32_t>(bytes[index]) |
                    (static_cast<std::uint32_t>(bytes[index + 1u]) << 8u) |
                    (static_cast<std::uint32_t>(bytes[index + 2u]) << 16u) |
                    (static_cast<std::uint32_t>(bytes[index + 3u]) << 24u);
                if (write.mapped->linear != nullptr)
                    write.mapped->linear->write_u32(write.offset, value);
                else
                    mmio_boundary(write.mapped->info,
                                  current,
                                  MemoryAccessWidth::Word,
                                  MemoryAccessOperation::Write,
                                  [&] {
                                      write.mapped->device->write_u32(
                                          write.offset, value);
                                  });
            } else if (write.mapped->linear != nullptr) {
                write.mapped->linear->write_u8(write.offset, bytes[index]);
            } else {
                mmio_boundary(write.mapped->info,
                              current,
                              MemoryAccessWidth::Byte,
                              MemoryAccessOperation::Write,
                              [&] {
                                  write.mapped->device->write_u8(
                                      write.offset, bytes[index]);
                              });
            }
            if (write.mapped->mmio) notify_interrupt_source_state_maybe_changed();
            committed += commit_size;
            if (access_observers_active()) {
                performance_counters_.observed_accesses += commit_size;
                for (std::size_t byte = 0u; byte < commit_size; ++byte) {
                    notify_access(MemoryAccessEvent{
                        MemoryAccessOperation::Write,
                        current + static_cast<std::uint32_t>(byte),
                        MemoryAccessWidth::Byte,
                        bytes[index + byte],
                        write.mapped->info.name});
                }
            } else {
                performance_counters_.unobserved_accesses += commit_size;
            }
        }
    } catch (...) {
        if (committed != 0u) {
            bool committed_changed = false;
            for (std::size_t index = 0u; index < committed; ++index) {
                committed_changed =
                    committed_changed || pending[index].observer_changed;
            }
            notify_guest_write({address, committed, source, committed_changed});
            if (guest_memory_access_sink_) {
                if (diagnostic_changed_bytes &&
                    diagnostic_changed_bytes->available(bytes.size()))
                    notify_guest_memory_write_range(
                        address,
                        committed,
                        source,
                        diagnostic_changed_bytes->first(committed),
                        &context);
                else
                    notify_guest_memory_access_loss(&context);
            }
        }
        throw;
    }
    notify_guest_write({address, bytes.size(), source, changed});
    if (guest_memory_access_sink_) {
        if (diagnostic_changed_bytes &&
            diagnostic_changed_bytes->available(bytes.size()))
            notify_guest_memory_write_range(
                address,
                bytes.size(),
                source,
                diagnostic_changed_bytes->first(bytes.size()),
                &context);
        else
            notify_guest_memory_access_loss(&context);
    }
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
        std::optional<DiagnosticChangedBytes> diagnostic_changed_bytes;
        if (guest_memory_access_sink_) diagnostic_changed_bytes.emplace(size);
        bool changed = true;
        if (diagnostic_changed_bytes &&
            diagnostic_changed_bytes->available(size)) {
            changed = false;
            for (std::size_t index = 0u; index < size; ++index) {
                const bool byte_changed =
                    destination_bytes[destination_offset + index] !=
                    source_bytes[source_offset + index];
                diagnostic_changed_bytes->set(index, byte_changed);
                changed = changed || byte_changed;
            }
        } else if (guest_write_observer_) {
            changed = std::memcmp(destination_bytes.data() + destination_offset,
                                  source_bytes.data() + source_offset,
                                  size) != 0;
        }
        std::memmove(destination_bytes.data() + destination_offset,
                     source_bytes.data() + source_offset,
                     size);
        performance_counters_.unobserved_accesses += static_cast<std::uint64_t>(size) * 2u;
        notify_guest_write({destination, size, source, changed});
        if (guest_memory_access_sink_) {
            if (diagnostic_changed_bytes &&
                diagnostic_changed_bytes->available(size))
                notify_guest_memory_write_range(
                    destination,
                    size,
                    source,
                    diagnostic_changed_bytes->first(size));
            else
                notify_guest_memory_access_loss();
        }
        return;
    }

    std::vector<std::uint8_t> payload(size);
    for (std::size_t index = 0u; index < size; ++index)
        payload[index] = read_u8(source_address + static_cast<std::uint32_t>(index));
    write_bytes(destination, payload, source);
}

const Memory::MappedRegion& Memory::resolve(const std::uint32_t address,
                                            const MemoryAccessWidth width,
                                            const MemoryAccessOperation operation,
                                            const bool record_lookup_metrics) const {
    require_alignment(address, width, operation);

    const auto access_size = width_bytes(width);
    const std::uint64_t start = address;
    if (access_size > address_space_size - start) {
        throw MemoryAccessError(
            MemoryAccessErrorReason::AddressOverflow, operation, address, width);
    }
    const std::uint64_t end = start + access_size;

    if (const auto* mapped = indexed_region(address, access_size, record_lookup_metrics);
        mapped != nullptr) {
        return *mapped;
    }

    for (const auto& mapped : regions_) {
        if (record_lookup_metrics) ++performance_counters_.reference_region_probes;
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
                                                   const std::size_t width,
                                                   const bool record_lookup_metrics) const noexcept {
    if (lookup_mode_ != MemoryLookupMode::Indexed || width == 0u) return nullptr;
    const auto end = static_cast<std::uint64_t>(address) + width;
    if (end > address_space_size) return nullptr;

    const auto slot = region_page_index_[address >> region_page_shift];
    if (slot < 0) return nullptr;
    const auto& mapped = regions_[static_cast<std::size_t>(slot)];
    const auto region_start = static_cast<std::uint64_t>(mapped.info.base_address);
    const auto region_end = region_start + mapped.info.size;
    if (address < region_start || end > region_end) return nullptr;
    if (record_lookup_metrics) ++performance_counters_.indexed_region_hits;
    return &mapped;
}

const Memory::MappedRegion*
Memory::prevalidated_writable_region(const std::uint32_t address,
                                     const std::size_t size) const noexcept {
    if (lookup_mode_ != MemoryLookupMode::Indexed || size == 0u ||
        size > address_space_size - static_cast<std::uint64_t>(address))
        return nullptr;

    const auto range_end = static_cast<std::uint64_t>(address) + size;
    const MappedRegion* mapped = indexed_region(address, size, false);
    if (mapped == nullptr) {
        for (const auto& candidate : regions_) {
            const auto region_start =
                static_cast<std::uint64_t>(candidate.info.base_address);
            const auto region_end = region_start + candidate.info.size;
            if (address < region_start || range_end > region_end) continue;
            if (mapped != nullptr) return nullptr;
            mapped = &candidate;
        }
    }
    if (mapped == nullptr ||
        mapped->info.access != MemoryRegionAccess::ReadWrite)
        return nullptr;

    const auto offset =
        static_cast<std::size_t>(region_offset(mapped->info, address));
    if (offset > mapped->device->size() ||
        size > mapped->device->size() - offset)
        return nullptr;
    return mapped;
}

const Memory::MappedRegion*
Memory::prevalidated_writable_linear_region(const std::uint32_t address,
                                            const std::size_t size) const noexcept {
    const auto* const mapped = prevalidated_writable_region(address, size);
    if (mapped == nullptr || mapped->linear == nullptr) return nullptr;

    const auto offset =
        static_cast<std::size_t>(region_offset(mapped->info, address));
    const auto backing = mapped->linear->bytes();
    if (offset > backing.size() || size > backing.size() - offset)
        return nullptr;
    return mapped;
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

void Memory::refresh_direct_linear_access_state() noexcept {
    ++direct_linear_generation_;
    if (direct_linear_generation_ == 0u) ++direct_linear_generation_;

    const bool common =
        direct_linear_backing_ != nullptr && direct_linear_bytes_ != nullptr &&
        direct_linear_physical_span_ != 0u && lookup_mode_ == MemoryLookupMode::Indexed &&
        !access_observers_active() && !guest_memory_access_sink_;
    direct_linear_reads_enabled_ = common;
    direct_linear_writes_enabled_ =
        common && guest_write_observer_allows_prevalidated_linear_writes();
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
                                const std::uint32_t value) const noexcept {
    if (!mapped.mmio) return;
    ++mmio_boundary_epoch_;
    if (mmio_boundary_epoch_ == 0u) ++mmio_boundary_epoch_;
    notify_interrupt_source_state_maybe_changed();
    if (crash_capsule_ != nullptr) {
        crash_capsule_->note_mmio(static_cast<std::uint8_t>(operation),
                                  static_cast<std::uint8_t>(width),
                                  address,
                                  value);
    }
    if (mmio_access_tracking_enabled_) {
        last_mmio_access_ =
            LastMmioAccessRecord{operation, address, width, value, mapped.info.base_address};
    }
    if (mmio_trace_handler_) {
        try {
            mmio_trace_handler_({operation, address, width, value, mapped.info.name});
        } catch (...) {
        }
    }
}

void Memory::notify_guest_write(const GuestWriteEvent& event) const {
    if (guest_write_observer_) guest_write_observer_(event);
}

void Memory::notify_guest_memory_access(const MappedRegion& mapped,
                                        const MemoryAccessOperation operation,
                                        const std::uint32_t physical_address,
                                        const std::uint32_t value,
                                        const MemoryAccessWidth width,
                                        const std::size_t size,
                                        const CodeWriteSource source,
                                        const bool scalar_value_valid,
                                        const bool bytes_changed,
                                        const GuestMemoryAccessContext* context) const noexcept {
    if (!guest_memory_access_sink_) return;

    GuestMemoryAccessEvent event;
    event.operation = operation;
    event.access_origin =
        context == nullptr ? GuestMemoryAccessOrigin::Memory : context->access_origin;
    if (context != nullptr) {
        event.instruction = context->instruction;
        event.virtual_address = context->virtual_address;
        event.retired_guest_instructions = context->retired_guest_instructions;
        event.attempted_guest_instructions = context->attempted_guest_instructions;
    } else {
        event.virtual_address = physical_address;
    }
    event.physical_address = physical_address;
    event.width = width;
    event.value = value;
    event.size = size;
    event.write_source = source;
    event.scalar_value_valid = scalar_value_valid;
    event.bytes_changed = bytes_changed;

    const auto offset = region_offset(mapped.info, physical_address);
    const auto projection = mapped.device->linear_projection(offset, width);
    if (valid_projection(projection, width_bytes(width))) {
        event.linear_backing = projection.backing;
        event.linear_contiguous = projection.contiguous;
        event.linear_byte_offsets = projection.byte_offsets;
        event.linear_byte_count = projection.byte_count;
        event.linear_offset = projection.byte_offsets.front();
        event.linear_size = projection.contiguous ? projection.byte_count : 0u;
    }

    guest_memory_access_sink_.callback(guest_memory_access_sink_.context, event);
}

void Memory::notify_guest_memory_write_range(const std::uint32_t address,
                                             const std::size_t size,
                                             const CodeWriteSource source,
                                             const std::span<const std::uint8_t> changed_bytes,
                                             const GuestMemoryAccessContext* context) const noexcept {
    if (!guest_memory_access_sink_ || size == 0u || changed_bytes.size() < size) return;

    std::size_t emitted = 0u;
    while (emitted < size) {
        const auto current = address + static_cast<std::uint32_t>(emitted);
        try {
            const auto& mapped =
                resolve(current,
                        MemoryAccessWidth::Byte,
                        MemoryAccessOperation::Write,
                        false);
            const auto remaining_in_region =
                mapped.info.size - static_cast<std::size_t>(region_offset(mapped.info, current));
            auto chunk = std::min(size - emitted, remaining_in_region);
            const auto projection =
                mapped.device->linear_projection(region_offset(mapped.info, current),
                                                 MemoryAccessWidth::Byte);
            const bool projection_valid =
                valid_projection(projection, sizeof(std::uint8_t));
            if (projection_valid && mapped.linear == nullptr) chunk = 1u;
            const bool chunk_changed = changed_bytes[emitted] != 0u;
            for (std::size_t index = 1u; index < chunk; ++index) {
                if ((changed_bytes[emitted + index] != 0u) != chunk_changed) {
                    chunk = index;
                    break;
                }
            }

            GuestMemoryAccessEvent event;
            event.operation = MemoryAccessOperation::Write;
            event.access_origin =
                context == nullptr ? GuestMemoryAccessOrigin::Memory : context->access_origin;
            if (context != nullptr) {
                event.instruction = context->instruction;
                event.virtual_address =
                    context->virtual_address + static_cast<std::uint32_t>(emitted);
                event.retired_guest_instructions = context->retired_guest_instructions;
                event.attempted_guest_instructions =
                    context->attempted_guest_instructions;
            } else {
                event.virtual_address = current;
            }
            event.physical_address = current;
            event.width = MemoryAccessWidth::Byte;
            event.size = chunk;
            event.write_source = source;
            event.scalar_value_valid = false;
            event.bytes_changed = chunk_changed;

            if (projection_valid) {
                event.linear_backing = projection.backing;
                event.linear_offset = projection.byte_offsets.front();
                event.linear_size = mapped.linear != nullptr ? chunk : 1u;
                event.linear_contiguous = projection.contiguous;
                event.linear_byte_offsets.front() = projection.byte_offsets.front();
                event.linear_byte_count = 1u;
            }
            guest_memory_access_sink_.callback(guest_memory_access_sink_.context, event);
            emitted += chunk;
        } catch (...) {
            return;
        }
    }
}

void Memory::notify_guest_memory_access_loss(
    const GuestMemoryAccessContext* const context) const noexcept {
    if (!guest_memory_access_sink_) return;
    GuestMemoryAccessEvent invalid_event;
    if (context != nullptr) {
        invalid_event.access_origin = context->access_origin;
        invalid_event.instruction = context->instruction;
        invalid_event.virtual_address = context->virtual_address;
        invalid_event.retired_guest_instructions =
            context->retired_guest_instructions;
        invalid_event.attempted_guest_instructions =
            context->attempted_guest_instructions;
    }
    guest_memory_access_sink_.callback(
        guest_memory_access_sink_.context, invalid_event);
}

} // namespace katana::runtime

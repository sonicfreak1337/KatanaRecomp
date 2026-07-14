#include "katana/runtime/memory.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace katana::runtime {
namespace {

constexpr std::uint64_t address_space_size = 0x100000000ull;

std::string hex_address(const std::uint32_t address) {
    std::ostringstream output;
    output
        << "0x"
        << std::hex
        << std::uppercase
        << std::setw(8)
        << std::setfill('0')
        << address;
    return output.str();
}

std::uint32_t region_offset(
    const MemoryRegionInfo& info,
    const std::uint32_t address
) {
    return address - info.base_address;
}

std::size_t width_bytes(const MemoryAccessWidth width) {
    return static_cast<std::size_t>(width);
}

void require_device_access(
    const std::size_t device_size,
    const std::uint32_t offset,
    const MemoryAccessWidth width
) {
    const auto access_size = width_bytes(width);
    const auto start = static_cast<std::size_t>(offset);
    if (access_size > device_size || start > device_size - access_size) {
        throw std::out_of_range(
            "Speichergeraetezugriff ausserhalb der Region."
        );
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

} // namespace

std::uint16_t MemoryDevice::read_u16(const std::uint32_t offset) const {
    require_device_access(size(), offset, MemoryAccessWidth::Halfword);
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(read_u8(offset)) |
        (static_cast<std::uint16_t>(read_u8(offset + 1u)) << 8u)
    );
}

std::uint32_t MemoryDevice::read_u32(const std::uint32_t offset) const {
    require_device_access(size(), offset, MemoryAccessWidth::Word);
    return
        static_cast<std::uint32_t>(read_u8(offset)) |
        (static_cast<std::uint32_t>(read_u8(offset + 1u)) << 8u) |
        (static_cast<std::uint32_t>(read_u8(offset + 2u)) << 16u) |
        (static_cast<std::uint32_t>(read_u8(offset + 3u)) << 24u);
}

void MemoryDevice::write_u16(
    const std::uint32_t offset,
    const std::uint16_t value
) {
    require_device_access(size(), offset, MemoryAccessWidth::Halfword);
    write_u8(offset, static_cast<std::uint8_t>(value & 0xFFu));
    write_u8(
        offset + 1u,
        static_cast<std::uint8_t>((value >> 8u) & 0xFFu)
    );
}

void MemoryDevice::write_u32(
    const std::uint32_t offset,
    const std::uint32_t value
) {
    require_device_access(size(), offset, MemoryAccessWidth::Word);
    write_u8(offset, static_cast<std::uint8_t>(value & 0xFFu));
    write_u8(
        offset + 1u,
        static_cast<std::uint8_t>((value >> 8u) & 0xFFu)
    );
    write_u8(
        offset + 2u,
        static_cast<std::uint8_t>((value >> 16u) & 0xFFu)
    );
    write_u8(
        offset + 3u,
        static_cast<std::uint8_t>((value >> 24u) & 0xFFu)
    );
}

LinearMemoryDevice::LinearMemoryDevice(const std::size_t size)
    : bytes_(size, 0u) {
    if (size == 0u) {
        throw std::invalid_argument(
            "Eine lineare Speicherregion darf nicht leer sein."
        );
    }
}

std::size_t LinearMemoryDevice::size() const noexcept {
    return bytes_.size();
}

std::uint8_t LinearMemoryDevice::read_u8(
    const std::uint32_t offset
) const {
    check(offset);
    return bytes_[static_cast<std::size_t>(offset)];
}

void LinearMemoryDevice::write_u8(
    const std::uint32_t offset,
    const std::uint8_t value
) {
    check(offset);
    bytes_[static_cast<std::size_t>(offset)] = value;
}

void LinearMemoryDevice::check(const std::uint32_t offset) const {
    if (static_cast<std::size_t>(offset) >= bytes_.size()) {
        throw std::out_of_range(
            "Speichergeraetezugriff ausserhalb der Region."
        );
    }
}

MmioMemoryDevice::MmioMemoryDevice(
    const std::size_t size,
    MmioReadHandler read_handler,
    MmioWriteHandler write_handler
)
    : size_(size),
      read_handler_(std::move(read_handler)),
      write_handler_(std::move(write_handler)) {
    if (size_ == 0u) {
        throw std::invalid_argument(
            "Eine MMIO-Region darf nicht leer sein."
        );
    }
    if (!read_handler_ && !write_handler_) {
        throw std::invalid_argument(
            "Eine MMIO-Region braucht mindestens einen Handler."
        );
    }
}

std::size_t MmioMemoryDevice::size() const noexcept {
    return size_;
}

std::uint8_t MmioMemoryDevice::read_u8(
    const std::uint32_t offset
) const {
    return static_cast<std::uint8_t>(
        read(offset, MemoryAccessWidth::Byte)
    );
}

std::uint16_t MmioMemoryDevice::read_u16(
    const std::uint32_t offset
) const {
    return static_cast<std::uint16_t>(
        read(offset, MemoryAccessWidth::Halfword)
    );
}

std::uint32_t MmioMemoryDevice::read_u32(
    const std::uint32_t offset
) const {
    return read(offset, MemoryAccessWidth::Word);
}

void MmioMemoryDevice::write_u8(
    const std::uint32_t offset,
    const std::uint8_t value
) {
    write(offset, value, MemoryAccessWidth::Byte);
}

void MmioMemoryDevice::write_u16(
    const std::uint32_t offset,
    const std::uint16_t value
) {
    write(offset, value, MemoryAccessWidth::Halfword);
}

void MmioMemoryDevice::write_u32(
    const std::uint32_t offset,
    const std::uint32_t value
) {
    write(offset, value, MemoryAccessWidth::Word);
}

void MmioMemoryDevice::check(
    const std::uint32_t offset,
    const MemoryAccessWidth width
) const {
    require_device_access(size_, offset, width);
}

std::uint32_t MmioMemoryDevice::read(
    const std::uint32_t offset,
    const MemoryAccessWidth width
) const {
    check(offset, width);
    if (!read_handler_) {
        throw std::runtime_error(
            "MMIO-Lesezugriff ohne registrierten Lesehandler."
        );
    }
    return read_handler_(offset, width) & width_mask(width);
}

void MmioMemoryDevice::write(
    const std::uint32_t offset,
    const std::uint32_t value,
    const MemoryAccessWidth width
) {
    check(offset, width);
    if (!write_handler_) {
        throw std::runtime_error(
            "MMIO-Schreibzugriff ohne registrierten Schreibhandler."
        );
    }
    write_handler_(offset, value & width_mask(width), width);
}

Memory::Memory(const std::size_t legacy_size) {
    if (legacy_size != 0u) {
        map_region(
            "legacy-linear-memory",
            0u,
            std::make_shared<LinearMemoryDevice>(legacy_size)
        );
    }
}

void Memory::map_region(
    std::string name,
    const std::uint32_t base_address,
    std::shared_ptr<MemoryDevice> device,
    const MemoryRegionAccess access
) {
    if (name.empty()) {
        throw std::invalid_argument(
            "Eine Speicherregion braucht einen Namen."
        );
    }
    if (!device) {
        throw std::invalid_argument(
            "Eine Speicherregion braucht ein Speichergeraet."
        );
    }
    if (device->size() == 0u) {
        throw std::invalid_argument(
            "Eine Speicherregion darf nicht leer sein."
        );
    }

    const std::uint64_t start = base_address;
    if (device->size() > address_space_size - start) {
        throw std::invalid_argument(
            "Eine Speicherregion ueberschreitet den 32-Bit-Adressraum."
        );
    }
    const std::uint64_t end = start + device->size();

    for (const auto& existing : regions_) {
        const std::uint64_t existing_start = existing.info.base_address;
        const std::uint64_t existing_end =
            existing_start + existing.info.size;
        if (start < existing_end && existing_start < end) {
            throw std::invalid_argument(
                "Speicherregionen duerfen sich nicht ueberlappen."
            );
        }
    }

    regions_.push_back(MappedRegion{
        MemoryRegionInfo{
            std::move(name),
            base_address,
            device->size(),
            access
        },
        std::move(device)
    });

    std::sort(
        regions_.begin(),
        regions_.end(),
        [](const MappedRegion& left, const MappedRegion& right) {
            return left.info.base_address < right.info.base_address;
        }
    );
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

bool Memory::contains(
    const std::uint32_t address,
    const std::size_t width
) const noexcept {
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

std::uint8_t Memory::read_u8(const std::uint32_t address) const {
    const auto& mapped = resolve(address, 1u);
    return mapped.device->read_u8(region_offset(mapped.info, address));
}

std::uint16_t Memory::read_u16(const std::uint32_t address) const {
    const auto& mapped = resolve(address, 2u);
    return mapped.device->read_u16(region_offset(mapped.info, address));
}

std::uint32_t Memory::read_u32(const std::uint32_t address) const {
    const auto& mapped = resolve(address, 4u);
    return mapped.device->read_u32(region_offset(mapped.info, address));
}

std::uint32_t Memory::read_s8(const std::uint32_t address) const {
    const auto value = read_u8(address);
    return (value & 0x80u) != 0u
        ? 0xFFFFFF00u | static_cast<std::uint32_t>(value)
        : static_cast<std::uint32_t>(value);
}

std::uint32_t Memory::read_s16(const std::uint32_t address) const {
    const auto value = read_u16(address);
    return (value & 0x8000u) != 0u
        ? 0xFFFF0000u | static_cast<std::uint32_t>(value)
        : static_cast<std::uint32_t>(value);
}

void Memory::write_u8(
    const std::uint32_t address,
    const std::uint8_t value
) {
    const auto& mapped = resolve_writable(address, 1u);
    mapped.device->write_u8(region_offset(mapped.info, address), value);
}

void Memory::write_u16(
    const std::uint32_t address,
    const std::uint16_t value
) {
    const auto& mapped = resolve_writable(address, 2u);
    mapped.device->write_u16(region_offset(mapped.info, address), value);
}

void Memory::write_u32(
    const std::uint32_t address,
    const std::uint32_t value
) {
    const auto& mapped = resolve_writable(address, 4u);
    mapped.device->write_u32(region_offset(mapped.info, address), value);
}

const Memory::MappedRegion& Memory::resolve(
    const std::uint32_t address,
    const std::size_t width
) const {
    if (width == 0u) {
        throw std::invalid_argument(
            "Ein Speicherzugriff braucht eine positive Breite."
        );
    }

    const std::uint64_t start = address;
    if (width > address_space_size - start) {
        throw std::out_of_range(
            "Speicherzugriff ueberschreitet den 32-Bit-Adressraum."
        );
    }
    const std::uint64_t end = start + width;

    for (const auto& mapped : regions_) {
        const std::uint64_t region_start = mapped.info.base_address;
        const std::uint64_t region_end = region_start + mapped.info.size;
        if (start >= region_start && end <= region_end) {
            return mapped;
        }
        if (start >= region_start && start < region_end) {
            throw std::out_of_range(
                "Speicherzugriff ueberschreitet die Region '" +
                mapped.info.name + "'."
            );
        }
    }

    throw std::out_of_range(
        "Keine Speicherregion fuer Adresse " + hex_address(address) + "."
    );
}

const Memory::MappedRegion& Memory::resolve_writable(
    const std::uint32_t address,
    const std::size_t width
) const {
    const auto& mapped = resolve(address, width);
    if (mapped.info.access != MemoryRegionAccess::ReadWrite) {
        throw std::runtime_error(
            "Schreibzugriff auf schreibgeschuetzte Region '" +
            mapped.info.name + "'."
        );
    }
    return mapped;
}

} // namespace katana::runtime
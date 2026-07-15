#include "katana/runtime/aica.hpp"

#include "katana/runtime/dreamcast_memory.hpp"

#include <stdexcept>
#include <string>

namespace katana::runtime {

std::size_t AicaRegisterFile::width_bytes(const MemoryAccessWidth width) noexcept {
    return static_cast<std::size_t>(width);
}

void AicaRegisterFile::check(const std::uint32_t offset, const MemoryAccessWidth width) const {
    const auto bytes = width_bytes(width);
    if (static_cast<std::size_t>(offset) + bytes > registers_.size()) {
        throw std::out_of_range("AICA-Registerzugriff liegt ausserhalb des Registerfensters.");
    }
}

std::uint32_t AicaRegisterFile::read(
    const std::uint32_t offset,
    const MemoryAccessWidth width
) const {
    check(offset, width);
    std::uint32_t result = 0u;
    for (std::size_t index = 0u; index < width_bytes(width); ++index) {
        result |= static_cast<std::uint32_t>(registers_[offset + index]) << (index * 8u);
    }
    return result;
}

void AicaRegisterFile::write(
    const std::uint32_t offset,
    const std::uint32_t value,
    const MemoryAccessWidth width
) {
    check(offset, width);
    for (std::size_t index = 0u; index < width_bytes(width); ++index) {
        registers_[offset + index] = static_cast<std::uint8_t>(value >> (index * 8u));
    }
    ++writes_;
}

void AicaRegisterFile::reset() noexcept {
    registers_.fill(0u);
    writes_ = 0u;
}

std::uint64_t AicaRegisterFile::write_count() const noexcept { return writes_; }

std::shared_ptr<AicaRegisterFile> map_aica_registers(Memory& memory) {
    auto registers = std::make_shared<AicaRegisterFile>();
    auto device = std::make_shared<MmioMemoryDevice>(
        aica_register_size,
        [registers](const std::uint32_t offset, const MemoryAccessWidth width) {
            return registers->read(offset, width);
        },
        [registers](const std::uint32_t offset, const std::uint32_t value, const MemoryAccessWidth width) {
            registers->write(offset, value, width);
        }
    );
    for (const auto segment : dreamcast_direct_segment_bases) {
        const auto base = segment + aica_register_physical_base;
        memory.map_region("dreamcast-aica-registers-" + std::to_string(base), base, device);
    }
    return registers;
}

} // namespace katana::runtime

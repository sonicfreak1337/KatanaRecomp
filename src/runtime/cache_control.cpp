#include "katana/runtime/cache_control.hpp"

#include <stdexcept>

namespace katana::runtime {
namespace {
template <std::size_t Size>
std::uint32_t read_cache_data(const std::array<std::uint8_t, Size>& data,
                              const std::uint32_t offset) {
    const auto index = static_cast<std::size_t>(offset) & (Size - 1u);
    return static_cast<std::uint32_t>(data[index]) |
           (static_cast<std::uint32_t>(data[index + 1u]) << 8u) |
           (static_cast<std::uint32_t>(data[index + 2u]) << 16u) |
           (static_cast<std::uint32_t>(data[index + 3u]) << 24u);
}

template <std::size_t Size>
void write_cache_data(std::array<std::uint8_t, Size>& data,
                      const std::uint32_t offset,
                      const std::uint32_t value) {
    const auto index = static_cast<std::size_t>(offset) & (Size - 1u);
    data[index] = static_cast<std::uint8_t>(value);
    data[index + 1u] = static_cast<std::uint8_t>(value >> 8u);
    data[index + 2u] = static_cast<std::uint8_t>(value >> 16u);
    data[index + 3u] = static_cast<std::uint8_t>(value >> 24u);
}

template <std::size_t Size>
void write_cache_address(std::array<std::uint32_t, Size>& entries,
                         const std::uint32_t offset,
                         const std::uint32_t value,
                         const std::uint32_t value_mask) {
    auto& entry = entries[(offset >> 5u) & (Size - 1u)];
    if ((offset & 8u) == 0u) {
        entry = value & value_mask;
        return;
    }
    constexpr std::uint32_t tag_mask = 0x1FFFFC00u;
    if ((entry & 1u) != 0u && (entry & tag_mask) == (value & tag_mask))
        entry = (entry & tag_mask) | (value & (value_mask & 3u));
}

std::shared_ptr<MmioMemoryDevice>
make_cache_array_device(const std::function<std::uint32_t(std::uint32_t)>& read,
                        const std::function<void(std::uint32_t, std::uint32_t)>& write) {
    return std::make_shared<MmioMemoryDevice>(
        sh4_cache_array_aperture_size,
        [read](const auto offset, const auto width) {
            if (width != MemoryAccessWidth::Word || (offset & 3u) != 0u)
                throw std::invalid_argument("SH-4-Cachearray erlaubt nur ausgerichtete Longwords.");
            return read(offset);
        },
        [write](const auto offset, const auto value, const auto width) {
            if (width != MemoryAccessWidth::Word || (offset & 3u) != 0u)
                throw std::invalid_argument("SH-4-Cachearray erlaubt nur ausgerichtete Longwords.");
            write(offset, value);
        });
}
} // namespace

std::uint32_t Sh4CacheControl::value() const noexcept {
    return value_;
}

std::uint64_t Sh4CacheControl::instruction_invalidation_count() const noexcept {
    return instruction_invalidations_;
}

void Sh4CacheControl::write(const std::uint32_t value) {
    if ((value & ~supported_write_mask) != 0u) {
        throw std::invalid_argument("SH-4-CCR-Schreibzugriff setzt reservierte Bits.");
    }
    if ((value & instruction_invalidate) != 0u) {
        ++instruction_invalidations_;
        for (auto& entry : instruction_addresses_) entry &= ~1u;
    }
    value_ = value & supported_write_mask & ~instruction_invalidate;
}

std::uint32_t Sh4CacheControl::read_instruction_address(const std::uint32_t offset) const {
    return instruction_addresses_[(offset >> 5u) & (instruction_addresses_.size() - 1u)];
}

std::uint32_t Sh4CacheControl::read_operand_address(const std::uint32_t offset) const {
    return operand_addresses_[(offset >> 5u) & (operand_addresses_.size() - 1u)];
}

std::uint32_t Sh4CacheControl::read_instruction_data(const std::uint32_t offset) const {
    return read_cache_data(instruction_data_, offset);
}

std::uint32_t Sh4CacheControl::read_operand_data(const std::uint32_t offset) const {
    return read_cache_data(operand_data_, offset);
}

void Sh4CacheControl::write_instruction_address(const std::uint32_t offset,
                                                const std::uint32_t value) {
    write_cache_address(instruction_addresses_, offset, value, 0x1FFFFC01u);
}

void Sh4CacheControl::write_operand_address(const std::uint32_t offset,
                                            const std::uint32_t value) {
    write_cache_address(operand_addresses_, offset, value, 0x1FFFFC03u);
}

void Sh4CacheControl::write_instruction_data(const std::uint32_t offset,
                                             const std::uint32_t value) {
    write_cache_data(instruction_data_, offset, value);
}

void Sh4CacheControl::write_operand_data(const std::uint32_t offset,
                                         const std::uint32_t value) {
    write_cache_data(operand_data_, offset, value);
}

void Sh4CacheControl::reset() noexcept {
    value_ = 0u;
    instruction_invalidations_ = 0u;
    instruction_addresses_.fill(0u);
    operand_addresses_.fill(0u);
    instruction_data_.fill(0u);
    operand_data_.fill(0u);
}

std::shared_ptr<Sh4CacheControl> map_sh4_cache_control(Memory& memory) {
    auto state = std::make_shared<Sh4CacheControl>();
    auto device = std::make_shared<MmioMemoryDevice>(
        sizeof(std::uint32_t),
        [state](const std::uint32_t offset, const MemoryAccessWidth width) {
            if (offset != 0u || width != MemoryAccessWidth::Word) {
                throw std::invalid_argument("SH-4-CCR erlaubt nur 32-Bit-Zugriffe.");
            }
            return state->value();
        },
        [state](
            const std::uint32_t offset, const std::uint32_t value, const MemoryAccessWidth width) {
            if (offset != 0u || width != MemoryAccessWidth::Word) {
                throw std::invalid_argument("SH-4-CCR erlaubt nur 32-Bit-Zugriffe.");
            }
            state->write(value);
        });
    memory.map_region("sh4-cache-control", sh4_cache_control_address, std::move(device));
    memory.map_region(
        "sh4-instruction-cache-address-array",
        sh4_instruction_cache_address_array,
        make_cache_array_device(
            [state](const auto offset) { return state->read_instruction_address(offset); },
            [state](const auto offset, const auto value) {
                state->write_instruction_address(offset, value);
            }));
    memory.map_region(
        "sh4-instruction-cache-data-array",
        sh4_instruction_cache_data_array,
        make_cache_array_device(
            [state](const auto offset) { return state->read_instruction_data(offset); },
            [state](const auto offset, const auto value) {
                state->write_instruction_data(offset, value);
            }));
    memory.map_region(
        "sh4-operand-cache-address-array",
        sh4_operand_cache_address_array,
        make_cache_array_device(
            [state](const auto offset) { return state->read_operand_address(offset); },
            [state](const auto offset, const auto value) {
                state->write_operand_address(offset, value);
            }));
    memory.map_region(
        "sh4-operand-cache-data-array",
        sh4_operand_cache_data_array,
        make_cache_array_device(
            [state](const auto offset) { return state->read_operand_data(offset); },
            [state](const auto offset, const auto value) {
                state->write_operand_data(offset, value);
            }));
    return state;
}

} // namespace katana::runtime

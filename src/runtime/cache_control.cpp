#include "katana/runtime/cache_control.hpp"

#include <stdexcept>

namespace katana::runtime {

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
    }
    value_ = value & supported_write_mask & ~instruction_invalidate;
}

void Sh4CacheControl::reset() noexcept {
    value_ = 0u;
    instruction_invalidations_ = 0u;
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
    return state;
}

} // namespace katana::runtime

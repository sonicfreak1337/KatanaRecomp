#include "katana/runtime/cache_control.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Exception, typename Function> bool throws(Function&& function) {
    try {
        function();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    using namespace katana::runtime;
    Memory memory(0u);
    const auto cache = map_sh4_cache_control(memory);

    require(memory.read_u32(sh4_cache_control_address) == 0u,
            "CCR besitzt keinen definierten Resetwert.");
    memory.write_u32(sh4_cache_control_address,
                     Sh4CacheControl::instruction_invalidate | 0x00000001u);
    const auto cached_value = memory.read_u32(sh4_cache_control_address);
    const auto invalidations = cache->instruction_invalidation_count();
    require(cached_value == 1u && invalidations == 1u,
            "CCR-Invalidierung ist nicht selbstloeschend oder wird nicht gezaehlt.");
    memory.write_u32(sh4_instruction_cache_address_array + 0x20u, 0xFFFFFFFFu);
    memory.write_u32(sh4_operand_cache_address_array + 0x40u, 0xABCDE803u);
    memory.write_u32(sh4_instruction_cache_data_array + 0x24u, 0x11223344u);
    memory.write_u32(sh4_operand_cache_data_array + 0x44u, 0x55667788u);
    require(memory.read_u32(sh4_instruction_cache_address_array + 0x20u) == 0x1FFFFC01u &&
                memory.read_u32(sh4_operand_cache_address_array + 0x40u) == 0x0BCDE803u &&
                memory.read_u32(sh4_instruction_cache_data_array + 0x24u) == 0x11223344u &&
                memory.read_u32(sh4_operand_cache_data_array + 0x44u) == 0x55667788u,
            "SH-4-IC-/OC-Adress- oder Datenarrays maskieren Longwords falsch.");
    memory.write_u32(sh4_operand_cache_address_array + 0x48u, 0x0BCDE800u);
    require(memory.read_u32(sh4_operand_cache_address_array + 0x40u) == 0x0BCDE800u,
            "Assoziativer OC-Adressarraywrite aktualisiert U/V bei passendem Tag nicht.");
    memory.write_u32(sh4_instruction_cache_address_array + 0x20u, 0x00000401u);
    memory.write_u32(sh4_cache_control_address, Sh4CacheControl::instruction_invalidate);
    require((memory.read_u32(sh4_instruction_cache_address_array + 0x20u) & 1u) == 0u,
            "CCR.ICI invalidiert die IC-Adressarray-Validbits nicht.");
    const auto invalid_bits = throws<MemoryAccessError>(
        [&] { memory.write_u32(sh4_cache_control_address, 0x40000000u); });
    const auto invalid_width = throws<MemoryAccessError>(
        [&] { static_cast<void>(memory.read_u16(sh4_cache_control_address)); });
    const auto invalid_array_width = throws<MemoryAccessError>(
        [&] { memory.write_u16(sh4_operand_cache_address_array, 0u); });
    require(invalid_bits && invalid_width && invalid_array_width,
            "CCR oder Cachearrays akzeptieren reservierte Bits oder falsche Zugriffsbreiten.");
    cache->reset();
    require(cache->value() == 0u && cache->instruction_invalidation_count() == 0u,
            "CCR-Reset hinterlaesst Zustand.");

    std::cout << "SH-4-CCR-Bootminimum erfolgreich.\n";
    return EXIT_SUCCESS;
}

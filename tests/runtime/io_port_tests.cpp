#include "katana/runtime/io_port.hpp"

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
    const auto ports = map_sh4_io_ports(memory, {0x0302u, 0x000Au});

    require(memory.read_u32(sh4_port_control_a_address) == 0u &&
                memory.read_u16(sh4_port_data_a_address) == 0x0302u &&
                memory.read_u32(sh4_port_control_b_address) == 0u &&
                memory.read_u16(sh4_port_data_b_address) == 0x000Au &&
                memory.read_u16(sh4_gpio_interrupt_control_address) == 0u,
            "SH-4-I/O-Ports bilden Reset- oder externe Eingangswerte nicht ab.");

    memory.write_u16(sh4_port_data_a_address, 0x0001u);
    memory.write_u32(sh4_port_control_a_address, 0x00000001u);
    memory.write_u16(sh4_port_data_b_address, 0x0005u);
    memory.write_u32(sh4_port_control_b_address, 0x00000011u);
    memory.write_u16(sh4_gpio_interrupt_control_address, 0xA55Au);
    require(memory.read_u16(sh4_port_data_a_address) == 0x0303u &&
                memory.read_u16(sh4_port_data_b_address) == 0x000Fu &&
                ports->gpio_interrupt_control() == 0xA55Au,
            "SH-4-I/O-Port liest Ausgangslatches und Eingangspins nicht bitgenau zusammen.");

    require(throws<MemoryAccessError>(
                [&] { static_cast<void>(memory.read_u32(sh4_port_data_a_address)); }) &&
                throws<std::invalid_argument>(
                    [&] { memory.write_u16(sh4_port_control_a_address, 0u); }),
            "SH-4-I/O-Port akzeptiert eine laut Registervertrag falsche Zugriffsbreite.");

    ports->reset();
    require(ports->control_a() == 0u && ports->control_b() == 0u &&
                ports->gpio_interrupt_control() == 0u && ports->data_a() == 0x0302u,
            "SH-4-I/O-Portreset verliert Eingangspins oder behaelt Steuerzustand.");

    std::cout << "SH-4-I/O-Portmodell erfolgreich.\n";
    return EXIT_SUCCESS;
}

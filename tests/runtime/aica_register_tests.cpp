#include "katana/runtime/aica.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) { std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
template<typename E, typename F> bool throws(F&& f) { try { f(); } catch (const E&) { return true; } return false; }
}

int main() {
    using namespace katana::runtime;
    Memory bus(0u);
    const auto aica = map_aica_registers(bus);
    bus.write_u32(0x00700000u, 0x78563412u);
    require(bus.read_u8(0x80700000u) == 0x12u && bus.read_u16(0xA0700002u) == 0x7856u,
        "AICA-Register sind nicht little-endian oder werden nicht zwischen Aliasen geteilt.");
    bus.write_u8(0x60700001u, 0xAAu);
    require(bus.read_u32(0x00700000u) == 0x7856AA12u && aica->write_count() == 2u,
        "Breitenbewusster AICA-Teilzugriff verliert Registerbits oder Ereignisse.");
    bus.write_u16(0x00700000u + aica_common_register_base, 0x0F0Fu);
    require(aica->read(aica_common_register_base, MemoryAccessWidth::Halfword) == 0x0F0Fu,
        "AICA-Common-Registerfenster ist nicht erreichbar.");
    require(throws<std::out_of_range>([&] {
        static_cast<void>(aica->read(static_cast<std::uint32_t>(aica_register_size - 1u), MemoryAccessWidth::Word));
    }), "AICA-Registerdatei akzeptiert einen ueberlaufenden Zugriff.");
    require(!bus.contains(0xE0700000u), "P4 wurde faelschlich als direkter AICA-Alias abgebildet.");
    aica->reset();
    require(aica->read(0u, MemoryAccessWidth::Word) == 0u && aica->write_count() == 0u,
        "AICA-Reset ist nicht deterministisch.");

    std::cout << "KR-2901 AICA-Registerminimum erfolgreich.\n";
}

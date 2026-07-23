#include "katana/runtime/dreamcast_memory.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
void require(const bool value, const std::string& message) {
    if (!value) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
template <typename E, typename F> bool throws(F&& f) {
    try {
        f();
    } catch (const E&) {
        return true;
    }
    return false;
}
void unlock(katana::runtime::Memory& bus) {
    bus.write_u8(0x00200000u + katana::runtime::dreamcast_flash_unlock_address_1, 0xAAu);
    bus.write_u8(0x00200000u + katana::runtime::dreamcast_flash_unlock_address_2, 0x55u);
}
void program(katana::runtime::Memory& bus, const std::uint32_t address, const std::uint8_t value) {
    unlock(bus);
    bus.write_u8(0x00200000u + katana::runtime::dreamcast_flash_unlock_address_1, 0xA0u);
    bus.write_u8(address, value);
}
} // namespace

int main() {
    using namespace katana::runtime;
    std::vector<std::uint8_t> source(dreamcast_flash_size, 0xFFu);
    source[0x1234u] = 0xF0u;
    Memory bus(0u);
    const auto flash = map_dreamcast_command_flash(bus, source);

    bus.write_u8(0x00200000u + dreamcast_flash_unlock_address_1, 0xAAu);
    const auto unlock_snapshot = flash->snapshot();
    require(unlock_snapshot.command_state == FlashCommandState::Unlock2 &&
                !unlock_snapshot.write_protected &&
                unlock_snapshot.size == dreamcast_flash_size,
            "Flash-Snapshot verliert den laufenden Command-FSM-Zustand.");
    bus.write_u8(0x00200000u, 0xF0u);

    program(bus, 0x00201234u, 0x5Au);
    require(bus.read_u8(0x80201234u) == 0x50u && source[0x1234u] == 0xF0u &&
                flash->source_byte(0x1234u) == 0xF0u,
            "Programmieren beachtet weder 1->0-Regel noch Copy-on-write.");
    program(bus, 0x00201234u, 0xFFu);
    require(bus.read_u8(0x00201234u) == 0x50u, "Programmieren setzt Bits unzulaessig von 0 auf 1.");

    unlock(bus);
    bus.write_u8(0x00200000u + dreamcast_flash_unlock_address_1, 0x80u);
    unlock(bus);
    bus.write_u8(0x00201234u, 0x30u);
    require(bus.read_u8(0x00201234u) == 0xFFu, "Sektorloeschung stellt 0xFF nicht wieder her.");

    require(throws<std::runtime_error>([&] { bus.write_u8(0x00200010u, 0x12u); }),
            "Direkter Bus-Schreibzugriff umgeht das Flash-Protokoll.");
    unlock(bus);
    require(throws<std::runtime_error>(
                [&] { bus.write_u8(0x00200000u + dreamcast_flash_unlock_address_1, 0x90u); }),
            "Nicht unterstuetztes Herstellerkommando scheitert nicht sichtbar.");
    bus.write_u8(0x00200000u, 0xF0u);

    flash->set_write_protected(true);
    require(flash->snapshot().write_protected,
            "Flash-Snapshot verliert den gesetzten Schreibschutz.");
    require(throws<std::runtime_error>([&] { program(bus, 0x00200020u, 0u); }),
            "Schreibschutz verhindert Programmierung nicht.");
    flash->set_write_protected(false);
    program(bus, 0x00200020u, 0x7Fu);
    require(bus.read_u8(0x00200020u) == 0x7Fu,
            "Reset nach Fehler stellt den Lesemodus nicht wieder her.");
    program(bus, 0x00200021u, 0xF0u);
    require(bus.read_u8(0x00200021u) == 0xF0u,
            "Programmdaten 0xF0 werden faelschlich als Reset interpretiert.");

    std::cout << "Zustandsbehaftetes Copy-on-write-Flash erfolgreich.\n";
}

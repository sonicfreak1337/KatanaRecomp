#include "katana/runtime/aica.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

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
} // namespace

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
                static_cast<void>(aica->read(static_cast<std::uint32_t>(aica_register_size - 1u),
                                             MemoryAccessWidth::Word));
            }),
            "AICA-Registerdatei akzeptiert einen ueberlaufenden Zugriff.");
    require(!bus.contains(0xE0700000u), "P4 wurde faelschlich als direkter AICA-Alias abgebildet.");
    aica->reset();
    require(aica->read(0u, MemoryAccessWidth::Word) == 0u && aica->write_count() == 0u,
            "AICA-Reset ist nicht deterministisch.");

    EventScheduler scheduler;
    const auto rtc = map_aica_rtc(bus, &scheduler);
    const auto initial = aica_rtc_default_seconds;
    require(bus.read_u32(0x00710000u) == (initial >> 16u) &&
                bus.read_u16(0x80710004u) == (initial & 0xFFFFu) &&
                bus.read_u8(0xA0710000u) == ((initial >> 16u) & 0xFFu),
            "AICA-RTC liefert High/Low nicht breiten- oder aliasgetreu.");
    static_cast<void>(scheduler.advance_by(2u * dreamcast_guest_cycles_per_second, 0u));
    require(rtc->counter() == initial + 2u &&
                bus.read_u32(0x60710004u) == ((initial + 2u) & 0xFFFFu),
            "AICA-RTC folgt nicht deterministisch der Gastzeit.");

    bus.write_u32(0x00710008u, 1u);
    require(rtc->write_enabled(), "AICA-RTC-Schreibfreigabe wurde nicht gesetzt.");
    bus.write_u16(0x20710004u, 0x5678u);
    require(rtc->write_enabled() && rtc->counter() == initial + 2u,
            "AICA-RTC-Low-Latch publiziert einen halbfertigen Zaehlerstand.");
    bus.write_u32(0x40710000u, 0x1234u);
    require(!rtc->write_enabled() && rtc->counter() == 0x12345678u,
            "AICA-RTC-High-Schreiben setzt Zaehler oder Schreibschutz falsch.");
    bus.write_u32(0x00710004u, 0x9ABCu);
    require(rtc->counter() == 0x12345678u,
            "AICA-RTC akzeptiert einen geschuetzten Zaehlerwrite.");
    require(bus.read_u32(0x00710008u) == 0u,
            "AICA-RTC-Control-Readback muss reservierte Bits als null liefern.");
    require(throws<MemoryAccessError>([&] { static_cast<void>(bus.read_u16(0x00710002u)); }),
            "AICA-RTC akzeptiert einen Zugriff zwischen den Registern.");
    require(!bus.contains(0xE0710000u), "P4 wurde faelschlich als direkter RTC-Alias abgebildet.");

    scheduler.reset();
    require(rtc->counter() == initial && !rtc->write_enabled(),
            "AICA-RTC-Schedulerreset ist nicht deterministisch.");

    std::cout << "KR-2901 AICA-Register und RTC erfolgreich.\n";
}

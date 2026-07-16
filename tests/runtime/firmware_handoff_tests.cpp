#include "katana/runtime/firmware_handoff.hpp"

#include <iostream>
#include <stdexcept>

using namespace katana::runtime;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        FirmwareHandoffMap map;
        map.map_segment(
            {"boot-rom-p1", FirmwareSegmentKind::Rom, 0x80000000u, 0x00000000u, 0x00200000u});
        map.map_segment(
            {"boot-rom-p2", FirmwareSegmentKind::Rom, 0xA0000000u, 0x00000000u, 0x00200000u});
        map.map_segment(
            {"main-ram", FirmwareSegmentKind::Ram, 0x8C000000u, 0x0C000000u, 0x01000000u});
        require(map.canonical_origin_count() == 2u,
                "P1-/P2-ROM-Aliase wurden als getrennte physische Funktionseinheiten erfunden.");
        const auto p1 = map.resolve(0x80000100u);
        const auto p2 = map.resolve(0xA0000100u);
        require(p1.physical_address == p2.physical_address &&
                    p1.virtual_address != p2.virtual_address,
                "Resetpfad kann ROM-Alias nicht wechseln und Diagnoseadressen bewahren.");

        map.record_copy({0x00001000u, 0x0C010000u, 0x100u, "boot-rom:0x1000->ram", true, false});
        const auto copied = map.resolve(0x8C010020u);
        require(copied.copy && copied.statically_proven && copied.copy->source_physical == 0x1000u,
                "RAM-Bootstrap behaelt keine verifizierte ROM-Provenienz.");
        map.mark_copy_changed(0xAC010020u);
        require(!map.resolve(0x8C010020u).statically_proven,
                "Veraenderte Kopie bleibt faelschlich statisch bewiesen.");

        map.install_runtime_symbol(
            {"bios_syscall_vector", 0x8C0000B0u, 0x0C0000B0u, "runtime-install"});
        require(map.runtime_symbols().size() == 1u &&
                    map.runtime_symbols()[0].physical_address == 0x0C0000B0u,
                "Dynamischer BIOS-ABI-Vektor ist nicht als Laufzeitsymbol sichtbar.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

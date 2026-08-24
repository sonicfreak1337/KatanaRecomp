#include "katana/runtime/native_port_content.hpp"
#include "katana/runtime/native_port_aot_runtime.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    const std::array immutable_ranges{
        katana::runtime::NativePortImmutableRange{
            0x0C000000u, 2u,
            katana::runtime::native_port_immutable_range_mask(
                katana::runtime::NativePortImmutableRangeKind::Executable)}};
    katana::runtime::NativePortImmutableWriteGuard immutable_guard(
        immutable_ranges);
    immutable_guard.reserve_additional_runtime_executable_ranges(2u);
    immutable_guard.add_runtime_executable_range(0x8C900000u, 0x100u);
    immutable_guard.validate_runtime_executable_range_present(
        0x0C900000u, 0x100u);
    immutable_guard.remove_runtime_executable_range_committed(
        0x0C900000u, 0x100u);
    require(!immutable_guard.tracks_address(0x8C900000u, 0x100u),
            "Vorbereitete Guard-Retirement-Transaktion bleibt aktiv.");

    katana::runtime::NativePortExecutableLifecycleLedger lifecycle_ledger(2u);
    const auto first_lifecycle = lifecycle_ledger.acquire(0x8C900000u, 0x1000u);
    require(first_lifecycle != 0u,
            "Der gemeinsame Executable-Lifecycle vergibt keine Generation.");

    bool overlap_failed = false;
    try {
        static_cast<void>(
            lifecycle_ledger.acquire(0x0C900800u, 0x1000u));
    } catch (const katana::runtime::NativePortContractError&) {
        overlap_failed = true;
    }
    require(overlap_failed,
            "Aliasierende Runtime-Image/AOT-Ranges werden nicht gemeinsam abgewiesen.");

    const auto retirement = lifecycle_ledger.release(first_lifecycle);
    const auto replacement = lifecycle_ledger.acquire(0x0C900000u, 0x1000u);
    require(retirement > first_lifecycle && replacement > retirement,
            "Executable-Aktivierung und Retirement sind nicht monoton generationiert.");
    static_cast<void>(lifecycle_ledger.release(replacement));

    std::cout << "Native-Port-Executable-Lifecycle erfolgreich.\n";
    return EXIT_SUCCESS;
}

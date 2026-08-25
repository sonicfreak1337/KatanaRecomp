#include "katana/runtime/native_port_content.hpp"
#include "katana/runtime/native_port_aot_runtime.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
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

    try {
        constexpr std::uint32_t source_start = 0x80800000u;
        constexpr std::uint32_t runtime_start = 0x8C900000u;
        constexpr std::string_view identity =
            "sha256:7af85194466a76bee16168ca8152d4560bd9bec17ade2525f267ed49a54f36a9";
        const std::array<std::uint8_t, 4u> bytes{0x09u, 0x00u, 0x0Bu, 0x00u};
        const std::array source_bindings{
            katana::runtime::NativePortLoadedAotSourceBindingView{
                katana::runtime::NativePortLoadedAotSourceTransform::Identity,
                identity, 0u, static_cast<std::uint32_t>(bytes.size()), 0u}};
        const std::array blocks{
            katana::runtime::NativePortLoadedAotBlockIdentityView{
                0u, static_cast<std::uint32_t>(bytes.size()), identity}};
        const std::array modules{
            katana::runtime::NativePortLoadedAotModuleView{
                source_start, static_cast<std::uint32_t>(bytes.size()),
                identity, source_bindings, blocks}};
        katana::runtime::NativePortMemory memory;
        auto& cpu = memory.cpu();
        cpu.memory.write_bytes(
            0x0C900000u, bytes,
            katana::runtime::CodeWriteSource::Copy);
        katana::runtime::NativePortImmutableWriteGuard module_guard(
            immutable_ranges);
        katana::runtime::NativePortExecutableLifecycleLedger module_ledger(1u);
        katana::runtime::NativePortLoadedAotBinder binder(
            cpu, modules, module_guard, module_ledger);
        const auto lifecycle = binder.stage_runtime_module(
            {identity, source_start, runtime_start,
             static_cast<std::uint32_t>(bytes.size())});
        require(binder.bind_entry(runtime_start),
                "Geladenes AOT-Testmodul wurde nicht aktiviert.");
        const auto active = binder.active_module_for_address(
            0x0C900002u);
        require(active.has_value() && active->sha256 == identity &&
                    active->source_start == source_start &&
                    active->runtime_start == runtime_start &&
                    active->byte_size == bytes.size() &&
                    active->lifecycle_generation == lifecycle,
                "Aktive Modulidentitaet verliert Alias, Range oder Generation.");
        require(!binder.active_module_for_address(0x8C800000u).has_value(),
                "Ungebundene Adresse wurde einem geladenen Modul zugeordnet.");
    } catch (const std::exception& error) {
        require(false, std::string("Aktive Modulidentitaet warf: ") +
                           error.what());
    }

    std::cout << "Native-Port-Executable-Lifecycle erfolgreich.\n";
    return EXIT_SUCCESS;
}

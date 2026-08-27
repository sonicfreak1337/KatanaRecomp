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

class ChainGuardHost final
    : public katana::runtime::NativePortHostServices {
  public:
    [[nodiscard]] std::uint64_t monotonic_time_nanoseconds()
        const noexcept override {
        return 1u;
    }

    [[nodiscard]] katana::runtime::NativePortLifecycleState
    poll_lifecycle() override {
        return katana::runtime::NativePortLifecycleState::Running;
    }

    void synchronize_simulation_boundary() override {}
    void begin_frame(std::uint64_t) override {}
    void present_frame(std::uint64_t) override {}

    [[nodiscard]] std::uint64_t presented_frames()
        const noexcept override {
        return 0u;
    }
};

[[nodiscard]] bool chain_guard_static_entry(
    const std::uint32_t address) noexcept {
    return address == 0x8C010000u;
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

    {
        katana::runtime::CpuState chain_cpu;
        ChainGuardHost chain_host;
        katana::runtime::NativePortContext chain_context;
        chain_context.cpu = &chain_cpu;
        chain_context.host = &chain_host;
        katana::runtime::NativePortImmutableWriteGuard chain_guard(
            immutable_ranges);
        katana::runtime::NativePortAotServices chain_services(
            chain_context, chain_guard_static_entry, chain_guard);
        require(chain_services.aot_contract_valid() &&
                    chain_services.can_chain_executable_block(0x8C010000u) &&
                    !chain_services.can_chain_executable_block(0x8C010002u),
                "Vollstaendiger AOT-Observer-Vertrag laesst keinen exakten "
                "statischen Chain-Entry zu.");
        chain_cpu.memory.clear_guest_write_batch_observer();
        require(!chain_services.aot_contract_valid() &&
                    !chain_services.can_chain_executable_block(0x8C010000u),
                "Retired Batch-Observer stoppt AOT-Chaining nicht fail-closed.");
        chain_cpu.memory.clear_guest_write_observer();
    }

    immutable_guard.reserve_additional_runtime_executable_ranges(3u);
    immutable_guard.add_runtime_executable_range(0x8C900000u, 0x100u);
    immutable_guard.add_runtime_executable_range(0x8C900200u, 0x80u);
    immutable_guard.validate_runtime_executable_range_present(
        0x0C900000u, 0x100u);
    require(immutable_guard.tracks_address(0x0C900000u, 0x100u) &&
                immutable_guard.tracks_address(0x8C900200u, 0x80u),
            "Verzoegerter Guard-Rebuild verlor registrierte Ranges.");
    immutable_guard.remove_runtime_executable_range_committed(
        0x0C900000u, 0x100u);
    require(!immutable_guard.tracks_address(0x8C900000u, 0x100u) &&
                immutable_guard.tracks_address(0x0C900200u, 0x80u),
            "Guard-Retirement verlor verbleibende Range oder blieb aktiv.");
    immutable_guard.observe_write(
        {0x8C900200u, 2u, katana::runtime::CodeWriteSource::Cpu, true});
    require(immutable_guard.write_detected() &&
                immutable_guard.first_write_address() == 0x0C900200u,
            "Noexcept Write-Observer materialisierte Dirty-Guard nicht.");
    immutable_guard.remove_runtime_executable_range_committed(
        0x0C900200u, 0x80u);

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
        require(binder.validate_bound_entry(runtime_start) &&
                    binder.validate_bound_entry(0x0C900000u),
                "Geladenes AOT-Entry-Bitmap verlor Exact- oder Alias-Entry.");
        bool midblock_failed = false;
        try {
            static_cast<void>(
                binder.validate_bound_entry(runtime_start + 2u));
        } catch (const katana::runtime::NativePortContractError&) {
            midblock_failed = true;
        }
        require(midblock_failed,
                "Geladenes AOT-Entry-Bitmap akzeptierte Midblock-Ziel.");
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
        module_guard.observe_write(
            {runtime_start, 2u, katana::runtime::CodeWriteSource::Cpu, true});
        bool generation_failed = false;
        try {
            static_cast<void>(binder.validate_bound_entry(runtime_start));
        } catch (const katana::runtime::NativePortContractError&) {
            generation_failed = true;
        }
        require(generation_failed,
                "Geladenes AOT-Entry-Bitmap umging Guard-Invalidierung.");
        require(binder.deactivate_runtime_range(runtime_start, bytes.size()) ==
                        1u &&
                    !binder.validate_bound_entry(runtime_start),
                "Retired geladenes AOT-Entry blieb dispatchbar.");
    } catch (const std::exception& error) {
        require(false, std::string("Aktive Modulidentitaet warf: ") +
                           error.what());
    }

    // Closure probes may retain a fixed runtime-image dispatch only at an
    // exact generated block boundary of the currently active immutable
    // generation. Merely landing inside the image or the block is not an
    // identity proof, and retirement removes the entry immediately.
    try {
        constexpr std::uint32_t source_start = 0x80810000u;
        constexpr std::uint32_t runtime_start = 0x8C910000u;
        constexpr std::string_view identity =
            "sha256:7af85194466a76bee16168ca8152d4560bd9bec17ade2525f267ed49a54f36a9";
        const std::array<std::uint8_t, 4u> bytes{
            0x09u, 0x00u, 0x0Bu, 0x00u};
        const std::array blocks{
            katana::runtime::NativePortLoadedAotBlockIdentityView{
                0u, static_cast<std::uint32_t>(bytes.size()), identity}};
        const std::array images{
            katana::runtime::NativePortRuntimeImageView{
                "closure-runtime-image", source_start, runtime_start,
                static_cast<std::uint32_t>(bytes.size()), identity, blocks}};
        katana::runtime::NativePortMemory memory;
        auto& cpu = memory.cpu();
        cpu.memory.write_bytes(
            0x0C910000u, bytes, katana::runtime::CodeWriteSource::Copy);
        katana::runtime::NativePortImmutableWriteGuard image_guard(
            immutable_ranges);
        katana::runtime::NativePortExecutableLifecycleLedger image_ledger(1u);
        katana::runtime::NativePortRuntimeImageBindings bindings(
            cpu, images, image_guard, image_ledger);
        bindings.activate("closure-runtime-image");
        const auto exact = bindings.active_entry_for_address(runtime_start);
        require(exact.has_value() && exact->image_id == "closure-runtime-image" &&
                    exact->image_sha256 == identity &&
                    exact->block_sha256 == identity &&
                    exact->source_start == source_start &&
                    exact->runtime_start == runtime_start &&
                    exact->source_offset == 0u &&
                    exact->block_size == bytes.size() &&
                    exact->lifecycle_generation != 0u,
                "Aktives Runtime-Image verlor exakte Entry-/Blockidentitaet "
                "oder Generation.");
        require(!bindings.active_entry_for_address(runtime_start + 2u).has_value(),
                "Runtime-Image-Midblock wurde als exakter Closure-Entry akzeptiert.");
        require(bindings.deactivate_runtime_range(runtime_start, bytes.size()) == 1u &&
                    !bindings.active_entry_for_address(runtime_start).has_value(),
                "Retired Runtime-Image blieb als Closure-Entry aktiv.");
    } catch (const std::exception& error) {
        require(false, std::string("Runtime-Image-Entryidentitaet warf: ") +
                           error.what());
    }

    std::cout << "Native-Port-Executable-Lifecycle erfolgreich.\n";
    return EXIT_SUCCESS;
}

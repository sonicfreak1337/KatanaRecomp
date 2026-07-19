#include "katana/runtime/executable_modules.hpp"
#include "katana/runtime/indirect_dispatch.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

katana::runtime::BlockExit block(katana::runtime::CpuState&,
                                 katana::runtime::BlockExecutionContext&) {
    katana::runtime::BlockExit exit;
    exit.kind = katana::runtime::BlockEndKind::Return;
    return exit;
}

void relocated_module_regression() {
    using namespace katana::runtime;
    CpuState cpu;
    const std::vector<std::uint8_t> source{0x09u, 0xfeu, 0xffu, 0xffu, 0x0bu, 0x00u};
    auto relocated = source;
    relocated[0] = 0x09u;
    relocated[1] = 0x00u;
    relocated[2] = 0x00u;
    relocated[3] = 0x00u;
    cpu.memory.write_bytes(0x200u, relocated, CodeWriteSource::Copy);

    ExecutableModule module;
    module.id = "relocated-synthetic-module";
    module.source_identity = "free-relocation-fixture-v1";
    module.guest_start = 0x200u;
    module.bytes = source;
    module.relocations = {{0u, executable_module_relocation_module_base32, 0}};
    ExecutableModuleCatalog modules;
    modules.publish(module);

    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    const BlockVariantKey variant{};
    bool relocated_snapshot_observed = false;
    std::size_t callback_calls = 0u;
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        &tracker,
        {true, 2u, source.size()},
        [&](const std::uint32_t target,
            const std::span<const std::uint8_t> snapshot,
            const BlockVariantKey& requested_variant) {
            ++callback_calls;
            relocated_snapshot_observed = snapshot.size() >= 4u && snapshot[0] == 0x09u &&
                                          snapshot[1] == 0x00u && snapshot[2] == 0x00u &&
                                          snapshot[3] == 0x00u;
            return MaterializedBlockCandidate{{target,
                                               target,
                                               2u,
                                               BlockEndKind::Return,
                                               requested_variant,
                                               block,
                                               "synthetic-relocated-decoder",
                                               true},
                                              true,
                                              true,
                                              true,
                                              true,
                                              1u,
                                              1u,
                                              1u,
                                              1u,
                                              1u};
        });
    IndirectDispatchRequest request;
    request.kind = IndirectDispatchKind::TailJump;
    request.callsite = 0x80u;
    request.target = 0x200u;
    request.variant = variant;
    request.dispatch_class = RuntimeDispatchClass::RuntimeOnly;
    request.materializer = &materializer;
    const auto dispatched = dispatch_indirect(cpu, blocks, request);
    const auto registered = blocks.resolve(dispatched.block);
    require(registered.has_value(), "Relokierter Runtimeblock wurde nicht registriert.");
    BlockExecutionContext context;
    const auto exit = registered->get().function(cpu, context);
    require(dispatched.block && relocated_snapshot_observed && exit.kind == BlockEndKind::Return &&
                callback_calls == 1u && materializer.metrics().materializations == 1u,
            "Kanonisch relokierte Modulbytes wurden nicht materialisiert und dispatcht.");

    modules.update_relocations(
        module.id, {{0u, executable_module_relocation_module_base32, 4}}, blocks, tracker);
    bool changed_addend_rejected = false;
    try {
        static_cast<void>(dispatch_indirect(cpu, blocks, request));
    } catch (const IndirectDispatchError&) {
        changed_addend_rejected = true;
    }
    require(changed_addend_rejected && callback_calls == 1u &&
                materializer.last_failure() == MaterializationFailure::ByteIdentityMismatch,
            "Geaenderter Relocation-Addend akzeptiert stale Gastbytes.");

    const auto relocation_generation = modules.find(module.id)->relocation_generation;
    bool unknown_type_rejected = false;
    try {
        modules.update_relocations(module.id, {{0u, 99u, 0}}, blocks, tracker);
    } catch (const std::invalid_argument&) {
        unknown_type_rejected = true;
    }
    require(unknown_type_rejected &&
                modules.find(module.id)->relocation_generation == relocation_generation,
            "Unbekannter Relocationtyp veraendert den aktiven Modulvertrag.");
}

} // namespace

int main() {
    using namespace katana::runtime;
    CpuState cpu;
    const std::vector<std::uint8_t> bytes{0x09u, 0x00u, 0x0bu, 0x00u};
    cpu.memory.write_bytes(0x100u, bytes, CodeWriteSource::Copy);

    ExecutableModuleCatalog modules;
    ExecutableModule module;
    module.id = "synthetic-module";
    module.source_identity = "free-fixture-v1";
    module.guest_start = 0x100u;
    module.bytes = bytes;
    modules.publish(module);

    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    BlockVariantKey variant;
    DemandBlockMaterializer materializer(modules,
                                         blocks,
                                         &tracker,
                                         {true, 2u, 4u},
                                         [](const std::uint32_t target,
                                            const std::span<const std::uint8_t>,
                                            const BlockVariantKey& requested_variant) {
                                             return MaterializedBlockCandidate{
                                                 {target,
                                                  target,
                                                  2u,
                                                  BlockEndKind::Return,
                                                  requested_variant,
                                                  block,
                                                  "synthetic-decoder",
                                                  true},
                                                 true,
                                                 true,
                                                 true,
                                                 true,
                                                 1u,
                                                 1u,
                                                 1u,
                                                 1u,
                                                 1u};
                                         });
    IndirectDispatchMetrics dispatch_metrics;
    IndirectDispatchRequest request;
    request.kind = IndirectDispatchKind::TailJump;
    request.callsite = 0x80u;
    request.target = 0x100u;
    request.variant = variant;
    request.dispatch_class = RuntimeDispatchClass::RuntimeOnly;
    request.metrics = &dispatch_metrics;
    request.materializer = &materializer;
    const auto first = dispatch_indirect(cpu, blocks, request);
    const auto second = dispatch_indirect(cpu, blocks, request);
    require(first.block == second.block && materializer.metrics().requests == 1u &&
                materializer.metrics().materializations == 1u &&
                materializer.metrics().materialized_bytes == 2u &&
                dispatch_metrics.runtime_only_hits() == 2u,
            "Demand-driven-Blockmaterialisierung ist nicht gecacht oder nicht gezaehlt.");
    const auto profile = dispatch_metrics.runtime_only_sites().find(0x80u);
    require(profile != dispatch_metrics.runtime_only_sites().end() && profile->second.calls == 2u &&
                profile->second.hits == 2u && profile->second.materializations == 1u &&
                profile->second.stability() == RuntimeTargetStability::Monomorphic &&
                dispatch_metrics.serialize_json().find("0x00000080") == std::string::npos &&
                dispatch_metrics.serialize_json(true).find("0x00000080") != std::string::npos,
            "Runtime-only-Siteprofil verliert Stabilitaet oder redigiert Adressen nicht.");

    cpu.memory.write_u8(0x102u, 0xffu);
    request.target = 0x102u;
    bool mismatch_rejected = false;
    try {
        static_cast<void>(dispatch_indirect(cpu, blocks, request));
    } catch (const IndirectDispatchError&) {
        mismatch_rejected = true;
    }
    require(mismatch_rejected && materializer.metrics().first_failure ==
                                     MaterializationFailure::ByteIdentityMismatch,
            "Veraenderte Modulbytes werden materialisiert oder unsichtbar akzeptiert.");

    cpu.memory.write_bytes(0x100u, bytes, CodeWriteSource::Copy);
    auto replacement = module;
    replacement.bytes[0] = 0x0bu;
    cpu.memory.write_bytes(0x100u, replacement.bytes, CodeWriteSource::Copy);
    modules.replace(replacement, blocks, tracker);
    materializer.record_invalidation(0x100u, replacement.bytes.size(), dispatch_metrics);
    require(!blocks.lookup(0x100u, variant).has_value() &&
                modules.find("synthetic-module") != nullptr &&
                modules.find("synthetic-module")->generation == 2u &&
                modules.metrics().replacements == 1u && modules.metrics().unloads == 1u &&
                dispatch_metrics.runtime_only_sites().at(0x80u).invalidations == 1u,
            "Modulersatz erhaelt Lebenszeit oder invalidiert alte Bloecke nicht.");

    modules.unload("synthetic-module", blocks, tracker);
    require(modules.resolve(0x100u) == nullptr && modules.metrics().unloads == 2u,
            "Modul-Unload hinterlaesst eine aktive ausfuehrbare Herkunft.");

    relocated_module_regression();

    std::cout << "KR-4704 executable module and materialization regression passed.\n";
    return EXIT_SUCCESS;
}

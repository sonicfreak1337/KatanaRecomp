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

void runtime_write_provenance_regression() {
    using namespace katana::runtime;
    CpuState cpu;
    ExecutableModuleCatalog modules;
    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    cpu.memory.set_guest_write_observer([&](const GuestWriteEvent& event) {
        modules.record_runtime_write(event.address, event.size, event.bytes_changed);
        const auto invalidation =
            tracker.observe_write(event.address, event.size, event.source, event.bytes_changed);
        if (!invalidation.byte_identical)
            static_cast<void>(blocks.erase_overlapping_physical(event.address, event.size));
    });

    std::size_t callback_calls = 0u;
    DemandBlockMaterializer materializer(modules,
                                         blocks,
                                         &tracker,
                                         {true, 4u, 512u},
                                         [&](const std::uint32_t target,
                                             const std::span<const std::uint8_t> snapshot,
                                             const BlockVariantKey& requested_variant) {
                                             ++callback_calls;
                                             return MaterializedBlockCandidate{
                                                 {target,
                                                  target,
                                                  2u,
                                                  BlockEndKind::Return,
                                                  requested_variant,
                                                  block,
                                                  "synthetic-runtime-write-decoder",
                                                  true},
                                                 snapshot.size() >= 2u,
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
    request.target = 0x400u;
    request.dispatch_class = RuntimeDispatchClass::RuntimeOnly;
    request.materializer = &materializer;

    bool unwritten_rejected = false;
    try {
        static_cast<void>(dispatch_indirect(cpu, blocks, request));
    } catch (const IndirectDispatchError&) {
        unwritten_rejected = true;
    }
    require(unwritten_rejected &&
                materializer.last_failure() == MaterializationFailure::UnknownSource &&
                modules.resolve(request.target) == nullptr,
            "Unbeschriebenes RAM wurde ohne Herkunft als Code autorisiert.");

    request.target = 0x300u;
    cpu.memory.write_u16(request.target, 0x0009u);
    cpu.memory.write_u16(request.target + 2u, 0x000Bu);
    const auto first = dispatch_indirect(cpu, blocks, request);
    require(first.block && callback_calls == 1u && modules.resolve(request.target) != nullptr &&
                modules.resolve(request.target)->materializable(request.target, 2u),
            "Tatsaechlich geschriebener Runtimecode wurde nicht bytegenau autorisiert.");

    const auto first_module_id = modules.resolve(request.target)->id;
    cpu.memory.write_u16(request.target, 0x000Bu);
    require(modules.find(first_module_id) == nullptr &&
                !blocks.lookup(request.target, request.variant).has_value(),
            "Gastschreibzugriff entwertet Runtimecode-Snapshot nicht.");
    const auto second = dispatch_indirect(cpu, blocks, request);
    require(second.block && callback_calls == 2u && modules.resolve(request.target) != nullptr &&
                modules.resolve(request.target)->id != first_module_id &&
                materializer.metrics().materializations == 2u,
            "Geaenderter Runtimecode erhielt keinen neuen Provenienz-Snapshot.");

    CpuState alias_cpu;
    const auto alias_ram = std::make_shared<LinearMemoryDevice>(0x1000u);
    alias_cpu.memory.map_region("runtime-write-p0", 0x0C000000u, alias_ram);
    alias_cpu.memory.map_region("runtime-write-p1", 0x8C000000u, alias_ram);
    ExecutableModuleCatalog alias_modules;
    RuntimeBlockTable alias_blocks;
    ExecutableCodeTracker alias_tracker;
    alias_blocks.bind_code_tracker(&alias_tracker);
    alias_cpu.memory.set_guest_write_observer([&](const GuestWriteEvent& event) {
        const auto physical = canonical_physical_address(event.address);
        alias_modules.record_runtime_write(event.address, event.size, event.bytes_changed);
        const auto invalidation =
            alias_tracker.observe_write(physical, event.size, event.source, event.bytes_changed);
        if (!invalidation.byte_identical)
            static_cast<void>(alias_blocks.erase_overlapping_physical(physical, event.size));
    });
    DemandBlockMaterializer alias_materializer(alias_modules,
                                               alias_blocks,
                                               &alias_tracker,
                                               {true, 2u, 256u},
                                               [](const std::uint32_t target,
                                                  const std::span<const std::uint8_t> snapshot,
                                                  const BlockVariantKey& requested_variant) {
                                                   return MaterializedBlockCandidate{
                                                       {target,
                                                        canonical_physical_address(target),
                                                        2u,
                                                        BlockEndKind::Return,
                                                        requested_variant,
                                                        block,
                                                        "synthetic-runtime-alias-decoder",
                                                        true},
                                                       snapshot.size() >= 2u,
                                                       true,
                                                       true,
                                                       true,
                                                       1u,
                                                       1u,
                                                       1u,
                                                       1u,
                                                       1u};
                                               });
    alias_cpu.memory.write_u16(0x0C000100u, 0x0009u);
    request.target = 0x8C000100u;
    request.materializer = &alias_materializer;
    const auto alias_dispatch = dispatch_indirect(alias_cpu, alias_blocks, request);
    const auto alias_module_id = alias_modules.resolve(request.target)->id;
    alias_cpu.memory.write_u16(0x0C000100u, 0x000Bu);
    require(alias_dispatch.block && alias_modules.find(alias_module_id) == nullptr &&
                !alias_blocks.lookup(request.target, request.variant).has_value(),
            "P0-Write entwertet den ueber P1 materialisierten Runtimecode nicht.");
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
    runtime_write_provenance_regression();

    std::cout << "KR-4704 executable module and materialization regression passed.\n";
    return EXIT_SUCCESS;
}

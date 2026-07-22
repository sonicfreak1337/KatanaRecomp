#include "katana/runtime/executable_modules.hpp"
#include "katana/runtime/block_guards.hpp"
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
        modules.record_runtime_write(event.address, event.size, event.source, event.bytes_changed);
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
                modules.resolve(request.target)->materializable(request.target, 2u) &&
                modules.resolve(request.target + 4u) == nullptr,
            "Tatsaechlich geschriebener Runtimecode wurde nicht bytegenau autorisiert.");

    const auto first_module_id = modules.resolve(request.target)->id;
    cpu.memory.write_u16(request.target, 0x000Bu);
    require(modules.find(first_module_id) != nullptr &&
                modules.resolve(request.target, 2u) == nullptr &&
                modules.resolve(request.target + 2u, 2u) != nullptr &&
                !blocks.lookup(request.target, request.variant).has_value(),
            "Gastschreibzugriff entwertet nicht exakt sein Runtimecode-Patchfenster.");
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
        alias_modules.record_runtime_write(
            event.address, event.size, event.source, event.bytes_changed);
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
                                                        4u,
                                                        BlockEndKind::Return,
                                                        requested_variant,
                                                        block,
                                                        "synthetic-runtime-alias-decoder",
                                                        true},
                                                       snapshot.size() >= 4u,
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
    alias_cpu.memory.write_u16(0x0C000102u, 0x000Bu);
    request.target = 0x8C000100u;
    request.materializer = &alias_materializer;
    const auto alias_dispatch = dispatch_indirect(alias_cpu, alias_blocks, request);
    request.target = 0x8C000102u;
    const auto alias_interior_dispatch = dispatch_indirect(alias_cpu, alias_blocks, request);
    const auto alias_module_id = alias_modules.resolve(request.target)->id;
    alias_cpu.memory.write_u16(0x0C000100u, 0x000Bu);
    require(alias_dispatch.block == alias_interior_dispatch.block &&
                alias_materializer.metrics().materializations == 1u &&
                alias_materializer.metrics().cache_hits == 1u &&
                alias_modules.find(alias_module_id) != nullptr &&
                alias_modules.resolve(0x8C000100u, 2u) == nullptr &&
                alias_modules.resolve(0x8C000102u, 2u) != nullptr &&
                !alias_blocks.active(alias_dispatch.block),
            "Innerer P1-Einstieg wird nicht wiederverwendet oder durch P0-Write entwertet.");
}

void partial_module_patch_regression() {
    using namespace katana::runtime;
    CpuState cpu;
    const std::vector<std::uint8_t> bytes{
        0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u,
        0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u};
    cpu.memory.write_bytes(0x1000u, bytes, CodeWriteSource::Copy);

    ExecutableModule source;
    source.id = "partial-source-module";
    source.source_identity = "free-partial-source-v1";
    source.guest_start = 0x1000u;
    source.bytes = bytes;
    ExecutableModuleCatalog modules;
    modules.publish(source);

    ExecutableModule alias_overlap = source;
    alias_overlap.id = "physical-alias-overlap";
    alias_overlap.guest_start = 0x80001000u;
    bool alias_rejected = false;
    try {
        modules.publish(alias_overlap);
    } catch (const std::invalid_argument&) {
        alias_rejected = true;
    }
    require(alias_rejected, "P0/P1/P2-Aliasueberlappung wurde nicht physisch erkannt.");

    modules.record_runtime_write(0x80001006u, 2u, CodeWriteSource::Cpu, true);
    cpu.memory.write_u16(0x1006u, 0x000Bu, CodeWriteSource::Copy);
    const auto* patched_source = modules.find(source.id);
    const std::vector<ExecutableModuleActiveExtent> expected_extents{{0u, 6u}, {8u, 8u}};
    require(patched_source != nullptr && patched_source->active_extents == expected_extents &&
                modules.resolve(0x1000u, 6u) == patched_source &&
                modules.resolve(0x1006u, 2u) == nullptr &&
                modules.resolve(0x1008u, 8u) == patched_source &&
                modules.resolve(0x1004u, 6u) == nullptr,
            "Partieller Patch hat Prefix/Suffix oder das physische Patchfenster verloren.");

    RuntimeBlockTable blocks;
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        nullptr,
        {true, 2u, 64u},
        [](const std::uint32_t target,
           const std::span<const std::uint8_t> snapshot,
           const BlockVariantKey& requested_variant) {
            return MaterializedBlockCandidate{{target,
                                               canonical_physical_address(target),
                                               2u,
                                               BlockEndKind::Return,
                                               requested_variant,
                                               block,
                                               "partial-extent-decoder",
                                               true},
                                              snapshot.size() == 6u,
                                              true,
                                              true,
                                              true,
                                              1u,
                                              1u,
                                              1u,
                                              1u,
                                              1u};
        });
    const auto materialized =
        materializer.try_materialize(cpu, 0x1000u, 0x1000u, BlockVariantKey{}, 0x80u);
    require(materialized.has_value() &&
                materializer.last_failure() == MaterializationFailure::None,
            "Materialisierung wurde nicht am aktiven Prefix-Extent begrenzt.");

    const auto promoted = modules.promote_runtime_write(cpu.memory, 0x1006u, 16u);
    require(promoted && modules.resolve(0x1006u, 2u) != nullptr &&
                modules.resolve(0x1008u, 2u) == modules.find(source.id),
            "Runtime-Patch wurde nicht ausschliesslich in die geoeffnete Luecke publiziert.");

    const auto extents_before_identical = modules.find(source.id)->active_extents;
    modules.record_runtime_write(0x1000u, 2u, CodeWriteSource::Cpu, false);
    require(modules.find(source.id)->active_extents == extents_before_identical,
            "Byte-identischer Write veraendert aktive Modulbereiche.");

    ExecutableModuleCatalog provenance;
    provenance.record_runtime_write(0x2000u, 4u, CodeWriteSource::Cpu, true);
    provenance.record_runtime_write(0x2000u, 4u, CodeWriteSource::Copy, true);
    require(!provenance.promote_runtime_write(cpu.memory, 0x2000u, 4u),
            "Copy-Load behaelt veraltete Runtime-Write-Provenienz.");
    provenance.record_runtime_write(0x2010u, 4u, CodeWriteSource::StoreQueue, true);
    provenance.record_runtime_write(0x2010u, 4u, CodeWriteSource::Dma, true);
    require(!provenance.promote_runtime_write(cpu.memory, 0x2010u, 4u),
            "DMA-Load behaelt veraltete Runtime-Write-Provenienz.");
    provenance.record_runtime_write(0x2020u, 4u, CodeWriteSource::Fpu, true);
    provenance.record_runtime_write(0x2020u, 4u, CodeWriteSource::Copy, false);
    cpu.memory.write_u32(0x2020u, 0x000B0009u, CodeWriteSource::Copy);
    require(provenance.promote_runtime_write(cpu.memory, 0x2020u, 4u),
            "Byte-identischer Load loescht aktuelle Runtime-Write-Provenienz.");

    ExecutableModuleCatalog loaded_modules;
    ExecutableModule old_loaded = source;
    old_loaded.id = "old-loaded-module";
    old_loaded.source_identity = "free-old-load-v1";
    loaded_modules.publish(old_loaded);
    ExecutableModule replacement;
    replacement.id = "replacement-window";
    replacement.source_identity = "free-replacement-load-v1";
    replacement.guest_start = 0x80001006u;
    replacement.bytes = {0x0Bu, 0x00u};
    RuntimeBlockTable loaded_blocks;
    ExecutableCodeTracker loaded_tracker;
    loaded_modules.publish_loaded_range(replacement, loaded_blocks, loaded_tracker);
    const auto* old_after_load = loaded_modules.find(old_loaded.id);
    require(old_after_load != nullptr && old_after_load->active_extents == expected_extents &&
                loaded_modules.resolve(0x1006u, 2u)->id == replacement.id &&
                loaded_modules.resolve(0x1000u, 6u) == old_after_load &&
                loaded_modules.resolve(0x1008u, 8u) == old_after_load,
            "Teilweiser Copy-Load deaktiviert weiterhin das gesamte Quellmodul.");
}

void non_overlapping_write_stress_regression() {
    using namespace katana::runtime;
    ExecutableModuleCatalog modules;
    constexpr std::uint32_t module_count = 512u;
    constexpr std::uint32_t first_address = 0x00100000u;
    constexpr std::uint32_t stride = 0x100u;
    const std::vector<std::uint8_t> bytes(16u, 0x09u);
    for (std::uint32_t index = 0u; index < module_count; ++index) {
        ExecutableModule module;
        module.id = "stress-module-" + std::to_string(index);
        module.source_identity = "free-stress-fixture-v1";
        module.guest_start = first_address + index * stride;
        module.bytes = bytes;
        modules.publish(std::move(module));
    }

    for (std::uint32_t index = 0u; index < module_count; ++index) {
        modules.record_runtime_write(first_address + index * stride + 0x80u,
                                     4u,
                                     CodeWriteSource::Cpu,
                                     true);
    }
    for (std::uint32_t index = 0u; index < module_count; ++index) {
        const auto* module = modules.resolve(first_address + index * stride, bytes.size());
        require(module != nullptr &&
                    module->active_extents ==
                        std::vector<ExecutableModuleActiveExtent>{{0u, 16u}},
                "Nicht ueberlappender Write fragmentiert ein Modul im Store-Hotpath.");
    }
    require(modules.metrics().loads == module_count && modules.metrics().unloads == 0u,
            "Nicht ueberlappende Store-Last veraendert Modulmetriken.");

    const auto patched_address = first_address + (module_count / 2u) * stride;
    modules.record_runtime_write(patched_address + 4u, 4u, CodeWriteSource::Cpu, true);
    const auto* patched = modules.find("stress-module-256");
    require(patched != nullptr &&
                patched->active_extents ==
                    std::vector<ExecutableModuleActiveExtent>{{0u, 4u}, {8u, 8u}},
            "Der schnelle Non-Overlap-Pfad ueberspringt einen echten Modulpatch.");
}

void active_extent_page_index_lifecycle_regression() {
    using namespace katana::runtime;
    CpuState cpu;
    cpu.memory.write_u16(0x00020000u, 0x000Bu, CodeWriteSource::Copy);
    ExecutableModuleCatalog modules;
    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;

    ExecutableModule source;
    source.id = "indexed-source";
    source.source_identity = "free-indexed-source-v1";
    source.guest_start = 0x00010000u;
    source.bytes.assign(0x2200u, 0x09u);
    modules.publish(source);

    const auto initial_metrics = modules.metrics();
    modules.record_runtime_write(0x00020000u, 2u, CodeWriteSource::Cpu, true);
    require(modules.metrics().write_index_rejections ==
                    initial_metrics.write_index_rejections + 1u &&
                modules.metrics().write_index_scans == initial_metrics.write_index_scans,
            "Aktiver Seitenindex verwirft einen sicher modulfreien Write nicht.");
    require(modules.promote_runtime_write(cpu.memory, 0x00020000u, 2u),
            "Seitenindex-Fast-Reject ueberspringt die Runtime-Write-Provenienz.");

    modules.record_runtime_write(0x80010004u, 2u, CodeWriteSource::Cpu, true);
    modules.record_runtime_write(0xA0011000u, 2u, CodeWriteSource::Cpu, true);
    require(modules.metrics().write_index_scans == initial_metrics.write_index_scans + 2u &&
                modules.resolve(0x00010004u, 2u) == nullptr &&
                modules.resolve(0x80011000u, 2u) == nullptr &&
                modules.resolve(0x00010006u, 2u) != nullptr &&
                modules.resolve(0x00011002u, 2u) != nullptr,
            "Aliaswrite trifft den Seitenindex oder aktualisierte Teil-Extents nicht.");

    modules.unload(source.id, blocks, tracker);
    const auto after_unload = modules.metrics();
    modules.record_runtime_write(0x00010008u, 2u, CodeWriteSource::Cpu, true);
    require(modules.metrics().write_index_rejections ==
                    after_unload.write_index_rejections + 1u &&
                modules.metrics().write_index_scans == after_unload.write_index_scans,
            "Unload hinterlaesst einen aktiven Seitenindexeintrag.");

    ExecutableModule replacement;
    replacement.id = "indexed-replacement";
    replacement.source_identity = "free-indexed-replacement-v1";
    replacement.guest_start = 0x00030000u;
    replacement.bytes.assign(0x100u, 0x09u);
    modules.publish(replacement);
    auto moved = replacement;
    moved.guest_start = 0x00031000u;
    modules.replace(moved, blocks, tracker);
    const auto after_replacement = modules.metrics();
    modules.record_runtime_write(0x00030004u, 2u, CodeWriteSource::Cpu, true);
    modules.record_runtime_write(0x80031004u, 2u, CodeWriteSource::Cpu, true);
    require(modules.metrics().write_index_rejections ==
                    after_replacement.write_index_rejections + 1u &&
                modules.metrics().write_index_scans == after_replacement.write_index_scans + 1u &&
                modules.resolve(0x00031004u, 2u) == nullptr &&
                modules.resolve(0x00031006u, 2u) != nullptr,
            "Replacement entfernt alte Indexseiten oder indiziert neue Aliasbytes nicht.");

    ExecutableModule loaded_source;
    loaded_source.id = "indexed-loaded-source";
    loaded_source.source_identity = "free-indexed-loaded-source-v1";
    loaded_source.guest_start = 0x00040000u;
    loaded_source.bytes.assign(0x100u, 0x09u);
    modules.publish(loaded_source);
    ExecutableModule loaded_window;
    loaded_window.id = "indexed-loaded-window";
    loaded_window.source_identity = "free-indexed-loaded-window-v1";
    loaded_window.guest_start = 0x80040020u;
    loaded_window.bytes.assign(0x10u, 0x0Bu);
    const auto before_loaded_range = modules.metrics();
    modules.publish_loaded_range(loaded_window, blocks, tracker);
    require(modules.metrics().write_index_scans == before_loaded_range.write_index_scans + 1u &&
                modules.resolve(0x00040020u, 0x10u)->id == loaded_window.id &&
                modules.resolve(0x00040000u, 0x20u)->id == loaded_source.id &&
                modules.resolve(0x00040030u, 0xD0u)->id == loaded_source.id,
            "Publish-loaded-range verliert neuen oder verbleibenden Seitenindexzustand.");
    const auto after_loaded_range = modules.metrics();
    modules.record_runtime_write(0xA0040022u, 2u, CodeWriteSource::Cpu, true);
    require(modules.metrics().write_index_scans == after_loaded_range.write_index_scans + 1u &&
                modules.resolve(0x00040022u, 2u) == nullptr &&
                modules.resolve(0x00040024u, 2u)->id == loaded_window.id,
            "Aliaspatch nach publish-loaded-range wird vom aktualisierten Index uebersehen.");
}

void rejected_loaded_range_is_atomic_regression() {
    using namespace katana::runtime;
    CpuState cpu;
    cpu.memory.write_u16(0x6000u, 0x000Bu, CodeWriteSource::Copy);

    ExecutableModuleCatalog modules;
    ExecutableModule source;
    source.id = "atomic-source";
    source.source_identity = "free-atomic-source-v1";
    source.guest_start = 0x4000u;
    source.bytes = {0x09u, 0x00u, 0x0Bu, 0x00u};
    modules.publish(source);
    modules.record_runtime_write(0x6000u, 2u, CodeWriteSource::Cpu, true);

    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    RuntimeBlock runtime_block{0x6000u,
                               0x6000u,
                               2u,
                               BlockEndKind::Return,
                               {},
                               block,
                               "atomic-load-runtime-block",
                               true};
    const auto block_identity = stable_runtime_block_identity(runtime_block);
    const auto handle = blocks.register_runtime(runtime_block);
    static_cast<void>(tracker.register_block({block_identity,
                                              0x6000u,
                                              2u,
                                              runtime_block.provenance,
                                              {},
                                              ExecutableBlockOrigin::RuntimeWrite}));
    blocks.bind_code_tracker(&tracker);

    const auto metrics_before = modules.metrics();
    const auto extents_before = modules.find(source.id)->active_extents;
    const auto tracker_generation_before = tracker.page_generation(0x6000u);
    const auto tracker_invalidations_before = tracker.invalidation_count();
    ExecutableModule rejected;
    rejected.id = source.id;
    rejected.source_identity = "free-rejected-load-v1";
    rejected.guest_start = 0x6000u;
    rejected.bytes = {0x0Bu, 0x00u};
    bool duplicate_rejected = false;
    try {
        modules.publish_loaded_range(rejected, blocks, tracker);
    } catch (const std::invalid_argument&) {
        duplicate_rejected = true;
    }

    const auto metrics_after = modules.metrics();
    require(duplicate_rejected && modules.find(source.id) != nullptr &&
                modules.find(source.id)->active_extents == extents_before &&
                modules.resolve(0x6000u, 2u) == nullptr && blocks.active(handle) &&
                tracker.valid(block_identity) &&
                tracker.page_generation(0x6000u) == tracker_generation_before &&
                tracker.invalidation_count() == tracker_invalidations_before &&
                metrics_after.loads == metrics_before.loads &&
                metrics_after.unloads == metrics_before.unloads &&
                metrics_after.replacements == metrics_before.replacements &&
                metrics_after.invalidated_blocks == metrics_before.invalidated_blocks &&
                metrics_after.write_index_rejections == metrics_before.write_index_rejections &&
                metrics_after.write_index_scans == metrics_before.write_index_scans,
            "Abgelehnter Load veraendert Katalog, Blocks, Tracker oder Metriken.");
    require(modules.promote_runtime_write(cpu.memory, 0x6000u, 2u),
            "Abgelehnter Load loescht Runtime-Write-Provenienz.");
}

void rejected_replacement_is_atomic_regression() {
    using namespace katana::runtime;
    ExecutableModuleCatalog modules;
    ExecutableModule source;
    source.id = "atomic-replace-source";
    source.source_identity = "free-atomic-replace-source-v1";
    source.guest_start = 0x7000u;
    source.bytes.assign(0x100u, 0x09u);
    modules.publish(source);
    ExecutableModule blocker;
    blocker.id = "atomic-replace-blocker";
    blocker.source_identity = "free-atomic-replace-blocker-v1";
    blocker.guest_start = 0x7100u;
    blocker.bytes.assign(0x100u, 0x0Bu);
    modules.publish(blocker);

    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    RuntimeBlock runtime_block{0x7000u,
                               0x7000u,
                               2u,
                               BlockEndKind::Return,
                               {},
                               block,
                               "atomic-replace-runtime-block",
                               true};
    const auto block_identity = stable_runtime_block_identity(runtime_block);
    const auto handle = blocks.register_runtime(runtime_block);
    static_cast<void>(tracker.register_block({block_identity,
                                              0x7000u,
                                              2u,
                                              runtime_block.provenance,
                                              {},
                                              ExecutableBlockOrigin::RuntimeWrite}));
    blocks.bind_code_tracker(&tracker);

    const auto metrics_before = modules.metrics();
    const auto extents_before = modules.find(source.id)->active_extents;
    const auto tracker_generation_before = tracker.page_generation(0x7000u);
    const auto tracker_invalidations_before = tracker.invalidation_count();
    auto overlap = source;
    overlap.source_identity = "free-atomic-replace-overlap-v2";
    overlap.guest_start = blocker.guest_start;
    bool overlap_rejected = false;
    try {
        modules.replace(overlap, blocks, tracker);
    } catch (const std::invalid_argument&) {
        overlap_rejected = true;
    }
    auto invalid = source;
    invalid.source_identity = "free-atomic-replace-invalid-v2";
    invalid.bytes.clear();
    bool invalid_rejected = false;
    try {
        modules.replace(invalid, blocks, tracker);
    } catch (const std::invalid_argument&) {
        invalid_rejected = true;
    }

    const auto metrics_after = modules.metrics();
    require(overlap_rejected && invalid_rejected && modules.find(source.id) != nullptr &&
                modules.find(source.id)->generation == source.generation &&
                modules.find(source.id)->active_extents == extents_before &&
                modules.resolve(0x7000u, source.bytes.size())->id == source.id &&
                modules.resolve(0x7100u, blocker.bytes.size())->id == blocker.id &&
                blocks.active(handle) && tracker.valid(block_identity) &&
                tracker.page_generation(0x7000u) == tracker_generation_before &&
                tracker.invalidation_count() == tracker_invalidations_before &&
                metrics_after.loads == metrics_before.loads &&
                metrics_after.unloads == metrics_before.unloads &&
                metrics_after.replacements == metrics_before.replacements &&
                metrics_after.invalidated_blocks == metrics_before.invalidated_blocks,
            "Abgelehnter Modulersatz mutiert Katalog, Index, Blocks, Tracker oder Metriken.");
}

void nonlinear_alias_boundary_regression() {
    using namespace katana::runtime;
    const auto rejected = [](const std::uint32_t start,
                             std::vector<ExecutableModuleActiveExtent> extents = {}) {
        ExecutableModuleCatalog modules;
        ExecutableModule module;
        module.id = "nonlinear-boundary";
        module.source_identity = "free-boundary-fixture-v1";
        module.guest_start = start;
        module.bytes = {0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u};
        module.active_extents = std::move(extents);
        try {
            modules.publish(std::move(module));
        } catch (const std::out_of_range&) {
            return true;
        }
        return false;
    };
    require(rejected(0x1FFFFFFEu) && rejected(0x9FFFFFFEu) &&
                rejected(0x7BFFFFFEu) &&
                rejected(0x1FFFFFFCu, {{0u, 4u}, {4u, 2u}}),
            "Physisch nichtlineares Modul oder Extent ueberlebt eine SH-4-Aliasgrenze.");

    ExecutableModuleCatalog legal;
    ExecutableModule edge;
    edge.id = "linear-edge";
    edge.source_identity = "free-linear-edge-v1";
    edge.guest_start = 0x1FFFFFFCu;
    edge.bytes = {0x09u, 0x00u, 0x0Bu, 0x00u};
    legal.publish(edge);
    require(legal.resolve(0x9FFFFFFCu, edge.bytes.size()) != nullptr,
            "Legales Modul unmittelbar vor der Aliasgrenze wird abgelehnt.");
}

void reverse_runtime_write_promotion_regression() {
    using namespace katana::runtime;
    CpuState cpu;
    ExecutableModuleCatalog modules;
    cpu.memory.set_guest_write_observer([&](const GuestWriteEvent& event) {
        modules.record_runtime_write(event.address, event.size, event.source, event.bytes_changed);
    });
    for (std::uint32_t offset = 0u; offset < 8u; offset += 2u)
        cpu.memory.write_u16(0x500u + offset, static_cast<std::uint16_t>(0x0009u + offset));

    require(modules.promote_runtime_write(cpu.memory, 0x504u, 128u),
            "Spaeter Runtime-Codebereich wurde nicht zuerst promotet.");
    require(modules.promote_runtime_write(cpu.memory, 0x500u, 128u),
            "Frueher entdeckter Runtime-Codebereich kollidiert mit seinem Nachbarn.");
    const auto* lower = modules.resolve(0x500u, 2u);
    const auto* upper = modules.resolve(0x504u, 2u);
    require(lower != nullptr && upper != nullptr && lower != upper && lower->bytes.size() == 4u &&
                upper->bytes.size() == 4u && lower->end_address() == upper->guest_start,
            "Rueckwaerts entdeckte Runtime-Codefenster werden nicht disjunkt begrenzt.");
}

void interpreter_interior_entry_regression() {
    using namespace katana::runtime;
    CpuState cpu;
    const std::vector<std::uint8_t> bytes{0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u};
    cpu.memory.write_bytes(0x500u, bytes, CodeWriteSource::Copy);
    ExecutableModule module;
    module.id = "interior-entry-module";
    module.source_identity = "free-interior-entry-fixture-v1";
    module.guest_start = 0x500u;
    module.bytes = bytes;
    ExecutableModuleCatalog modules;
    modules.publish(module);
    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    std::size_t callback_calls = 0u;
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        &tracker,
        {true, 4u, 64u},
        [&](const std::uint32_t target,
            const std::span<const std::uint8_t> snapshot,
            const BlockVariantKey& requested_variant) {
            ++callback_calls;
            return MaterializedBlockCandidate{{target,
                                               target,
                                               static_cast<std::uint32_t>(snapshot.size()),
                                               BlockEndKind::Return,
                                               requested_variant,
                                               block,
                                               "synthetic-interpreter-decoder",
                                               true},
                                              snapshot.size() >= 2u,
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
    request.target = 0x500u;
    request.dispatch_class = RuntimeDispatchClass::RuntimeOnly;
    request.metrics = &dispatch_metrics;
    request.materializer = &materializer;
    const auto first = dispatch_indirect(cpu, blocks, request);
    request.callsite = 0x82u;
    request.target = 0x502u;
    const auto interior = dispatch_indirect(cpu, blocks, request);
    require(first.block == interior.block && interior.resulting_pc == 0x502u && cpu.pc == 0x502u &&
                callback_calls == 1u && materializer.metrics().materializations == 1u &&
                materializer.metrics().cache_hits == 1u && blocks.size() == 1u &&
                dispatch_metrics.runtime_only_sites().at(0x82u).materializations == 0u,
            "Interpreter-Inneneinstieg erzeugt einen ueberlappenden zweiten Runtimeblock.");

    auto replacement = module;
    replacement.bytes[2] = 0x0Bu;
    cpu.memory.write_bytes(0x500u, replacement.bytes, CodeWriteSource::Copy);
    modules.replace(replacement, blocks, tracker);
    materializer.record_invalidation(0x500u, replacement.bytes.size(), dispatch_metrics);
    const auto rematerialized = dispatch_indirect(cpu, blocks, request);
    require(rematerialized.block != first.block && callback_calls == 2u &&
                materializer.metrics().materializations == 2u,
            "Modulrewrite behaelt einen stale Interpreter-Inneneinstieg aktiv.");

    CpuState native_cpu;
    native_cpu.memory.write_bytes(0x600u, bytes, CodeWriteSource::Copy);
    ExecutableModule native_module = module;
    native_module.id = "native-interior-module";
    native_module.source_identity = "free-native-interior-fixture-v1";
    native_module.guest_start = 0x600u;
    ExecutableModuleCatalog native_modules;
    native_modules.publish(native_module);
    RuntimeBlockTable native_blocks;
    std::size_t native_callbacks = 0u;
    DemandBlockMaterializer native_materializer(
        native_modules,
        native_blocks,
        nullptr,
        {true, 4u, 64u},
        [&](const std::uint32_t target,
            const std::span<const std::uint8_t> snapshot,
            const BlockVariantKey& requested_variant) {
            ++native_callbacks;
            return MaterializedBlockCandidate{{target,
                                               target,
                                               static_cast<std::uint32_t>(snapshot.size()),
                                               BlockEndKind::Return,
                                               requested_variant,
                                               block,
                                               "synthetic-native-decoder",
                                               true},
                                              true,
                                              false,
                                              true,
                                              true,
                                              true,
                                              1u,
                                              1u,
                                              1u,
                                              1u,
                                              1u};
        });
    request.metrics = nullptr;
    request.materializer = &native_materializer;
    request.target = 0x600u;
    static_cast<void>(dispatch_indirect(native_cpu, native_blocks, request));
    request.target = 0x602u;
    bool native_interior_rejected = false;
    try {
        static_cast<void>(dispatch_indirect(native_cpu, native_blocks, request));
    } catch (const IndirectDispatchError&) {
        native_interior_rejected = true;
    }
    require(native_interior_rejected && native_callbacks == 1u &&
                native_materializer.last_failure() == MaterializationFailure::InvalidBlock,
            "Nativer Runtimeblock akzeptiert unbewiesenen Inneneinstieg.");
}

void mmu_materialization_origin_regression() {
    using namespace katana::runtime;
    CpuState cpu;
    const auto ram = std::make_shared<LinearMemoryDevice>(0x1000u);
    cpu.memory.map_region("mmu-materialization-ram", 0x0C000000u, ram);
    cpu.memory.write_u16(0x0C000100u, 0x0009u, CodeWriteSource::Copy);
    cpu.address_space = std::make_shared<RuntimeAddressSpace>();
    cpu.address_space->set_mode(AddressTranslationMode::Mmu);
    cpu.address_space->write_mmucr(1u);
    cpu.address_space->ldtlb(
        {0x00002000u, 0x0C000000u, 4096u, 0u, 0u, true, true, true, true, true, true, false});

    ExecutableModule module;
    module.id = "mmu-physical-module";
    module.source_identity = "free-mmu-materialization-fixture-v1";
    module.guest_start = 0x0C000100u;
    module.bytes = {0x09u, 0x00u};
    ExecutableModuleCatalog modules;
    modules.publish(module);
    RuntimeBlockTable blocks;
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        nullptr,
        {true, 2u, 16u},
        [](const std::uint32_t target,
           const std::span<const std::uint8_t> snapshot,
           const BlockVariantKey& requested_variant) {
            return MaterializedBlockCandidate{{target,
                                               target,
                                               2u,
                                               BlockEndKind::Return,
                                               requested_variant,
                                               block,
                                               "synthetic-mmu-decoder",
                                               true},
                                              snapshot.size() == 2u && snapshot[0] == 0x09u &&
                                                  snapshot[1] == 0x00u,
                                              false,
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
    request.target = 0x00002100u;
    request.materializer = &materializer;
    const auto result = dispatch_indirect(cpu, blocks, request);
    const auto resolved_block = blocks.resolve(result.block);
    require(resolved_block.has_value() &&
                resolved_block->get().virtual_start == 0x00002100u &&
                resolved_block->get().physical_origin == 0x0C000100u,
            "MMU-Materialisierung liest virtuelle statt physischer Bytes oder verliert die VA.");
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
    partial_module_patch_regression();
    non_overlapping_write_stress_regression();
    active_extent_page_index_lifecycle_regression();
    rejected_loaded_range_is_atomic_regression();
    rejected_replacement_is_atomic_regression();
    nonlinear_alias_boundary_regression();
    reverse_runtime_write_promotion_regression();
    interpreter_interior_entry_regression();
    mmu_materialization_origin_regression();

    std::cout << "KR-4704 executable module and materialization regression passed.\n";
    return EXIT_SUCCESS;
}

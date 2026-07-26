#include "katana/runtime/executable_modules.hpp"
#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/dreamcast_boot.hpp"
#include "katana/runtime/indirect_dispatch.hpp"
#include "katana/runtime/native_aot_template.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
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
            const std::uint32_t,
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
    require(changed_addend_rejected && callback_calls == 2u &&
                materializer.last_failure() == MaterializationFailure::ByteIdentityMismatch,
            "Geaenderter Relocation-Addend akzeptiert stale Gastbytes nach der AOT-Pruefung.");

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

void native_aot_template_materialization_regression() {
    using namespace katana::runtime;
    CpuState cpu;
    std::vector<std::uint8_t> template_bytes(56u);
    for (std::size_t index = 0u; index < template_bytes.size(); ++index)
        template_bytes[index] = static_cast<std::uint8_t>(index * 3u + 1u);
    cpu.memory.write_bytes(0x1000u, template_bytes, CodeWriteSource::Copy);
    cpu.memory.write_bytes(0x2000u, template_bytes, CodeWriteSource::Copy);

    ExecutableModule source;
    source.id = "native-aot-source";
    source.source_identity = "free-native-aot-source-v1";
    source.guest_start = 0x1000u;
    source.bytes = template_bytes;
    ExecutableModule runtime_copy = source;
    runtime_copy.id = "native-aot-runtime-copy";
    runtime_copy.source_identity = "free-native-aot-runtime-copy-v1";
    runtime_copy.guest_start = 0x2000u;
    ExecutableModuleCatalog modules;
    modules.publish(source);
    modules.publish(runtime_copy);

    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        &tracker,
        {true, 4u, 128u},
        [](const std::uint32_t target,
           const std::uint32_t,
           const std::span<const std::uint8_t> snapshot,
           const BlockVariantKey& requested_variant) {
            RuntimeBlock runtime_block{target,
                                       target,
                                       4u,
                                       BlockEndKind::Return,
                                       requested_variant,
                                       block,
                                       "native-aot-template",
                                       false,
                                       RuntimeAotTemplateContract{
                                           {0x1000u, 0x2000u, 48u},
                                           56u,
                                           {{40u, 4u}}}};
            return MaterializedBlockCandidate{std::move(runtime_block),
                                              snapshot.size() >= 4u,
                                              false,
                                              true,
                                              true,
                                              true,
                                              1u,
                                              2u,
                                              1u,
                                              1u,
                                              56u};
        });
    const auto handle = materializer.try_materialize(cpu, 0x2010u, 0x2010u, {}, 0x80u);
    require(handle.has_value() && blocks.resolve(*handle)->get().aot_template.has_value() &&
                tracker.blocks().size() == 1u &&
                tracker.blocks()[0].block.physical_start == 0x2000u &&
                tracker.blocks()[0].block.size == template_bytes.size() &&
                tracker.blocks()[0].block.origin == ExecutableBlockOrigin::RomRamCopy &&
                tracker.blocks()[0].block.mutable_ranges ==
                    std::vector<NativeAotTemplateMutableRange>{{40u, 4u}} &&
                materializer.validate_for_dispatch(cpu, *handle, 0x2010u),
            "AOT-Mitteleinstieg trackt nicht die vollstaendige Vorlage samt Literalbereich.");

    cpu.memory.write_u8(0x2028u, static_cast<std::uint8_t>(template_bytes[40] ^ 0x5Au));
    const auto scratch_write =
        tracker.observe_write(0x2028u, 1u, CodeWriteSource::Cpu);
    const auto scratch_erased = blocks.erase_overlapping_physical(0x2028u, 1u);
    require(scratch_write.invalidated_blocks.empty() && scratch_erased == 0u &&
                blocks.resolve(*handle).has_value() &&
                materializer.validate_for_dispatch(cpu, *handle, 0x2010u),
            "Retained Snapshot oder Invalidierung verwarf einen bewiesenen Scratchslot-Write.");

    cpu.memory.write_u8(0x202Cu, static_cast<std::uint8_t>(template_bytes[44] ^ 0xFFu));
    require(!materializer.validate_for_dispatch(cpu, *handle, 0x2010u) &&
                materializer.last_failure() == MaterializationFailure::AotTemplateMismatch &&
                materializer.metrics().byte_identity_failures != 0u,
            "Mutation direkt neben dem Scratchslot blieb fuer nativen AOT-Dispatch unsichtbar.");
    const auto adjacent_write =
        tracker.observe_write(0x202Cu, 1u, CodeWriteSource::Cpu);
    require(adjacent_write.invalidated_blocks.size() == 1u &&
                blocks.erase_overlapping_physical(0x202Cu, 1u) == 1u,
            "Mutation direkt neben dem Scratchslot invalidierte Tracker/Blocktabelle nicht.");
    cpu.memory.write_u8(0x2028u, template_bytes[40]);
    cpu.memory.write_u8(0x202Cu, template_bytes[44]);

    RuntimeBlockTable rejected_blocks;
    DemandBlockMaterializer rejected(
        modules,
        rejected_blocks,
        nullptr,
        {true, 1u, 128u},
        [](const std::uint32_t,
           const std::uint32_t,
           const std::span<const std::uint8_t>,
           const BlockVariantKey&) {
            MaterializedBlockCandidate candidate;
            candidate.rejection_failure = MaterializationFailure::AotTemplateMismatch;
            return candidate;
        });
    require(!rejected.try_materialize(cpu, 0x2020u, 0x2020u, {}, 0x84u) &&
                rejected.last_failure() == MaterializationFailure::AotTemplateMismatch,
            "Binder-Ablehnungsgrund wird pauschal als DecodeRejected verschluckt.");
}

void overlapping_vbr_aot_target_identity_regression() {
    using namespace katana::runtime;
    constexpr std::uint32_t source_address = 0x80010000u;
    constexpr std::uint32_t vbr = 0x80002000u;
    constexpr std::uint32_t runtime_address = vbr + 0x600u;
    constexpr std::uint32_t target_module_address = 0xA0002000u;
    const auto runtime_physical = canonical_physical_address(runtime_address);

    std::vector<std::uint8_t> template_bytes(56u, 0u);
    for (std::size_t offset = 0u; offset < template_bytes.size(); offset += 2u)
        template_bytes[offset] = 0x09u;
    CpuState cpu;
    cpu.vbr = vbr;
    cpu.memory.write_bytes(
        canonical_physical_address(source_address), template_bytes, CodeWriteSource::Copy);
    cpu.memory.write_bytes(runtime_physical, template_bytes, CodeWriteSource::Copy);

    ExecutableModule source_module;
    source_module.id = "overlapping-vbr-aot-source";
    source_module.source_identity = "sha256:overlapping-vbr-aot-source";
    source_module.guest_start = source_address;
    source_module.bytes = template_bytes;
    ExecutableModule target_module;
    target_module.id = "foreign-bootstrap-alias";
    target_module.source_identity = "sha256:foreign-bootstrap-alias";
    target_module.guest_start = target_module_address;
    target_module.bytes.assign(0x1000u, 0xA5u);
    ExecutableModuleCatalog modules;
    modules.publish(source_module);
    modules.publish(target_module);
    const auto target_resolved =
        modules.resolve(runtime_physical, template_bytes.size()) != nullptr;
    const auto target_matches = modules.validate_bytes_at(cpu.memory,
                                                          runtime_physical,
                                                          runtime_physical,
                                                          template_bytes.size());
    require(target_resolved && !target_matches,
            "VBR-AOT-Fixture besitzt keinen abweichenden physischen Zielmodulalias.");

    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    static_cast<void>(blocks.register_static({source_address,
                                              canonical_physical_address(source_address),
                                              2u,
                                              BlockEndKind::Return,
                                              {},
                                              block,
                                              "overlapping-vbr-aot-source-block"}));
    const std::array templates{NativeAotTemplate{
        source_module.id,
        source_module.source_identity,
        source_address,
        static_cast<std::uint32_t>(template_bytes.size()),
        0x600,
        {}}};
    NativeAotTemplateBinder binder(cpu, modules, blocks, templates);
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        &tracker,
        {true, 4u, 128u},
        [&binder](const std::uint32_t target,
                  const std::uint32_t physical_origin,
                  const std::span<const std::uint8_t> snapshot,
                  const BlockVariantKey& variant) {
            auto bound = binder.bind(target, physical_origin, snapshot, variant);
            return std::move(bound.candidate);
        });
    const auto handle = materializer.try_materialize(
        cpu, runtime_address, runtime_physical, {}, 0x8C10D26Cu);
    const auto resolved = handle ? blocks.resolve(*handle) : std::nullopt;
    require(resolved.has_value() && resolved->get().aot_template.has_value() &&
                resolved->get().aot_template->mapping ==
                    CodeAddressMapping{source_address,
                                       runtime_address,
                                       static_cast<std::uint32_t>(template_bytes.size())} &&
                materializer.metrics().materializations == 1u &&
                materializer.metrics().byte_identity_failures == 0u &&
                materializer.validate_for_dispatch(
                    cpu, *handle, runtime_address, runtime_physical),
            "Binder-valides VBR-AOT wurde vom ueberlappenden Zielmodulalias verworfen.");

    RuntimeBlockTable non_aot_blocks;
    ExecutableCodeTracker non_aot_tracker;
    non_aot_blocks.bind_code_tracker(&non_aot_tracker);
    std::size_t non_aot_callbacks = 0u;
    DemandBlockMaterializer non_aot_materializer(
        modules,
        non_aot_blocks,
        &non_aot_tracker,
        {true, 4u, 128u},
        [&non_aot_callbacks](const std::uint32_t target,
                             const std::uint32_t physical_origin,
                             const std::span<const std::uint8_t>,
                             const BlockVariantKey& variant) {
            ++non_aot_callbacks;
            return MaterializedBlockCandidate{{target,
                                               physical_origin,
                                               2u,
                                               BlockEndKind::Return,
                                               variant,
                                               block,
                                               "overlapping-vbr-non-aot",
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
                                              2u};
        });
    require(!non_aot_materializer.try_materialize(
                 cpu, runtime_address, runtime_physical, {}, 0x8C10D26Cu) &&
                non_aot_callbacks == 1u &&
                non_aot_materializer.last_failure() ==
                    MaterializationFailure::ByteIdentityMismatch &&
                non_aot_materializer.metrics().materializations == 0u,
            "Non-AOT-Kandidat umging die abweichende Zielmodul-Byteidentitaet.");
}

void module_incarnation_aba_regression() {
    using namespace katana::runtime;

    CpuState cpu;
    const std::vector<std::uint8_t> first_source_bytes{
        0x09u, 0x00u, 0x0Bu, 0x00u};
    const std::vector<std::uint8_t> second_source_bytes{
        0x0Bu, 0x00u, 0x09u, 0x00u};
    const std::vector<std::uint8_t> third_source_bytes{
        0x09u, 0x00u, 0x09u, 0x00u};
    cpu.memory.write_bytes(0x1000u, first_source_bytes, CodeWriteSource::Copy);
    cpu.memory.write_bytes(0x2000u, first_source_bytes, CodeWriteSource::Copy);

    ExecutableModule source;
    source.id = "aot-incarnation-source";
    source.source_identity = "free-aot-incarnation-source-v1";
    source.guest_start = 0x1000u;
    source.bytes = first_source_bytes;
    ExecutableModule runtime_copy = source;
    runtime_copy.id = "aot-incarnation-runtime-copy";
    runtime_copy.source_identity = "free-aot-incarnation-runtime-copy-v1";
    runtime_copy.guest_start = 0x2000u;
    ExecutableModuleCatalog modules;
    modules.publish(source);
    modules.publish(runtime_copy);

    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    BlockMaterializationPolicy policy;
    policy.enabled = true;
    policy.max_blocks = 4u;
    policy.max_bytes = 64u;
    policy.max_memory_bytes = first_source_bytes.size();
    std::size_t callback_calls = 0u;
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        &tracker,
        policy,
        [&](const std::uint32_t target,
            const std::uint32_t physical_origin,
            const std::span<const std::uint8_t> snapshot,
            const BlockVariantKey& requested_variant) {
            ++callback_calls;
            MaterializedBlockCandidate candidate;
            candidate.block = {
                target,
                physical_origin,
                2u,
                BlockEndKind::Return,
                requested_variant,
                block,
                "aot-incarnation-template",
                false,
                RuntimeAotTemplateContract{{0x1000u, 0x2000u, 4u}, 4u}};
            candidate.decode_candidate_validated = snapshot.size() >= 2u;
            candidate.bounded_analysis_complete = true;
            candidate.ir_verified = true;
            candidate.code_generated = true;
            candidate.guest_cycles = 1u;
            candidate.instructions = 1u;
            candidate.recursive_seeds = 1u;
            candidate.peak_memory_bytes = first_source_bytes.size();
            return candidate;
        });
    IndirectDispatchMetrics dispatch_metrics;
    IndirectDispatchRequest request;
    request.kind = IndirectDispatchKind::TailJump;
    request.callsite = 0x80u;
    request.target = 0x2000u;
    request.dispatch_class = RuntimeDispatchClass::RuntimeOnly;
    request.metrics = &dispatch_metrics;
    request.materializer = &materializer;

    const auto first = dispatch_indirect(cpu, blocks, request);
    const auto first_block = blocks.resolve(first.block);
    require(first.materialized && first_block.has_value() &&
                modules.find(source.id)->generation == 1u,
            "Erste AOT-Modulinkarnation wurde nicht deterministisch gebunden.");
    const auto first_identity =
        stable_runtime_block_identity(first_block->get());

    modules.unload(source.id, blocks, tracker);
    require(blocks.active(first.block) && tracker.valid(first_identity),
            "Der ABA-Test invalidiert seinen getrennten AOT-Runtimebereich bereits beim "
            "Quell-Unload.");
    auto second_source = source;
    second_source.bytes = second_source_bytes;
    cpu.memory.write_bytes(
        second_source.guest_start, second_source.bytes, CodeWriteSource::Copy);
    modules.publish(second_source);
    require(modules.find(source.id)->generation == 2u,
            "Direktes Republish verwendet die alte Modulinkarnationsgeneration.");

    const auto second = dispatch_indirect(cpu, blocks, request);
    const auto second_block = blocks.resolve(second.block);
    require(second.materialized && second.block != first.block &&
                second_block.has_value() && !blocks.active(first.block) &&
                !tracker.valid(first_identity),
            "Republish reaktiviert einen stale AOT-Handle der alten Modulinkarnation.");
    const auto second_identity =
        stable_runtime_block_identity(second_block->get());

    modules.unload(source.id, blocks, tracker);
    require(blocks.active(second.block) && tracker.valid(second_identity),
            "Der Loaded-Range-ABA-Test verlor seinen getrennten AOT-Handle vorzeitig.");
    auto third_source = source;
    third_source.bytes = third_source_bytes;
    cpu.memory.write_bytes(
        third_source.guest_start, third_source.bytes, CodeWriteSource::Copy);
    modules.publish_loaded_range(third_source,
                                 blocks,
                                 tracker,
                                 LoadedRangeWriteObservation::ObservedChanged);
    require(modules.find(source.id)->generation == 3u,
            "Loaded-Range-Republish verwendet keine monotone Modulinkarnation.");

    const auto third = dispatch_indirect(cpu, blocks, request);
    const auto third_block = blocks.resolve(third.block);
    require(third.materialized && third.block != second.block &&
                third_block.has_value() && !blocks.active(second.block) &&
                !tracker.valid(second_identity) && callback_calls == 3u,
            "Loaded-Range-Republish behaelt einen stale AOT-Handle aktiv.");

    const auto snapshot = modules.snapshot();
    std::vector<std::pair<std::uint64_t, bool>> source_incarnations;
    for (const auto& module : snapshot.modules) {
        if (module.id == source.id)
            source_incarnations.emplace_back(module.generation, module.active);
    }
    require(source_incarnations ==
                std::vector<std::pair<std::uint64_t, bool>>{
                    {1u, false}, {2u, false}, {3u, true}},
            "Modulkatalog-Snapshot verliert die monotone Inkarnationshistorie.");

    ExecutableModuleCatalog exhausted_modules;
    RuntimeBlockTable exhausted_blocks;
    ExecutableCodeTracker exhausted_tracker;
    auto exhausted = source;
    exhausted.id = "exhausted-incarnation";
    exhausted.generation = std::numeric_limits<std::uint64_t>::max();
    exhausted_modules.publish(exhausted);
    exhausted_modules.unload(exhausted.id, exhausted_blocks, exhausted_tracker);
    bool exhausted_republish_rejected = false;
    try {
        exhausted.generation = 1u;
        exhausted_modules.publish(exhausted);
    } catch (const std::overflow_error&) {
        exhausted_republish_rejected = true;
    }
    const auto exhausted_snapshot = exhausted_modules.snapshot();
    require(exhausted_republish_rejected &&
                exhausted_modules.find(exhausted.id) == nullptr &&
                exhausted_snapshot.modules.size() == 1u &&
                exhausted_snapshot.modules.front().generation ==
                    std::numeric_limits<std::uint64_t>::max() &&
                !exhausted_snapshot.modules.front().active,
            "Ausgeschoepfte Modulinkarnation wird per Generation-ABA wiederverwendet.");
}

void multi_extent_module_lifecycle_regression() {
    using namespace katana::runtime;

    enum class LifecycleAction : std::uint8_t { Unload, Replace, Relocate };
    for (const auto action :
         {LifecycleAction::Unload, LifecycleAction::Replace, LifecycleAction::Relocate}) {
        constexpr std::uint32_t base = 0x10000u;
        constexpr std::uint32_t page = 0x1000u;
        CpuState cpu;
        const std::vector<std::uint8_t> bytes(page * 3u, 0x09u);
        cpu.memory.write_bytes(base, bytes, CodeWriteSource::Copy);

        ExecutableModule foreign;
        foreign.id = "multi-extent-foreign-hole";
        foreign.source_identity = "free-multi-extent-foreign-hole-v1";
        foreign.guest_start = base + page;
        foreign.bytes.assign(page, 0x09u);

        ExecutableModule owner;
        owner.id = "multi-extent-owner";
        owner.source_identity = "free-multi-extent-owner-v1";
        owner.guest_start = base;
        owner.bytes = bytes;

        ExecutableModuleCatalog modules;
        RuntimeBlockTable blocks;
        ExecutableCodeTracker tracker;
        blocks.bind_code_tracker(&tracker);
        modules.publish(foreign);
        modules.publish_loaded_range(
            owner, blocks, tracker, LoadedRangeWriteObservation::ObservedByteIdentical);
        const std::vector<ExecutableModuleActiveExtent> expected_extents{{0u, page * 3u}};
        require(modules.find(owner.id) != nullptr &&
                    modules.find(owner.id)->active_extents == expected_extents,
                "Byte-identisches Lochmodul verliert seinen ueberdeckten Mehrfach-Owner.");

        DemandBlockMaterializer materializer(
            modules,
            blocks,
            &tracker,
            {true, 4u, 16u},
            [](const std::uint32_t target,
               const std::uint32_t physical_origin,
               const std::span<const std::uint8_t> snapshot,
               const BlockVariantKey& requested_variant) {
                MaterializedBlockCandidate candidate;
                candidate.block = {target,
                                   physical_origin,
                                   2u,
                                   BlockEndKind::Return,
                                   requested_variant,
                                   block,
                                   "multi-extent-lifecycle-block",
                                   true};
                candidate.decode_candidate_validated = snapshot.size() >= 2u;
                candidate.interpreter_backed = true;
                candidate.guest_cycles = 1u;
                candidate.instructions = 1u;
                candidate.recursive_seeds = 1u;
                candidate.peak_memory_bytes = 2u;
                return candidate;
            });
        const auto foreign_handle =
            materializer.try_materialize(cpu, base + page, base + page, {}, 0x80u);
        const auto owner_handle =
            materializer.try_materialize(cpu, base, base, {}, 0x84u);
        require(foreign_handle.has_value() && owner_handle.has_value(),
                "Lochmodul-Lifecycle konnte seine getrennten Urspruenge nicht materialisieren.");
        const auto foreign_block = blocks.resolve(*foreign_handle);
        const auto owner_block = blocks.resolve(*owner_handle);
        require(foreign_block.has_value() && owner_block.has_value(),
                "Lochmodul-Lifecycle verlor einen vorbereiteten Runtimeblock.");
        const auto foreign_identity = stable_runtime_block_identity(foreign_block->get());
        const auto owner_identity = stable_runtime_block_identity(owner_block->get());
        const auto invalidated_before = modules.metrics().invalidated_blocks;

        switch (action) {
        case LifecycleAction::Unload:
            modules.unload(owner.id, blocks, tracker);
            break;
        case LifecycleAction::Replace: {
            auto replacement = owner;
            replacement.source_identity = "free-multi-extent-owner-v2";
            replacement.guest_start = base + page * 4u;
            replacement.bytes = {0x0Bu, 0x00u};
            replacement.active_extents.clear();
            modules.replace(std::move(replacement), blocks, tracker);
            break;
        }
        case LifecycleAction::Relocate:
            modules.update_relocations(owner.id, {}, blocks, tracker);
            break;
        }
        materializer.reconcile_inactive_origins();

        require(blocks.active(*foreign_handle) && !blocks.active(*owner_handle) &&
                    tracker.valid(foreign_identity) && !tracker.valid(owner_identity) &&
                    tracker.page_generation(base + page) == 0u &&
                    materializer.validate_for_dispatch(
                        cpu, *foreign_handle, base + page, base + page) &&
                    materializer.metrics().retained_validation_bytes == 2u &&
                    materializer.metrics().reclaimed_validation_bytes == 2u &&
                    modules.metrics().invalidated_blocks == invalidated_before + 1u,
                "Lochmodul-Lifecycle invalidiert fremden Code oder zaehlt denselben "
                "Block mehrfach.");
    }
}

void relocation_generation_overflow_regression() {
    using namespace katana::runtime;

    constexpr std::uint32_t source_address = 0x3000u;
    constexpr std::uint32_t runtime_address = 0x4000u;
    const std::vector<std::uint8_t> bytes{0x09u, 0x00u, 0x0Bu, 0x00u};
    CpuState cpu;
    cpu.memory.write_bytes(source_address, bytes, CodeWriteSource::Copy);
    cpu.memory.write_bytes(runtime_address, bytes, CodeWriteSource::Copy);

    ExecutableModule source;
    source.id = "relocation-generation-overflow-source";
    source.source_identity = "free-relocation-generation-overflow-source-v1";
    source.guest_start = source_address;
    source.bytes = bytes;
    source.relocation_generation = std::numeric_limits<std::uint64_t>::max();
    ExecutableModule runtime_copy = source;
    runtime_copy.id = "relocation-generation-overflow-runtime-copy";
    runtime_copy.source_identity = "free-relocation-generation-overflow-runtime-copy-v1";
    runtime_copy.guest_start = runtime_address;
    runtime_copy.relocation_generation = 1u;

    ExecutableModuleCatalog modules;
    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    modules.publish(source);
    modules.publish(runtime_copy);
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        &tracker,
        {true, 2u, 8u},
        [](const std::uint32_t target,
           const std::uint32_t physical_origin,
           const std::span<const std::uint8_t> snapshot,
           const BlockVariantKey& requested_variant) {
            MaterializedBlockCandidate candidate;
            candidate.block = {
                target,
                physical_origin,
                2u,
                BlockEndKind::Return,
                requested_variant,
                block,
                "relocation-generation-overflow-aot",
                false,
                RuntimeAotTemplateContract{{source_address, runtime_address, 4u}, 4u}};
            candidate.decode_candidate_validated = snapshot.size() >= 2u;
            candidate.bounded_analysis_complete = true;
            candidate.ir_verified = true;
            candidate.code_generated = true;
            candidate.guest_cycles = 1u;
            candidate.instructions = 1u;
            candidate.recursive_seeds = 1u;
            candidate.peak_memory_bytes = 4u;
            return candidate;
        });
    const auto handle =
        materializer.try_materialize(cpu, runtime_address, runtime_address, {}, 0x90u);
    require(handle.has_value(), "Relocation-Overflow konnte seinen AOT-Runtimecopy nicht binden.");
    const auto registered = blocks.resolve(*handle);
    require(registered.has_value(), "Relocation-Overflow verlor seinen AOT-Runtimeblock.");
    const auto identity = stable_runtime_block_identity(registered->get());
    const auto catalog_before = modules.snapshot();
    const auto materialization_before = materializer.metrics();

    bool overflow_rejected = false;
    try {
        modules.update_relocations(
            source.id,
            {{0u, executable_module_relocation_module_base32, 0}},
            blocks,
            tracker);
    } catch (const std::overflow_error&) {
        overflow_rejected = true;
    }

    require(overflow_rejected && modules.snapshot() == catalog_before &&
                blocks.active(*handle) && tracker.valid(identity) &&
                materializer.validate_for_dispatch(
                    cpu, *handle, runtime_address, runtime_address) &&
                materializer.metrics().retained_validation_bytes ==
                    materialization_before.retained_validation_bytes &&
                materializer.metrics().reclaimed_validation_bytes ==
                    materialization_before.reclaimed_validation_bytes,
            "Ausgeschoepfte Relocation-Generation mutiert Quelle oder stale AOT-Lifecycle.");
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
                                             const std::uint32_t,
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
    alias_cpu.write_sr(sr_md_mask);
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
                                                  const std::uint32_t,
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

void byte_identical_runtime_write_provenance_regression() {
    using namespace katana::runtime;

    CpuState cpu;
    ExecutableModuleCatalog modules;
    constexpr std::array guest_sources{CodeWriteSource::Cpu,
                                       CodeWriteSource::Fpu,
                                       CodeWriteSource::StoreQueue,
                                       CodeWriteSource::Fallback};
    constexpr std::array identical_load_sources{CodeWriteSource::Copy,
                                                CodeWriteSource::Dma,
                                                CodeWriteSource::Copy,
                                                CodeWriteSource::Dma};
    for (std::size_t index = 0u; index < guest_sources.size(); ++index) {
        const auto address = 0x1800u + static_cast<std::uint32_t>(index * 0x10u);
        cpu.memory.write_u32(address, 0x00090009u, CodeWriteSource::Copy);
        modules.record_runtime_write(address, 4u, guest_sources[index], false);
        modules.record_runtime_write(address, 4u, identical_load_sources[index], false);
        require(modules.promote_runtime_write(cpu.memory, address, 4u),
                "Byte-identischer echter Gastwrite setzt keine haltbare "
                "Runtimewrite-Provenienz.");
    }

    ExecutableModule stable;
    stable.id = "byte-identical-stable-module";
    stable.source_identity = "free-byte-identical-stable-module-v1";
    stable.guest_start = 0x2000u;
    stable.bytes = {0x09u, 0x00u, 0x0Bu, 0x00u};
    cpu.memory.write_bytes(stable.guest_start, stable.bytes, CodeWriteSource::Copy);
    modules.publish(stable);

    RuntimeBlockTable blocks;
    RuntimeBlock prefix_block{stable.guest_start,
                              stable.guest_start,
                              static_cast<std::uint32_t>(stable.bytes.size()),
                              BlockEndKind::Return,
                              {},
                              block,
                              "byte-identical-stable-prefix",
                              true};
    const auto prefix_handle = blocks.register_static(prefix_block);
    const auto generation = modules.find(stable.id)->generation;
    const auto extents = modules.find(stable.id)->active_extents;
    for (const auto source : guest_sources)
        modules.record_runtime_write(stable.guest_start, stable.bytes.size(), source, false);
    require(modules.find(stable.id) != nullptr &&
                modules.find(stable.id)->generation == generation &&
                modules.find(stable.id)->active_extents == extents &&
                blocks.active(prefix_handle),
            "Byte-identischer echter Gastwrite invalidiert Modulprefix oder Runtimeblock.");
}

void runtime_write_delay_slot_extension_regression() {
    using namespace katana::runtime;

    constexpr std::uint32_t base = 0x2400u;
    constexpr std::uint32_t delayed_branch = base + 126u;
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

    std::vector<std::uint8_t> bytes(132u);
    for (std::size_t offset = 0u; offset < bytes.size(); offset += 2u)
        bytes[offset] = 0x09u;
    bytes[126u] = 0x0Bu;
    bytes[127u] = 0x00u;
    bytes[128u] = 0x09u;
    bytes[129u] = 0x00u;
    cpu.memory.write_bytes(base, bytes, CodeWriteSource::Cpu);

    bool saw_delay_slot_snapshot = false;
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        &tracker,
        {true, 4u, 512u},
        [&](const std::uint32_t target,
            const std::uint32_t,
            const std::span<const std::uint8_t> snapshot,
            const BlockVariantKey& requested_variant) {
            const auto block_size = target == delayed_branch ? 4u : 2u;
            if (target == delayed_branch)
                saw_delay_slot_snapshot =
                    snapshot.size() >= block_size && snapshot[0u] == 0x0Bu &&
                    snapshot[1u] == 0x00u && snapshot[2u] == 0x09u &&
                    snapshot[3u] == 0x00u;
            return MaterializedBlockCandidate{{target,
                                               canonical_physical_address(target),
                                               block_size,
                                               BlockEndKind::Return,
                                               requested_variant,
                                               block,
                                               "runtime-write-delay-slot-decoder",
                                               true},
                                              snapshot.size() >= block_size,
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
    request.callsite = 0x100u;
    request.target = base;
    request.dispatch_class = RuntimeDispatchClass::RuntimeOnly;
    request.materializer = &materializer;
    const auto prefix = dispatch_indirect(cpu, blocks, request);
    const auto* initial = modules.resolve(base, 2u);
    require(initial != nullptr && initial->bytes.size() == 128u,
            "Initiale Runtimewrite-Promotion besitzt nicht das gebundene 128-Byte-Fenster.");
    const auto module_id = initial->id;
    const auto module_generation = initial->generation;
    const std::vector<std::uint8_t> prefix_bytes(initial->bytes.begin(), initial->bytes.end());

    request.callsite = 0x104u;
    request.target = delayed_branch;
    const auto delayed = dispatch_indirect(cpu, blocks, request);
    const auto* expanded = modules.resolve(delayed_branch, 4u);
    require(delayed.block && saw_delay_slot_snapshot && expanded != nullptr &&
                expanded->id == module_id && expanded->generation == module_generation &&
                expanded->bytes.size() == bytes.size() &&
                std::equal(prefix_bytes.begin(), prefix_bytes.end(), expanded->bytes.begin()) &&
                expanded->materializable(base + 128u, 2u) && blocks.active(prefix.block) &&
                materializer.validate_for_dispatch(cpu, prefix.block, base),
            "Runtimewrite-Modul wurde fuer einen Delay-Slot nicht sicher und "
            "prefixstabil erweitert.");
}

void identity_preserving_loaded_range_regression() {
    using namespace katana::runtime;
    CpuState cpu;
    constexpr std::uint32_t static_address = 0x2000u;
    constexpr std::uint32_t byte_identical_new_address = 0x3000u;
    constexpr std::uint32_t new_address = 0x4000u;
    const std::vector<std::uint8_t> original{0x09u, 0x00u, 0x0Bu, 0x00u};
    cpu.memory.write_bytes(static_address, original, CodeWriteSource::Copy);

    ExecutableModuleCatalog modules;
    ExecutableModule original_module;
    original_module.id = "static-aot-source";
    original_module.source_identity = "free-static-aot-source-v1";
    original_module.guest_start = static_address;
    original_module.bytes = original;
    modules.publish(original_module);

    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    RuntimeBlock compiled;
    compiled.virtual_start = static_address;
    compiled.physical_origin = static_address;
    compiled.size = static_cast<std::uint32_t>(original.size());
    compiled.end_kind = BlockEndKind::Return;
    compiled.function = block;
    compiled.provenance = "free-static-aot-block-v1";
    const auto compiled_identity = stable_runtime_block_identity(compiled);
    const auto compiled_handle = blocks.register_static(compiled);
    require(tracker.register_block({compiled_identity,
                                    static_address,
                                    static_cast<std::uint32_t>(original.size()),
                                    "free-static-aot-block-v1",
                                    {},
                                    ExecutableBlockOrigin::ImageSegment}) ==
                BlockRegistrationResult::Inserted,
            "Statischer AOT-Trackerblock wurde nicht registriert.");

    ExecutableLoadWriteTracker load_writes;
    cpu.memory.set_guest_write_observer([&](const GuestWriteEvent& event) {
        load_writes.observe(event);
        modules.record_runtime_write(event.address, event.size, event.source, event.bytes_changed);
        const auto invalidation =
            tracker.observe_write(event.address, event.size, event.source, event.bytes_changed);
        if (!invalidation.byte_identical)
            static_cast<void>(blocks.erase_overlapping_physical(event.address, event.size));
    });

    // The BIOS PIO path emits adjacent halfword writes. Their combined byte-identity must survive
    // until the one module-load callback for the complete transfer.
    cpu.memory.write_u16(static_address, 0x0009u, CodeWriteSource::Copy);
    cpu.memory.write_u16(static_address + 2u, 0x000Bu, CodeWriteSource::Copy);
    ExecutableModule identical = original_module;
    identical.id = "identical-disc-reload";
    identical.source_identity = "free-identical-disc-reload-v1";
    modules.publish_loaded_range(
        identical, blocks, tracker, load_writes.consume(static_address, original.size()));
    require(modules.find(identical.id) != nullptr &&
                modules.find(identical.id)->active_extents ==
                    std::vector<ExecutableModuleActiveExtent>{{0u, 4u}} &&
                modules.find(original_module.id) != nullptr &&
                blocks.active(compiled_handle) && tracker.valid(compiled_identity) &&
                tracker.invalidation_count() == 0u &&
                tracker.page_generation(static_address) == 0u,
            "Byte-identischer Disc-Reload invalidiert statischen AOT-Code oder verliert seine "
            "eigene Modullebenszeit.");

    const std::vector<std::uint8_t> byte_identical_new_bytes(4u, 0u);
    cpu.memory.write_bytes(byte_identical_new_address,
                           byte_identical_new_bytes,
                           CodeWriteSource::Dma);
    ExecutableModule byte_identical_new;
    byte_identical_new.id = "new-byte-identical-disc-load";
    byte_identical_new.source_identity = "free-new-byte-identical-disc-load-v1";
    byte_identical_new.guest_start = byte_identical_new_address;
    byte_identical_new.bytes = byte_identical_new_bytes;
    modules.publish_loaded_range(
        byte_identical_new,
        blocks,
        tracker,
        load_writes.consume(byte_identical_new_address, byte_identical_new_bytes.size()));
    require(modules.resolve(byte_identical_new_address, byte_identical_new_bytes.size()) != nullptr &&
                modules.resolve(byte_identical_new_address, byte_identical_new_bytes.size())->id ==
                    byte_identical_new.id &&
                tracker.page_generation(byte_identical_new_address) == 0u,
            "Neuer bereits bytegleicher Disc-Load verliert seinen Modulnachweis.");

    const std::vector<std::uint8_t> newly_loaded{0x09u, 0x00u, 0x09u, 0x00u};
    cpu.memory.write_bytes(new_address, newly_loaded, CodeWriteSource::Dma);
    ExecutableModule fresh;
    fresh.id = "new-disc-load";
    fresh.source_identity = "free-new-disc-load-v1";
    fresh.guest_start = new_address;
    fresh.bytes = newly_loaded;
    modules.publish_loaded_range(
        fresh, blocks, tracker, load_writes.consume(new_address, newly_loaded.size()));
    require(modules.resolve(new_address, newly_loaded.size()) != nullptr &&
                modules.resolve(new_address, newly_loaded.size())->id == fresh.id,
            "Neuer beobachteter Disc-Load wird nicht als Modul publiziert.");

    auto changed = original;
    changed[0] = 0x0Bu;
    cpu.memory.write_bytes(static_address, changed, CodeWriteSource::Copy);
    ExecutableModule replacement;
    replacement.id = "changed-disc-reload";
    replacement.source_identity = "free-changed-disc-reload-v1";
    replacement.guest_start = static_address;
    replacement.bytes = changed;
    modules.publish_loaded_range(
        replacement, blocks, tracker, load_writes.consume(static_address, changed.size()));
    require(!blocks.active(compiled_handle) && !tracker.valid(compiled_identity) &&
                tracker.invalidation_count() == 1u &&
                tracker.page_generation(static_address) == 1u &&
                modules.resolve(static_address, changed.size()) != nullptr &&
                modules.resolve(static_address, changed.size())->id == replacement.id,
            "Geaenderter Disc-Reload invalidiert AOT-Code nicht exakt einmal oder verliert sein "
            "neues Modul.");
}

void identity_preserving_loaded_range_overlap_regression() {
    using namespace katana::runtime;

    {
        ExecutableModuleCatalog modules;
        RuntimeBlockTable blocks;
        ExecutableCodeTracker tracker;
        ExecutableModule original;
        original.id = "partial-overlap-source";
        original.source_identity = "free-partial-overlap-source-v1";
        original.guest_start = 0x00008000u;
        original.bytes = {0x10u, 0x11u, 0x12u, 0x13u};
        modules.publish(original);

        ExecutableModule incoming;
        incoming.id = "partial-overlap-tail";
        incoming.source_identity = "free-partial-overlap-tail-v1";
        incoming.guest_start = 0x80008002u;
        incoming.bytes = {0x12u, 0x13u, 0x14u, 0x15u};
        modules.publish_loaded_range(incoming,
                                     blocks,
                                     tracker,
                                     LoadedRangeWriteObservation::ObservedByteIdentical);

        const auto* published = modules.find(incoming.id);
        require(published != nullptr &&
                    published->active_extents ==
                        std::vector<ExecutableModuleActiveExtent>{{0u, 4u}} &&
                    modules.resolve(0x00008000u, 4u) == modules.find(original.id) &&
                    modules.resolve(0x00008002u, 2u) == modules.find(original.id) &&
                    modules.resolve(0x00008004u, 2u) == published &&
                    modules.metrics().loads == 2u,
                "Teilweise byte-identische Aliasueberdeckung verliert den ueberdeckten "
                "Mehrfach-Owner.");
    }

    {
        ExecutableModuleCatalog modules;
        RuntimeBlockTable blocks;
        ExecutableCodeTracker tracker;
        ExecutableModule original;
        original.id = "superset-overlap-source";
        original.source_identity = "free-superset-overlap-source-v1";
        original.guest_start = 0x00009002u;
        original.bytes = {0x22u, 0x23u, 0x24u, 0x25u};
        modules.publish(original);

        ExecutableModule incoming;
        incoming.id = "superset-overlap-edges";
        incoming.source_identity = "free-superset-overlap-edges-v1";
        incoming.guest_start = 0x00009000u;
        incoming.bytes = {0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u};
        modules.publish_loaded_range(incoming,
                                     blocks,
                                     tracker,
                                     LoadedRangeWriteObservation::ObservedByteIdentical);

        const auto* published = modules.find(incoming.id);
        require(published != nullptr &&
                    published->active_extents ==
                        std::vector<ExecutableModuleActiveExtent>{{0u, 8u}} &&
                    modules.resolve(0x00009000u, 2u) == published &&
                    modules.resolve(0x00009002u, 4u) == modules.find(original.id) &&
                    modules.resolve(0x00009006u, 2u) == published &&
                    modules.resolve(0x00009000u, 8u) == published,
                "Byte-identischer Superset-Load verliert seinen ueberdeckten "
                "Mehrfach-Owner.");
    }

    {
        ExecutableModuleCatalog modules;
        RuntimeBlockTable blocks;
        ExecutableCodeTracker tracker;
        ExecutableModule left;
        left.id = "union-left";
        left.source_identity = "free-union-left-v1";
        left.guest_start = 0x0000A000u;
        left.bytes = {0x30u, 0x31u};
        modules.publish(left);
        ExecutableModule right;
        right.id = "union-right";
        right.source_identity = "free-union-right-v1";
        right.guest_start = 0x8000A002u;
        right.bytes = {0x32u, 0x33u};
        modules.publish(right);

        RuntimeBlock guarded_block{0x0000A000u,
                                   0x0000A000u,
                                   4u,
                                   BlockEndKind::Return,
                                   {},
                                   block,
                                   "union-byte-identical-guard",
                                   true};
        const auto guarded_identity = stable_runtime_block_identity(guarded_block);
        const auto guarded_handle = blocks.register_runtime(guarded_block);
        require(tracker.register_block({guarded_identity,
                                        0x0000A000u,
                                        4u,
                                        guarded_block.provenance,
                                        {},
                                        ExecutableBlockOrigin::RuntimeWrite}) ==
                    BlockRegistrationResult::Inserted,
                "Union-No-op-Test konnte seinen Trackerblock nicht registrieren.");
        blocks.bind_code_tracker(&tracker);
        const auto catalog_before = modules.snapshot();
        const auto tracker_generation_before = tracker.page_generation(0x0000A000u);
        const auto tracker_invalidations_before = tracker.invalidation_count();

        ExecutableModule incoming;
        incoming.id = "union-fully-covered";
        incoming.source_identity = "free-union-fully-covered-v1";
        incoming.guest_start = 0xA000A000u;
        incoming.bytes = {0x30u, 0x31u, 0x32u, 0x33u};
        modules.publish_loaded_range(incoming,
                                     blocks,
                                     tracker,
                                     LoadedRangeWriteObservation::ObservedByteIdentical);

        const auto* published = modules.find(incoming.id);
        require(published != nullptr &&
                    published->active_extents ==
                        std::vector<ExecutableModuleActiveExtent>{{0u, 4u}} &&
                    modules.snapshot().modules.size() == catalog_before.modules.size() + 1u &&
                    blocks.active(guarded_handle) && tracker.valid(guarded_identity) &&
                    tracker.page_generation(0x0000A000u) == tracker_generation_before &&
                    tracker.invalidation_count() == tracker_invalidations_before,
                "Vollstaendige byte-identische Union-Coverage verliert ihren Owner oder "
                "invalidiert Blocktabelle beziehungsweise Tracker-Generation.");
    }

    {
        ExecutableModuleCatalog modules;
        RuntimeBlockTable blocks;
        ExecutableCodeTracker tracker;
        ExecutableModule original;
        original.id = "same-id-active";
        original.source_identity = "free-same-id-active-v1";
        original.guest_start = 0x0000B002u;
        original.bytes = {0x42u, 0x43u};
        modules.publish(original);

        RuntimeBlock guarded_block{0x0000B000u,
                                   0x0000B000u,
                                   6u,
                                   BlockEndKind::Return,
                                   {},
                                   block,
                                   "same-id-rejection-guard",
                                   true};
        const auto guarded_identity = stable_runtime_block_identity(guarded_block);
        const auto guarded_handle = blocks.register_runtime(guarded_block);
        require(tracker.register_block({guarded_identity,
                                        0x0000B000u,
                                        6u,
                                        guarded_block.provenance,
                                        {},
                                        ExecutableBlockOrigin::RuntimeWrite}) ==
                    BlockRegistrationResult::Inserted,
                "Same-ID-Test konnte seinen Trackerblock nicht registrieren.");
        blocks.bind_code_tracker(&tracker);
        const auto catalog_before = modules.snapshot();
        const auto tracker_generation_before = tracker.page_generation(0x0000B000u);
        const auto tracker_invalidations_before = tracker.invalidation_count();

        ExecutableModule incoming;
        incoming.id = original.id;
        incoming.source_identity = "free-same-id-superset-v2";
        incoming.guest_start = 0x0000B000u;
        incoming.bytes = {0x40u, 0x41u, 0x42u, 0x43u, 0x44u, 0x45u};
        bool rejected = false;
        try {
            modules.publish_loaded_range(incoming,
                                         blocks,
                                         tracker,
                                         LoadedRangeWriteObservation::ObservedByteIdentical);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }

        require(rejected && modules.snapshot() == catalog_before &&
                    blocks.active(guarded_handle) && tracker.valid(guarded_identity) &&
                    tracker.page_generation(0x0000B000u) == tracker_generation_before &&
                    tracker.invalidation_count() == tracker_invalidations_before,
                "Byte-identischer Same-ID-Superset wird nicht atomar abgelehnt.");
    }

    {
        ExecutableModuleCatalog modules;
        RuntimeBlockTable blocks;
        ExecutableCodeTracker tracker;
        ExecutableModule original;
        original.id = "conflicting-overlap-source";
        original.source_identity = "free-conflicting-overlap-source-v1";
        original.guest_start = 0x0000C000u;
        original.bytes = {0x50u, 0x51u, 0x52u, 0x53u};
        modules.publish(original);
        const auto catalog_before = modules.snapshot();
        const auto tracker_generation_before = tracker.page_generation(0x0000C000u);
        const auto tracker_invalidations_before = tracker.invalidation_count();

        ExecutableModule incoming;
        incoming.id = "conflicting-overlap";
        incoming.source_identity = "free-conflicting-overlap-v1";
        incoming.guest_start = 0x0000C002u;
        incoming.bytes = {0xFFu, 0x53u, 0x54u, 0x55u};
        bool rejected = false;
        try {
            modules.publish_loaded_range(incoming,
                                         blocks,
                                         tracker,
                                         LoadedRangeWriteObservation::ObservedByteIdentical);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }

        require(rejected && modules.snapshot() == catalog_before &&
                    modules.find(incoming.id) == nullptr &&
                    tracker.page_generation(0x0000C000u) == tracker_generation_before &&
                    tracker.invalidation_count() == tracker_invalidations_before,
                "Widerspruechliche Byte-identisch-Metadaten werden nicht atomar abgelehnt.");
    }
}

void byte_identical_module_owner_lifecycle_regression() {
    using namespace katana::runtime;

    const auto make_module = [](std::string id,
                                std::string source_identity,
                                const std::uint32_t guest_start,
                                std::vector<std::uint8_t> bytes) {
        ExecutableModule module;
        module.id = std::move(id);
        module.source_identity = std::move(source_identity);
        module.guest_start = guest_start;
        module.bytes = std::move(bytes);
        return module;
    };
    const auto install_guard = [](RuntimeBlockTable& blocks,
                                  ExecutableCodeTracker& tracker,
                                  const std::uint32_t address,
                                  const std::uint32_t size,
                                  std::string provenance) {
        RuntimeBlock guarded{address,
                             canonical_physical_address(address),
                             size,
                             BlockEndKind::Return,
                             {},
                             block,
                             std::move(provenance),
                             true};
        const auto identity = stable_runtime_block_identity(guarded);
        const auto handle = blocks.register_static(guarded);
        require(tracker.register_block({identity,
                                        canonical_physical_address(address),
                                        size,
                                        guarded.provenance,
                                        {},
                                        ExecutableBlockOrigin::ImageSegment}) ==
                    BlockRegistrationResult::Inserted,
                "Mehrfach-Owner-Test konnte seinen AOT-Trackerblock nicht registrieren.");
        return std::pair{handle, identity};
    };

    {
        ExecutableModuleCatalog modules;
        RuntimeBlockTable blocks;
        ExecutableCodeTracker tracker;
        blocks.bind_code_tracker(&tracker);
        const std::vector<std::uint8_t> bytes{
            0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u};
        auto owner_a =
            make_module("identical-owner-a", "free-identical-owner-a-v1", 0x00010000u, bytes);
        auto owner_b =
            make_module("identical-owner-b", "free-identical-owner-b-v1", 0x80010000u, bytes);
        modules.publish(owner_a);
        const auto [guard, identity] =
            install_guard(blocks, tracker, owner_a.guest_start, 4u, "identical-owner-aot");
        modules.publish_loaded_range(owner_b,
                                     blocks,
                                     tracker,
                                     LoadedRangeWriteObservation::ObservedByteIdentical);

        require(modules.find(owner_a.id) != nullptr && modules.find(owner_b.id) != nullptr &&
                    modules.find(owner_a.id)->active_extents ==
                        std::vector<ExecutableModuleActiveExtent>{{0u, 8u}} &&
                    modules.find(owner_b.id)->active_extents ==
                        std::vector<ExecutableModuleActiveExtent>{{0u, 8u}} &&
                    modules.snapshot().active_extent_page_refcounts ==
                    std::vector<ExecutableModulePageRefcountSnapshot>{{0x00010000u, 2u}},
                "Byteidentische P0/P1-Module besitzen keine zwei aktiven Owner.");

        auto replacement = owner_a;
        replacement.source_identity = "free-identical-owner-a-v2";
        modules.replace(replacement, blocks, tracker);
        require(modules.find(owner_a.id) != nullptr &&
                    modules.find(owner_a.id)->generation == 2u &&
                    modules.find(owner_a.id)->source_identity == replacement.source_identity &&
                    modules.find(owner_b.id) != nullptr && blocks.active(guard) &&
                    tracker.valid(identity) && tracker.invalidation_count() == 0u &&
                    modules.snapshot().active_extent_page_refcounts ==
                        std::vector<ExecutableModulePageRefcountSnapshot>{{0x00010000u, 2u}},
                "Byteidentischer In-place-Ersatz wird vom Peer verworfen oder invalidiert "
                "dessen weiterhin gedeckte AOT-Bindung.");

        modules.unload(owner_a.id, blocks, tracker);
        require(modules.find(owner_a.id) == nullptr && modules.find(owner_b.id) != nullptr &&
                    modules.resolve(0x00010000u, bytes.size()) == modules.find(owner_b.id) &&
                    blocks.active(guard) && tracker.valid(identity) &&
                    tracker.invalidation_count() == 0u,
                "Unload des ersten P0/P1-Owners entfernt die weiter gedeckte AOT-Bindung.");

        modules.unload(owner_b.id, blocks, tracker);
        require(modules.resolve(0x00010000u, bytes.size()) == nullptr &&
                    !blocks.active(guard) && !tracker.valid(identity) &&
                    tracker.invalidation_count() == 1u,
                "Letzter P0/P1-Owner entfernt Provenienz oder AOT-Bindung nicht exakt.");
    }

    {
        ExecutableModuleCatalog modules;
        RuntimeBlockTable blocks;
        ExecutableCodeTracker tracker;
        blocks.bind_code_tracker(&tracker);
        const std::vector<std::uint8_t> bytes{0x09u, 0x00u, 0x0Bu, 0x00u};
        auto owner_a =
            make_module("reverse-owner-a", "free-reverse-owner-a-v1", 0x00012000u, bytes);
        auto owner_b =
            make_module("reverse-owner-b", "free-reverse-owner-b-v1", 0xA0012000u, bytes);
        modules.publish(owner_a);
        const auto [guard, identity] =
            install_guard(blocks, tracker, owner_a.guest_start, 4u, "reverse-owner-aot");
        modules.publish_loaded_range(owner_b,
                                     blocks,
                                     tracker,
                                     LoadedRangeWriteObservation::ObservedByteIdentical);

        modules.unload(owner_b.id, blocks, tracker);
        require(modules.find(owner_a.id) != nullptr && modules.find(owner_b.id) == nullptr &&
                    modules.resolve(0xA0012000u, bytes.size()) == modules.find(owner_a.id) &&
                    blocks.active(guard) && tracker.valid(identity) &&
                    tracker.invalidation_count() == 0u,
                "Umgekehrte P0/P2-Entladereihenfolge entfernt den verbleibenden Owner.");
        modules.unload(owner_a.id, blocks, tracker);
        require(!blocks.active(guard) && !tracker.valid(identity) &&
                    tracker.invalidation_count() == 1u,
                "Umgekehrte Entladereihenfolge behaelt die letzte AOT-Bindung.");
    }

    {
        ExecutableModuleCatalog modules;
        RuntimeBlockTable blocks;
        ExecutableCodeTracker tracker;
        blocks.bind_code_tracker(&tracker);
        const std::vector<std::uint8_t> bytes{0x09u, 0x00u, 0x0Bu, 0x00u};
        auto owner_p0 =
            make_module("alias-owner-p0", "free-alias-owner-p0-v1", 0x00014000u, bytes);
        auto owner_p1 =
            make_module("alias-owner-p1", "free-alias-owner-p1-v1", 0x80014000u, bytes);
        auto owner_p2 =
            make_module("alias-owner-p2", "free-alias-owner-p2-v1", 0xA0014000u, bytes);
        modules.publish(owner_p0);
        const auto [guard, identity] =
            install_guard(blocks, tracker, owner_p0.guest_start, 4u, "three-alias-owner-aot");
        modules.publish_loaded_range(owner_p1,
                                     blocks,
                                     tracker,
                                     LoadedRangeWriteObservation::ObservedByteIdentical);
        modules.publish_loaded_range(owner_p2,
                                     blocks,
                                     tracker,
                                     LoadedRangeWriteObservation::ObservedByteIdentical);
        require(modules.snapshot().active_extent_page_refcounts ==
                    std::vector<ExecutableModulePageRefcountSnapshot>{{0x00014000u, 3u}},
                "P0/P1/P2-Aliase werden nicht als drei physische Owner gezaehlt.");

        modules.unload(owner_p0.id, blocks, tracker);
        modules.unload(owner_p1.id, blocks, tracker);
        require(modules.resolve(0x80014000u, bytes.size()) == modules.find(owner_p2.id) &&
                    blocks.active(guard) && tracker.valid(identity) &&
                    tracker.invalidation_count() == 0u,
                "P0/P1-Unload entfernt den verbleibenden P2-Owner.");
        modules.unload(owner_p2.id, blocks, tracker);
        require(!blocks.active(guard) && !tracker.valid(identity) &&
                    tracker.invalidation_count() == 1u,
                "Letzter P2-Aliasowner behaelt eine veraltete AOT-Bindung.");
    }

    {
        ExecutableModuleCatalog modules;
        RuntimeBlockTable blocks;
        ExecutableCodeTracker tracker;
        blocks.bind_code_tracker(&tracker);
        auto owner_a = make_module("partial-owner-a",
                                   "free-partial-owner-a-v1",
                                   0x00016000u,
                                   {0x00u,
                                    0x01u,
                                    0x02u,
                                    0x03u,
                                    0x04u,
                                    0x05u,
                                    0x06u,
                                    0x07u});
        auto owner_b = make_module("partial-owner-b",
                                   "free-partial-owner-b-v1",
                                   0x80016004u,
                                   {0x04u,
                                    0x05u,
                                    0x06u,
                                    0x07u,
                                    0x08u,
                                    0x09u,
                                    0x0Au,
                                    0x0Bu});
        modules.publish(owner_a);
        const auto [prefix, prefix_identity] =
            install_guard(blocks, tracker, 0x00016000u, 4u, "partial-owner-prefix");
        const auto [overlap, overlap_identity] =
            install_guard(blocks, tracker, 0x00016004u, 4u, "partial-owner-overlap");
        const auto [tail, tail_identity] =
            install_guard(blocks, tracker, 0x00016008u, 4u, "partial-owner-tail");
        modules.publish_loaded_range(owner_b,
                                     blocks,
                                     tracker,
                                     LoadedRangeWriteObservation::ObservedByteIdentical);
        require(modules.find(owner_b.id)->active_extents ==
                    std::vector<ExecutableModuleActiveExtent>{{0u, 8u}},
                "Partielle Ueberlappung schneidet den gemeinsamen Ownerbereich weiterhin ab.");

        modules.unload(owner_a.id, blocks, tracker);
        require(!blocks.active(prefix) && !tracker.valid(prefix_identity) &&
                    blocks.active(overlap) && tracker.valid(overlap_identity) &&
                    blocks.active(tail) && tracker.valid(tail_identity) &&
                    modules.resolve(0x00016004u, 4u) == modules.find(owner_b.id) &&
                    tracker.invalidation_count() == 1u,
                "Partieller Owner-Unload invalidiert nicht exakt den exklusiven Prefix.");
        modules.unload(owner_b.id, blocks, tracker);
        require(!blocks.active(overlap) && !tracker.valid(overlap_identity) &&
                    !blocks.active(tail) && !tracker.valid(tail_identity) &&
                    tracker.invalidation_count() == 3u,
                "Letzter partieller Owner behaelt Overlap oder exklusiven Tail.");
    }
}

void runtime_aot_owner_failover_regression() {
    using namespace katana::runtime;

    constexpr std::uint32_t address = 0x00018000u;
    const std::vector<std::uint8_t> bytes{0x09u, 0x00u, 0x0Bu, 0x00u};
    CpuState cpu;
    cpu.memory.write_bytes(address, bytes, CodeWriteSource::Copy);
    ExecutableModule owner_a;
    owner_a.id = "runtime-aot-owner-a";
    owner_a.source_identity = "free-runtime-aot-owner-a-v1";
    owner_a.guest_start = address;
    owner_a.bytes = bytes;
    ExecutableModule owner_b = owner_a;
    owner_b.id = "runtime-aot-owner-b";
    owner_b.source_identity = "free-runtime-aot-owner-b-v1";
    owner_b.guest_start = 0x80018000u;

    ExecutableModuleCatalog modules;
    modules.publish(owner_a);
    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    BlockMaterializationPolicy policy;
    policy.enabled = true;
    policy.max_blocks = 2u;
    policy.max_bytes = 16u;
    policy.max_guest_cycles = 16u;
    policy.max_instructions = 8u;
    policy.max_recursive_seeds = 4u;
    policy.max_analysis_time_ms = 100u;
    policy.max_memory_bytes = 64u;
    policy.max_materializations_per_run = 2u;
    policy.max_repeated_misses_per_target = 2u;
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        &tracker,
        policy,
        [](const std::uint32_t target,
           const std::uint32_t physical,
           const std::span<const std::uint8_t>,
           const BlockVariantKey& variant) {
            MaterializedBlockCandidate candidate;
            candidate.block = {target,
                               physical,
                               2u,
                               BlockEndKind::Return,
                               variant,
                               block,
                               "runtime-aot-owner-failover",
                               true};
            candidate.decode_candidate_validated = true;
            candidate.bounded_analysis_complete = true;
            candidate.ir_verified = true;
            candidate.code_generated = true;
            candidate.guest_cycles = 1u;
            candidate.instructions = 1u;
            candidate.recursive_seeds = 1u;
            candidate.peak_memory_bytes = 4u;
            return candidate;
        });
    const auto handle =
        materializer.try_materialize(cpu, address, address, BlockVariantKey{}, 0x00017FF0u);
    require(handle.has_value() && blocks.active(*handle),
            "Runtime-AOT-Fixture konnte ihre erste Ownerbindung nicht materialisieren.");

    modules.publish_loaded_range(owner_b,
                                 blocks,
                                 tracker,
                                 LoadedRangeWriteObservation::ObservedByteIdentical);
    modules.unload(owner_a.id, blocks, tracker);
    materializer.reconcile_inactive_origins();
    require(blocks.active(*handle) &&
                materializer.validate_for_dispatch(cpu, *handle, address, address) &&
                materializer.last_failure() == MaterializationFailure::None,
            "Runtime-AOT-Bindung faellt beim ersten byteidentischen Owner-Unload aus.");

    modules.unload(owner_b.id, blocks, tracker);
    materializer.reconcile_inactive_origins();
    require(!blocks.active(*handle) &&
                materializer.metrics().retained_validation_bytes == 0u,
            "Runtime-AOT-Bindung ueberlebt den letzten physisch aktiven Owner.");
}

void relocation_owner_failover_regression() {
    using namespace katana::runtime;

    constexpr std::uint32_t address = 0x0001A000u;
    const std::vector<std::uint8_t> source{0x00u, 0x00u, 0x00u, 0x00u, 0x0Bu, 0x00u};
    const std::vector<std::uint8_t> relocated{0x00u, 0xA0u, 0x01u, 0x00u, 0x0Bu, 0x00u};
    CpuState cpu;
    cpu.memory.write_bytes(address, relocated, CodeWriteSource::Copy);

    ExecutableModule owner_a;
    owner_a.id = "relocation-owner-a";
    owner_a.source_identity = "free-relocation-owner-a-v1";
    owner_a.guest_start = address;
    owner_a.bytes = source;
    owner_a.relocations = {{0u, executable_module_relocation_module_base32, 0}};
    auto owner_b = owner_a;
    owner_b.id = "relocation-owner-b";
    owner_b.source_identity = "free-relocation-owner-b-v1";

    ExecutableModuleCatalog modules;
    modules.publish(owner_a);
    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    BlockMaterializationPolicy policy;
    policy.enabled = true;
    policy.max_blocks = 2u;
    policy.max_bytes = 16u;
    policy.max_guest_cycles = 16u;
    policy.max_instructions = 8u;
    policy.max_recursive_seeds = 4u;
    policy.max_analysis_time_ms = 100u;
    policy.max_memory_bytes = 64u;
    policy.max_materializations_per_run = 2u;
    policy.max_repeated_misses_per_target = 2u;
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        &tracker,
        policy,
        [](const std::uint32_t target,
           const std::uint32_t physical,
           const std::span<const std::uint8_t>,
           const BlockVariantKey& variant) {
            return MaterializedBlockCandidate{{target,
                                               physical,
                                               2u,
                                               BlockEndKind::Return,
                                               variant,
                                               block,
                                               "relocation-owner-failover",
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
                                              4u};
        });
    const auto handle =
        materializer.try_materialize(cpu, address, address, BlockVariantKey{}, 0x00019FF0u);
    require(handle.has_value() && blocks.active(*handle),
            "Relocation-Mehrfachowner konnte seinen Ausgangsblock nicht materialisieren.");
    modules.publish_loaded_range(owner_b,
                                 blocks,
                                 tracker,
                                 LoadedRangeWriteObservation::ObservedByteIdentical);

    modules.update_relocations(
        owner_a.id, {{0u, executable_module_relocation_module_base32, 4}}, blocks, tracker);
    materializer.reconcile_inactive_origins();
    require(blocks.active(*handle) &&
                materializer.validate_for_dispatch(cpu, *handle, address, address) &&
                materializer.last_failure() == MaterializationFailure::None &&
                tracker.invalidation_count() == 0u,
            "Relocation-Generation des ersten Owners bindet nicht auf den byteidentischen "
            "Peer um.");

    modules.update_relocations(
        owner_b.id, {{0u, executable_module_relocation_module_base32, 8}}, blocks, tracker);
    materializer.reconcile_inactive_origins();
    require(!blocks.active(*handle) && tracker.invalidation_count() == 1u,
            "Relocation-Origin ueberlebt ohne byteidentischen Owner.");
}

void multi_owner_selection_regression() {
    using namespace katana::runtime;

    const std::vector<std::uint8_t> bytes{0x09u, 0x00u, 0x0Bu, 0x00u};
    {
        ExecutableModule denied;
        denied.id = "permission-shadow-denied";
        denied.source_identity = "free-permission-shadow-denied-v1";
        denied.guest_start = 0x0001C000u;
        denied.bytes = bytes;
        denied.executable_permission = false;
        auto executable = denied;
        executable.id = "permission-shadow-executable";
        executable.source_identity = "free-permission-shadow-executable-v1";
        executable.executable_permission = true;

        ExecutableModuleCatalog modules;
        RuntimeBlockTable blocks;
        ExecutableCodeTracker tracker;
        modules.publish(denied);
        modules.publish_loaded_range(executable,
                                     blocks,
                                     tracker,
                                     LoadedRangeWriteObservation::ObservedByteIdentical);
        require(modules.resolve(denied.guest_start, 2u) == modules.find(executable.id) &&
                    modules.authorize_control_transfer(denied.guest_start),
                "Nichtausfuehrbarer erster Owner schattet den gueltigen spaeteren Owner ab.");
    }

    {
        constexpr std::uint32_t address = 0x0001E000u;
        CpuState cpu;
        cpu.memory.write_bytes(address, bytes, CodeWriteSource::Copy);
        ExecutableModule proven_data;
        proven_data.id = "role-shadow-proven-data";
        proven_data.source_identity = "free-role-shadow-proven-data-v1";
        proven_data.guest_start = address;
        proven_data.bytes = bytes;
        proven_data.range_roles = {
            {0u, static_cast<std::uint32_t>(bytes.size()), ExecutableStorageRole::ProvenData}};
        auto promotable = proven_data;
        promotable.id = "role-shadow-promotable";
        promotable.source_identity = "free-role-shadow-promotable-v1";
        promotable.executable_permission = false;
        promotable.control_transfer_promotion_allowed = true;

        ExecutableModuleCatalog modules;
        RuntimeBlockTable blocks;
        ExecutableCodeTracker tracker;
        blocks.bind_code_tracker(&tracker);
        modules.publish(proven_data);
        modules.publish_loaded_range(promotable,
                                     blocks,
                                     tracker,
                                     LoadedRangeWriteObservation::ObservedByteIdentical);
        DemandBlockMaterializer materializer(
            modules,
            blocks,
            &tracker,
            {true, 1u, 4u},
            [](const std::uint32_t target,
               const std::uint32_t physical,
               const std::span<const std::uint8_t>,
               const BlockVariantKey& variant) {
                return MaterializedBlockCandidate{{target,
                                                   physical,
                                                   2u,
                                                   BlockEndKind::Return,
                                                   variant,
                                                   block,
                                                   "role-shadow-materializer",
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
                                                  2u};
            });
        const auto handle =
            materializer.try_materialize(cpu, address, address, BlockVariantKey{}, 0x0001DFF0u);
        require(handle.has_value() && blocks.active(*handle) &&
                    modules.find(promotable.id)->materializable(address, 2u) &&
                    modules.resolve(address, 2u) == modules.find(promotable.id),
                "ProvenData-Owner schattet die Promotion des spaeteren Runtimeowners ab.");
    }
}

void longest_materializable_owner_regression() {
    using namespace katana::runtime;

    constexpr std::uint32_t address = 0x00020000u;
    const std::vector<std::uint8_t> bytes{
        0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x0Bu, 0x00u};
    CpuState cpu;
    cpu.memory.write_bytes(address, bytes, CodeWriteSource::Copy);

    ExecutableModule short_owner;
    short_owner.id = "short-runtime-aot-owner";
    short_owner.source_identity = "free-short-runtime-aot-owner-v1";
    short_owner.guest_start = address;
    short_owner.bytes = bytes;
    short_owner.active_extents = {{0u, 2u}};
    auto long_owner = short_owner;
    long_owner.id = "long-runtime-aot-owner";
    long_owner.source_identity = "free-long-runtime-aot-owner-v1";
    long_owner.active_extents.clear();

    ExecutableModuleCatalog modules;
    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    modules.publish(short_owner);
    modules.publish_loaded_range(long_owner,
                                 blocks,
                                 tracker,
                                 LoadedRangeWriteObservation::ObservedByteIdentical);

    std::vector<std::size_t> snapshot_sizes;
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        &tracker,
        {true, 2u, 16u},
        [&](const std::uint32_t target,
            const std::uint32_t physical,
            const std::span<const std::uint8_t> snapshot,
            const BlockVariantKey& variant) {
            snapshot_sizes.push_back(snapshot.size());
            MaterializedBlockCandidate candidate;
            candidate.block = {
                target,
                physical,
                4u,
                BlockEndKind::Return,
                variant,
                block,
                "longest-owner-runtime-aot",
                false,
                RuntimeAotTemplateContract{{address, address, 8u}, 8u}};
            candidate.decode_candidate_validated = snapshot.size() >= 4u;
            candidate.bounded_analysis_complete = true;
            candidate.ir_verified = true;
            candidate.code_generated = true;
            candidate.guest_cycles = 1u;
            candidate.instructions = 2u;
            candidate.recursive_seeds = 1u;
            candidate.peak_memory_bytes = 8u;
            return candidate;
        });

    const auto handle =
        materializer.try_materialize(cpu, address, address, BlockVariantKey{}, 0x0001FFF0u);
    const auto registered = handle ? blocks.resolve(*handle) : std::nullopt;
    require(handle.has_value() && registered.has_value() &&
                modules.resolve(address, 2u) == modules.find(short_owner.id) &&
                snapshot_sizes == std::vector<std::size_t>{bytes.size()} &&
                registered->get().provenance.find(long_owner.id) != std::string::npos,
            "Kurzer erster Owner schattet den laengeren Runtime-AOT-Snapshot ab.");

    modules.unload(long_owner.id, blocks, tracker);
    const auto retry =
        materializer.try_materialize(cpu, address, address, BlockVariantKey{}, 0x0001FFF0u);
    require(!blocks.active(*handle) && !retry.has_value() &&
                modules.resolve(address, 2u) == modules.find(short_owner.id) &&
                snapshot_sizes == std::vector<std::size_t>{bytes.size(), 2u} &&
                materializer.last_failure() == MaterializationFailure::DecodeRejected &&
                materializer.metrics().materializations == 1u,
            "Langer Runtime-AOT-Block ueberlebt den langen Owner oder wird aus dem "
            "Zweibyte-Restowner neu gebunden.");
}

void dreamcast_main_ram_mirror_invalidation_regression() {
    using namespace katana::runtime;
    DreamcastRuntimeBootImage boot_image;
    const std::vector<std::uint8_t> disc_sector(dreamcast_data_sector_size, 0u);
    boot_image.source = std::make_shared<MemoryDiscSource>(
        std::span<const std::uint8_t>(disc_sector), "synthetic-main-ram-mirror-disc");
    boot_image.system_bootstrap.resize(dreamcast_system_bootstrap_size, 0u);
    boot_image.boot_file = {0x09u, 0x00u};
    boot_image.content_identity = "sha256:synthetic-main-ram-mirror-content";
    boot_image.repeated_bootstrap_reads_match = true;
    boot_image.repeated_reads_match = true;
    CpuState cpu;
    auto runtime = initialize_dreamcast_runtime(cpu, boot_image);

    const auto* bootstrap_module =
        runtime.module_catalog->find(dreamcast_initial_disc_bootstrap_module_id);
    const auto* boot_module =
        runtime.module_catalog->find(dreamcast_initial_boot_executable_module_id);
    require(bootstrap_module != nullptr && boot_module != nullptr &&
                bootstrap_module->guest_start == dreamcast_system_bootstrap_address &&
                bootstrap_module->bytes.size() == dreamcast_system_bootstrap_size &&
                boot_module->guest_start == dreamcast_disc_boot_address &&
                boot_module->bytes == boot_image.boot_file &&
                runtime.module_catalog->resolve(0x0C008000u, dreamcast_system_bootstrap_size) ==
                    bootstrap_module &&
                runtime.module_catalog->resolve(0x0C010000u, boot_image.boot_file.size()) ==
                    boot_module &&
                runtime.module_catalog->validate_bytes_at(cpu.memory,
                                                          dreamcast_system_bootstrap_address,
                                                          0x0C008000u,
                                                          dreamcast_system_bootstrap_size) &&
                runtime.module_catalog->validate_bytes_at(cpu.memory,
                                                          dreamcast_disc_boot_address,
                                                          0x0C010000u,
                                                          boot_image.boot_file.size()),
            "Initialer Discbootstrap und Bootcode sind nicht als lokal gebundene "
            "AOT-Quellmodule publiziert.");

    constexpr std::array<std::uint32_t, 3u> mirror_bases{
        0x0D000000u, 0x0E000000u, 0x0F000000u};
    for (std::size_t index = 0u; index < mirror_bases.size(); ++index) {
        const auto offset = 0x00006000u + static_cast<std::uint32_t>(index * 0x100u);
        const auto canonical = 0x0C000000u + offset;
        RuntimeBlock compiled;
        compiled.virtual_start = 0x8C000000u + offset;
        compiled.physical_origin = canonical;
        compiled.size = 2u;
        compiled.end_kind = BlockEndKind::Return;
        compiled.function = block;
        compiled.provenance = "free-dreamcast-main-ram-mirror-aot-" + std::to_string(index);
        const auto identity = stable_runtime_block_identity(compiled);
        const auto handle = runtime.runtime_blocks->register_static(compiled);
        require(runtime.code_tracker->register_block(
                    {identity,
                     canonical,
                     2u,
                     compiled.provenance,
                     {},
                     ExecutableBlockOrigin::ImageSegment}) ==
                    BlockRegistrationResult::Inserted,
                "Dreamcast-Mirror-AOT-Block wurde nicht registriert.");

        cpu.memory.write_u16(mirror_bases[index] + offset,
                             static_cast<std::uint16_t>(0x1000u + index),
                             CodeWriteSource::Cpu);
        const auto table_active = runtime.runtime_blocks->active(handle);
        const auto tracker_valid = runtime.code_tracker->valid(identity);
        const auto page_generation = runtime.code_tracker->page_generation(canonical);
        require(!table_active && !tracker_valid && page_generation != 0u,
                "Dreamcast-Haupt-RAM-Spiegel invalidiert nicht denselben kanonischen AOT-Block.");
    }

    const auto register_wrap_block = [&](const std::uint32_t virtual_address,
                                         const std::uint32_t physical_address,
                                         const std::string& provenance) {
        RuntimeBlock compiled;
        compiled.virtual_start = virtual_address;
        compiled.physical_origin = physical_address;
        compiled.size = 16u;
        compiled.end_kind = BlockEndKind::Return;
        compiled.function = block;
        compiled.provenance = provenance;
        const auto identity = stable_runtime_block_identity(compiled);
        const auto handle = runtime.runtime_blocks->register_static(compiled);
        require(runtime.code_tracker->register_block({identity,
                                                      physical_address,
                                                      16u,
                                                      provenance,
                                                      {},
                                                      ExecutableBlockOrigin::ImageSegment}) ==
                    BlockRegistrationResult::Inserted,
                "Dreamcast-Mirror-Wrap-AOT-Block wurde nicht registriert.");
        return std::pair{handle, identity};
    };
    const auto tail = register_wrap_block(
        0x8CFFFFF0u, 0x0CFFFFF0u, "free-dreamcast-main-ram-wrap-tail-v1");
    const auto head = register_wrap_block(
        0x8C000000u, 0x0C000000u, "free-dreamcast-main-ram-wrap-head-v1");
    const std::vector<std::uint8_t> wrap_bytes(32u, 0xA5u);
    cpu.memory.write_bytes(0x0DFFFFF0u, wrap_bytes, CodeWriteSource::Cpu);
    require(!runtime.runtime_blocks->active(tail.first) &&
                !runtime.runtime_blocks->active(head.first) &&
                !runtime.code_tracker->valid(tail.second) &&
                !runtime.code_tracker->valid(head.second) &&
                runtime.code_tracker->page_generation(0x0CFFFFF0u) != 0u &&
                runtime.code_tracker->page_generation(0x0C000000u) != 0u,
            "Mirror-Wrap invalidiert nicht Tail und Head desselben Haupt-RAM-Backings.");
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
           const std::uint32_t,
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

void active_extent_overlap_query_regression() {
    using namespace katana::runtime;
    ExecutableModuleCatalog modules;
    require(!modules.may_overlap_active_extent(0x00100000u, 1u) &&
                !modules.may_overlap_active_extent(0xFFFFFFFFu, 0u),
            "Leerer Active-Extent-Index meldet einen linearen oder leeren Bereich.");
    require(modules.may_overlap_active_extent(0x1FFFFFFFu, 2u) &&
                modules.may_overlap_active_extent(0xFFFFFFFFu, 2u),
            "Nichtlineare Alias- oder Adressraumgrenze wird nicht konservativ abgelehnt.");

    ExecutableModule module;
    module.id = "active-extent-overlap-query";
    module.source_identity = "free-active-extent-overlap-query-v1";
    module.guest_start = 0x00101FF0u;
    module.bytes.assign(0x30u, 0x09u);
    modules.publish(module);

    require(modules.may_overlap_active_extent(0x00101FF0u, 1u) &&
                modules.may_overlap_active_extent(0x80102000u, 0x20u) &&
                modules.may_overlap_active_extent(0xA0102030u, 1u) &&
                !modules.may_overlap_active_extent(0x00103000u, 1u),
            "Active-Extent-Query verliert direkte/Alias-Seiten oder ist nicht konservativ.");

    modules.record_runtime_write(0x80101FF0u, 0x10u, CodeWriteSource::Cpu, true);
    require(!modules.may_overlap_active_extent(0x00101FF0u, 1u) &&
                modules.may_overlap_active_extent(0x00102000u, 0x20u),
            "Active-Extent-Query behaelt eine geleerte Seite oder verliert den Restbereich.");

    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    modules.unload(module.id, blocks, tracker);
    require(!modules.may_overlap_active_extent(0xA0102000u, 0x1000u),
            "Active-Extent-Query behaelt nach Unload einen Seitenindexeintrag.");
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
            const std::uint32_t,
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
            const std::uint32_t,
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
    std::optional<std::uint32_t> observed_callback_physical_origin;
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        nullptr,
        {true, 2u, 16u},
        [&](const std::uint32_t target,
           const std::uint32_t physical_origin,
           const std::span<const std::uint8_t> snapshot,
           const BlockVariantKey& requested_variant) {
            observed_callback_physical_origin = physical_origin;
            return MaterializedBlockCandidate{{target,
                                               physical_origin,
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
                resolved_block->get().physical_origin == 0x0C000100u &&
                observed_callback_physical_origin == 0x0C000100u,
            "MMU-Materialisierung liest virtuelle statt physischer Bytes oder verliert die VA.");
}

void deterministic_materialization_without_host_time_regression() {
    using namespace katana::runtime;

    const auto run = [](const BlockMaterializationPolicy policy,
                        const std::uint64_t analysis_time_ms,
                        const std::uint64_t instructions) {
        CpuState cpu;
        const std::vector<std::uint8_t> bytes{0x09u, 0x00u};
        cpu.memory.write_bytes(0x100u, bytes, CodeWriteSource::Copy);

        ExecutableModule module;
        module.id = "deterministic-no-host-time-module";
        module.source_identity = "free-deterministic-materialization-fixture-v1";
        module.guest_start = 0x100u;
        module.bytes = bytes;
        ExecutableModuleCatalog modules;
        modules.publish(module);
        RuntimeBlockTable blocks;
        DemandBlockMaterializer materializer(
            modules,
            blocks,
            nullptr,
            policy,
            [analysis_time_ms, instructions](
                const std::uint32_t target,
                const std::uint32_t physical_origin,
                const std::span<const std::uint8_t>,
                const BlockVariantKey& requested_variant) {
                MaterializedBlockCandidate candidate;
                candidate.block = {target,
                                   physical_origin,
                                   2u,
                                   BlockEndKind::Return,
                                   requested_variant,
                                   block,
                                   "deterministic-no-host-time-decoder",
                                   true};
                candidate.decode_candidate_validated = true;
                candidate.bounded_analysis_complete = true;
                candidate.ir_verified = true;
                candidate.code_generated = true;
                candidate.guest_cycles = 1u;
                candidate.instructions = instructions;
                candidate.recursive_seeds = 1u;
                candidate.analysis_time_ms = analysis_time_ms;
                candidate.peak_memory_bytes = 1u;
                return candidate;
            });
        const auto result =
            materializer.try_materialize(cpu, 0x100u, 0x100u, BlockVariantKey{}, 0x80u);
        return std::pair{result.has_value(), materializer.last_failure()};
    };

    BlockMaterializationPolicy deterministic;
    deterministic.enabled = true;
    deterministic.max_blocks = 1u;
    deterministic.max_bytes = 2u;
    deterministic.max_guest_cycles = 1u;
    deterministic.max_instructions = 1u;
    deterministic.max_recursive_seeds = 1u;
    deterministic.max_analysis_time_ms = 0u;
    deterministic.max_memory_bytes = 2u;
    deterministic.max_materializations_per_run = 1u;
    deterministic.max_repeated_misses_per_target = 1u;
    deterministic.deterministic_no_host_time = true;

    const auto no_host_time = run(deterministic, std::uint64_t{1u} << 60u, 1u);
    require(no_host_time.first && no_host_time.second == MaterializationFailure::None,
            "Deterministische Materialisierung haengt weiterhin von Hostzeit ab.");

    const auto deterministic_instruction_overflow =
        run(deterministic, std::uint64_t{1u} << 60u, 2u);
    require(!deterministic_instruction_overflow.first &&
                deterministic_instruction_overflow.second ==
                    MaterializationFailure::BudgetExhausted,
            "Deterministischer Modus ignoriert das Instruktionsbudget.");

    auto host_timed = deterministic;
    host_timed.deterministic_no_host_time = false;
    host_timed.max_analysis_time_ms = 1u;
    const auto host_time_budget = run(host_timed, 2u, 1u);
    require(!host_time_budget.first &&
                host_time_budget.second == MaterializationFailure::BudgetExhausted,
            "Standardmodus wertet das bestehende Analysezeitbudget nicht mehr aus.");
}

void materializer_alias_boundary_invalidation_regression() {
    using namespace katana::runtime;

    CpuState cpu;
    const std::vector<std::uint8_t> bytes{0x09u, 0x00u, 0x0Bu, 0x00u};
    cpu.memory.write_bytes(0u, bytes, CodeWriteSource::Copy);
    ExecutableModule module;
    module.id = "alias-boundary-materializer";
    module.source_identity = "free-alias-boundary-materializer-v1";
    module.guest_start = 0u;
    module.bytes = bytes;
    ExecutableModuleCatalog modules;
    modules.publish(module);
    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        &tracker,
        {true, 2u, 4u},
        [](const std::uint32_t target,
           const std::uint32_t physical_origin,
           const std::span<const std::uint8_t>,
           const BlockVariantKey& variant) {
            MaterializedBlockCandidate candidate;
            candidate.block = {target,
                               physical_origin,
                               2u,
                               BlockEndKind::Return,
                               variant,
                               block,
                               "alias-boundary-native-block",
                               true};
            candidate.decode_candidate_validated = true;
            candidate.bounded_analysis_complete = true;
            candidate.ir_verified = true;
            candidate.code_generated = true;
            candidate.guest_cycles = 1u;
            candidate.instructions = 1u;
            candidate.recursive_seeds = 1u;
            candidate.peak_memory_bytes = 2u;
            return candidate;
        });
    const auto handle =
        materializer.try_materialize(cpu, 0u, 0u, BlockVariantKey{}, 0x80u);
    require(handle.has_value(), "Aliasgrenzen-Fixture konnte keinen Runtimeblock binden.");
    const auto resolved = blocks.resolve(*handle);
    require(resolved.has_value(), "Aliasgrenzen-Fixture verlor ihren Runtimeblock.");
    const auto identity = stable_runtime_block_identity(resolved->get());
    IndirectDispatchMetrics metrics;
    metrics.record_hit(RuntimeDispatchClass::RuntimeOnly, 0x80u, 0u, true);

    materializer.record_invalidation(0x1FFFFFFEu, 4u, metrics);
    require(!blocks.active(*handle) && !tracker.valid(identity) &&
                materializer.metrics().retained_validation_bytes == 0u &&
                materializer.metrics().reclaimed_validation_bytes == 2u &&
                metrics.runtime_only_sites().at(0x80u).invalidations == 1u,
            "Nichtlineare P0/P1/P2-Aliasgrenze verfehlt den gewrappten "
            "Materialisierungsursprung.");
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
                                            const std::uint32_t,
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

    {
        ExecutableModuleCatalog snapshot_catalog;
        ExecutableModule snapshot_module;
        snapshot_module.id = "snapshot-module";
        snapshot_module.source_identity = "free-snapshot";
        snapshot_module.guest_start = 0x2000u;
        snapshot_module.bytes = bytes;
        snapshot_catalog.publish(snapshot_module);
        snapshot_catalog.record_runtime_write(
            0x4003u, 2u, CodeWriteSource::Cpu, true);
        const auto snapshot = snapshot_catalog.snapshot();
        require(snapshot.modules.size() == 1u &&
                    snapshot.modules.front().id == snapshot_module.id &&
                    snapshot.modules.front().source_identity == snapshot_module.source_identity &&
                    snapshot.modules.front().guest_start == snapshot_module.guest_start &&
                    snapshot.modules.front().bytes == snapshot_module.bytes &&
                    snapshot.modules.front().generation == 1u &&
                    snapshot.modules.front().active &&
                    snapshot.runtime_write_pages.size() == 1u &&
                    snapshot.runtime_write_pages.front().physical_page == 0x4000u &&
                    snapshot.runtime_write_pages.front().written[0u] == 0x18u &&
                    snapshot.active_extent_page_refcounts ==
                        std::vector<ExecutableModulePageRefcountSnapshot>{{0x2000u, 1u}},
                "Modulkatalog-Snapshot verliert Runtimewrite-Bitmap oder Extent-Generation.");
    }

    relocated_module_regression();
    native_aot_template_materialization_regression();
    overlapping_vbr_aot_target_identity_regression();
    module_incarnation_aba_regression();
    multi_extent_module_lifecycle_regression();
    relocation_generation_overflow_regression();
    runtime_write_provenance_regression();
    byte_identical_runtime_write_provenance_regression();
    runtime_write_delay_slot_extension_regression();
    identity_preserving_loaded_range_regression();
    identity_preserving_loaded_range_overlap_regression();
    byte_identical_module_owner_lifecycle_regression();
    runtime_aot_owner_failover_regression();
    relocation_owner_failover_regression();
    multi_owner_selection_regression();
    longest_materializable_owner_regression();
    dreamcast_main_ram_mirror_invalidation_regression();
    partial_module_patch_regression();
    non_overlapping_write_stress_regression();
    active_extent_page_index_lifecycle_regression();
    active_extent_overlap_query_regression();
    rejected_loaded_range_is_atomic_regression();
    rejected_replacement_is_atomic_regression();
    nonlinear_alias_boundary_regression();
    reverse_runtime_write_promotion_regression();
    interpreter_interior_entry_regression();
    mmu_materialization_origin_regression();
    deterministic_materialization_without_host_time_regression();
    materializer_alias_boundary_invalidation_regression();

    std::cout << "KR-4704 executable module and materialization regression passed.\n";
    return EXIT_SUCCESS;
}

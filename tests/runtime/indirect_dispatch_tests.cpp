#include "katana/runtime/indirect_dispatch.hpp"
#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/executable_modules.hpp"
#include "katana/runtime/native_aot_template.hpp"
#include "katana/runtime/native_port_aot_runtime.hpp"
#include "katana/runtime/native_port_content.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace katana::runtime;

static_assert(native_bringup_coverage_maximum_source_transfers == 262'144u);

namespace {
BlockExit block(CpuState&, BlockExecutionContext&) {
    return {};
}
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void static_aot_p2_alias_regression() {
    constexpr std::uint64_t runtime_generation = 7u;
    RuntimeBlockTable blocks;
    blocks.bind_code_tracker(
        nullptr, StaticAotInvalidationContract::Coordinated);
    RuntimeBlock native{0x8C001000u,
                        0x0C001000u,
                        4u,
                        BlockEndKind::Return,
                        {0u, 0u, 0u, 0u, runtime_generation},
                        block,
                        "static-aot-alias",
                        false};
    const std::uint32_t fastpath_descriptor = 0xA07FA57u;
    native.static_variant_policy =
        StaticVariantPolicy::DirectP1P2RuntimeStateAgnostic;
    native.fastpath = {
        RuntimeBlockFastpathKind::MemoryFill,
        &fastpath_descriptor,
    };
    static_cast<void>(blocks.register_static(std::move(native)));
    blocks.seal_static();

    CpuState cpu;
    cpu.write_sr(sr_md_mask);
    cpu.address_space = std::make_shared<RuntimeAddressSpace>();
    const BlockAddress source{0x8C000100u, 0x0C000100u};
    IndirectDispatchRequest request{
        IndirectDispatchKind::TailJump,
        0x8C000102u,
        0xAC001000u,
        0u,
        source};
    request.variant.runtime_generation = runtime_generation;
    const auto first = dispatch_indirect(cpu, blocks, request);
    const auto cached = dispatch_indirect(cpu, blocks, request);
    require(first.alias_lookup && cached.alias_lookup &&
                first.execution.virtual_start == 0x8C001000u &&
                cached.execution.virtual_start == 0x8C001000u &&
                cached.execution.variant.runtime_generation == runtime_generation &&
                first.execution.fastpath.kind == RuntimeBlockFastpathKind::MemoryFill &&
                first.execution.fastpath.descriptor == &fastpath_descriptor &&
                cached.execution.fastpath.kind == RuntimeBlockFastpathKind::MemoryFill &&
                cached.execution.fastpath.descriptor == &fastpath_descriptor &&
                cpu.pc == 0x8C001000u,
            "Static-AOT-P2-Alias oder sein Inline-Cachehit verliert Owner-PC/Fastpathbindung.");
}

void materializer_lifecycle_regression() {
    CpuState cpu;
    const std::vector<std::uint8_t> bytes{0x09u, 0x00u, 0x0Bu, 0x00u};
    cpu.memory.write_bytes(0x1000u, bytes, CodeWriteSource::Copy);
    cpu.memory.write_bytes(0x2000u, bytes, CodeWriteSource::Copy);

    ExecutableModule source;
    source.id = "shared-aot-source";
    source.source_identity = "free-shared-aot-source-v1";
    source.guest_start = 0x1000u;
    source.bytes = bytes;
    ExecutableModule runtime_copy = source;
    runtime_copy.id = "shared-aot-runtime-copy";
    runtime_copy.source_identity = "free-shared-aot-runtime-copy-v1";
    runtime_copy.guest_start = 0x2000u;
    ExecutableModuleCatalog modules;
    modules.publish(source);
    modules.publish(runtime_copy);

    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    cpu.memory.set_guest_write_observer(
        [&](const GuestWriteEvent& event) {
            modules.record_runtime_write(
                event.address, event.size, event.source, event.bytes_changed);
            const auto invalidation = tracker.observe_write(
                event.address, event.size, event.source, event.bytes_changed);
            if (!invalidation.byte_identical)
                static_cast<void>(
                    blocks.erase_overlapping_physical(event.address, event.size));
        },
        GuestWriteObserverContract::StableForPrevalidatedLinearWrites);
    BlockMaterializationPolicy policy;
    policy.enabled = true;
    policy.max_blocks = 8u;
    policy.max_bytes = 16u;
    policy.max_memory_bytes = bytes.size();
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        &tracker,
        policy,
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
                               "shared-native-aot-template",
                               false,
                               RuntimeAotTemplateContract{{0x1000u, 0x2000u, 4u}, 4u}};
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
    IndirectDispatchMetrics metrics;
    IndirectDispatchRequest request;
    request.kind = IndirectDispatchKind::TailJump;
    request.callsite = 0x80u;
    request.target = 0x2000u;
    request.dispatch_class = RuntimeDispatchClass::RuntimeOnly;
    request.metrics = &metrics;
    request.materializer = &materializer;
    DispatchDiagnosticRecorder diagnostics;
    request.diagnostics = &diagnostics;

    const auto first = dispatch_indirect(cpu, blocks, request);
    const auto first_block = blocks.resolve(first.block);
    require(first.materialized && first_block.has_value() &&
                diagnostics.total_occurrences() == 1u &&
                diagnostics.events().size() == 1u,
            "Erste AOT-Bindung wird nicht materialisiert oder detailliert ausgewiesen.");
    const auto first_identity = stable_runtime_block_identity(first_block->get());
    const auto cached = dispatch_indirect(cpu, blocks, request);
    const auto cached_guard =
        materializer.capture_dispatch_generation_guard(cpu, cached.block, true);
    require(!cached.materialized && cached.block == first.block &&
                cached.diagnostic.empty() && cached_guard.has_value() &&
                materializer.dispatch_generation_guard_current(cpu, *cached_guard) &&
                diagnostics.total_occurrences() == 1u,
            "Cache-Hit wird faelschlich materialisiert oder als Detailereignis erfasst.");

    request.target = 0x2002u;
    request.callsite = 0x84u;
    const auto sibling = dispatch_indirect(cpu, blocks, request);
    const auto sibling_block = blocks.resolve(sibling.block);
    require(sibling.materialized && sibling_block.has_value() &&
                diagnostics.total_occurrences() == 2u &&
                materializer.metrics().retained_validation_bytes == bytes.size() &&
                materializer.metrics().peak_retained_validation_bytes == bytes.size(),
            "Weitere AOT-Bindung fehlt im Detail oder dupliziert den Retained-Proof.");
    const auto sibling_identity = stable_runtime_block_identity(sibling_block->get());

    auto replacement = source;
    modules.replace(replacement, blocks, tracker);
    require(!materializer.dispatch_generation_guard_current(cpu, *cached_guard),
            "Modul-/Codegeneration laesst einen alten Dispatch-Cacheguard bestehen.");
    request.target = 0x2000u;
    request.callsite = 0x80u;
    const auto rebound = dispatch_indirect(cpu, blocks, request);
    const auto rebound_block = blocks.resolve(rebound.block);
    require(rebound.materialized && rebound.block != first.block &&
                rebound_block.has_value() && !blocks.active(first.block) &&
                !blocks.active(sibling.block) && !tracker.valid(first_identity) &&
                !tracker.valid(sibling_identity) &&
                tracker.valid(stable_runtime_block_identity(rebound_block->get())) &&
                diagnostics.total_occurrences() == 3u &&
                materializer.metrics().materializations == 3u &&
                materializer.metrics().retained_validation_bytes == bytes.size() &&
                materializer.metrics().peak_retained_validation_bytes == bytes.size() &&
                materializer.metrics().reclaimed_validation_bytes == bytes.size() &&
                metrics.misses() == 0u &&
                metrics.runtime_only_sites().at(0x80u).invalidations == 1u &&
                metrics.runtime_only_sites().at(0x84u).invalidations == 1u,
            "Quellmodulwechsel invalidiert oder bindet abhaengige AOT-Bloecke nicht atomar neu.");

    static_cast<void>(blocks.erase_overlapping_physical(0x2000u, bytes.size()));
    materializer.reconcile_inactive_origins(&metrics);
    require(materializer.metrics().retained_validation_bytes == 0u &&
                materializer.metrics().reclaimed_validation_bytes == bytes.size() * 2u &&
                metrics.runtime_only_sites().at(0x80u).invalidations == 2u,
            "Produktive Blockinvalidierung raeumt Origin und Retained-Proof nicht auf.");

    IndirectDispatchMetrics guarded_metrics;
    request.callsite = 0x88u;
    request.target = 0x2000u;
    request.dispatch_class = RuntimeDispatchClass::GuardedFallback;
    request.metrics = &guarded_metrics;
    const auto guarded = dispatch_indirect(cpu, blocks, request);
    require(guarded.materialized && guarded_metrics.hits() == 1u &&
                guarded_metrics.runtime_only_site_count() == 0u,
            "GuardedFallback-AOT-Bindung wird als Runtime-only-Site klassifiziert.");
    static_cast<void>(blocks.erase_overlapping_physical(0x2000u, bytes.size()));
    materializer.reconcile_inactive_origins(&guarded_metrics);
    require(guarded_metrics.runtime_only_site_count() == 0u &&
                materializer.metrics().retained_validation_bytes == 0u,
            "GuardedFallback-Invalidierung erzeugt beim Reconcile ein Runtime-only-Siteprofil.");
}

void runtime_aot_alias_lifetime_regression() {
    constexpr std::uint32_t runtime_start = 0x0C001000u;
    constexpr std::uint32_t source_start = 0x0C002000u;
    const std::vector<std::uint8_t> bytes{0x09u, 0x00u, 0x0Bu, 0x00u};

    CpuState cpu;
    cpu.write_sr(sr_md_mask);
    cpu.memory.map_region(
        "runtime-aot-alias-ram", runtime_start, std::make_shared<LinearMemoryDevice>(0x2000u));
    cpu.memory.write_bytes(runtime_start, bytes, CodeWriteSource::Copy);
    cpu.memory.write_bytes(source_start, bytes, CodeWriteSource::Copy);

    ExecutableModule runtime_copy;
    runtime_copy.id = "runtime-aot-alias-copy";
    runtime_copy.source_identity = "free-runtime-aot-alias-copy-v1";
    runtime_copy.guest_start = runtime_start;
    runtime_copy.bytes = bytes;
    ExecutableModule source = runtime_copy;
    source.id = "runtime-aot-alias-source";
    source.source_identity = "free-runtime-aot-alias-source-v1";
    source.guest_start = source_start;
    ExecutableModuleCatalog modules;
    modules.publish(runtime_copy);
    modules.publish(source);

    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    BlockMaterializationPolicy policy;
    policy.enabled = true;
    policy.max_blocks = 4u;
    policy.max_bytes = 16u;
    policy.max_memory_bytes = bytes.size();
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        &tracker,
        policy,
        [runtime_start, source_start](const std::uint32_t target,
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
                               "runtime-aot-alias-template",
                               false,
                               RuntimeAotTemplateContract{
                                   {source_start, runtime_start, 4u}, 4u}};
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
    IndirectDispatchMetrics metrics;
    IndirectDispatchRequest request;
    request.kind = IndirectDispatchKind::TailJump;
    request.callsite = 0x90u;
    request.target = runtime_start;
    request.dispatch_class = RuntimeDispatchClass::RuntimeOnly;
    request.metrics = &metrics;
    request.materializer = &materializer;

    const auto first = dispatch_indirect(cpu, blocks, request);
    const auto resolved = blocks.resolve(first.block);
    require(first.materialized && resolved.has_value(),
            "Runtime-AOT-Aliasfixture wurde nicht initial materialisiert.");
    const auto identity = stable_runtime_block_identity(resolved->get());
    constexpr std::array aliases{
        0x8C001000u, 0xAC001000u, 0x0C001000u, 0x8C001000u, 0xAC001000u};
    for (const auto alias : aliases) {
        request.target = alias;
        const auto dispatched = dispatch_indirect(cpu, blocks, request);
        require(dispatched.block == first.block && !dispatched.materialized &&
                    dispatched.alias_lookup == (alias != runtime_start) &&
                    cpu.pc == runtime_start,
                "Kanonischer Runtime-Alias wurde rematerialisiert oder verlor seinen "
                "normalisierten PC.");
    }
    require(blocks.active(first.block) && tracker.valid(identity) &&
                materializer.metrics().materializations == 1u &&
                materializer.metrics().retained_validation_bytes == bytes.size() &&
                materializer.metrics().peak_retained_validation_bytes == bytes.size() &&
                materializer.metrics().reclaimed_validation_bytes == 0u &&
                metrics.runtime_only_sites().at(request.callsite).invalidations == 0u,
            "Runtime-Aliaswechsel vervielfacht Block/Validation-Proof oder invalidiert Herkunft.");

    cpu.write_sr(sr_md_mask);
    cpu.address_space = std::make_shared<RuntimeAddressSpace>();
    cpu.address_space->set_mode(AddressTranslationMode::Mmu);
    cpu.address_space->write_mmucr(1u);
    cpu.address_space->ldtlb(
        {runtime_start, 0x0D001000u, 4096u, 0u, 0u, true, true, true, true, true, true, false});
    const auto remapped_entry =
        cpu.address_space
            ->translate(runtime_start, TranslationAccess::Instruction, cpu.privileged_mode())
            .physical_address;
    require(remapped_entry == 0x0D001000u &&
                !materializer.validate_for_dispatch(
                    cpu, first.block, runtime_start, remapped_entry) &&
                materializer.last_failure() == MaterializationFailure::StaleHandle &&
                blocks.active(first.block),
            "Gleiche kanonische VA validiert unter MMU-Remap einen falschen physischen "
            "Runtimeblock.");
}

void missing_aot_dispatch_regression() {
    constexpr std::uint32_t source_address = 0x88001000u;
    constexpr std::uint32_t runtime_address = 0x00003000u;
    const std::vector<std::uint8_t> bytes{0x0Bu, 0x00u, 0x09u, 0x00u};
    CpuState cpu;
    cpu.memory.write_bytes(runtime_address, bytes, CodeWriteSource::Copy);

    ExecutableModule loaded;
    loaded.id = "loaded-disc-file";
    loaded.source_identity = "sha256:loaded-fixture";
    loaded.content_identity = "content-fixture";
    loaded.byte_identity = "sha256:loaded-fixture";
    loaded.guest_start = runtime_address;
    loaded.bytes = bytes;
    ExecutableModuleCatalog modules;
    modules.publish(loaded);

    RuntimeBlockTable blocks;
    static_cast<void>(blocks.register_static({source_address,
                                              canonical_physical_address(source_address),
                                              2u,
                                              BlockEndKind::Return,
                                              {},
                                              block,
                                              "latent-aot-source"}));
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    const std::array templates{NativeAotTemplate{
        "latent-aot-source",
        loaded.byte_identity,
        source_address,
        static_cast<std::uint32_t>(bytes.size()),
        0,
        {},
        NativeAotTemplateDestination::LoadedModule,
        loaded.content_identity,
        loaded.byte_identity}};
    NativeAotTemplateBinder binder(cpu, modules, blocks, templates);
    BlockMaterializationPolicy policy;
    policy.enabled = true;
    policy.max_blocks = 4u;
    policy.max_bytes = 16u;
    policy.max_memory_bytes = 16u;
    policy.max_materializations_per_run = 4u;
    policy.max_repeated_misses_per_target = 2u;
    DemandBlockMaterializer materializer(
        modules,
        blocks,
        &tracker,
        policy,
        [&binder](const std::uint32_t target,
                  const std::uint32_t physical,
                  const std::span<const std::uint8_t> snapshot,
                  const BlockVariantKey& variant) {
            return binder.bind(target, physical, snapshot, variant).candidate;
        });
    IndirectDispatchMetrics metrics;
    IndirectDispatchRequest request;
    request.kind = IndirectDispatchKind::TailJump;
    request.callsite = 0x80u;
    request.target = runtime_address + 2u;
    request.resolution_origin = DispatchResolutionOrigin::RuntimeOnly;
    request.dispatch_class = RuntimeDispatchClass::RuntimeOnly;
    request.metrics = &metrics;
    request.materializer = &materializer;
    bool rejected = false;
    try {
        static_cast<void>(dispatch_indirect(cpu, blocks, request));
    } catch (const IndirectDispatchError&) {
        rejected = true;
    }
    require(rejected && materializer.last_failure() == MaterializationFailure::MissingAot &&
                metrics.first_error().has_value() &&
                metrics.first_error()->error == DispatchDiagnosticError::MissingAot,
            "Missing-AOT ging zwischen Binder, Materializer und Dispatchdiagnose verloren.");
}

void materialization_identity_diagnostic_regression() {
    const auto expect_rejection =
        [](const MaterializationFailure failure,
           const DispatchDiagnosticError expected_error,
           const char* expected_name) {
            constexpr std::uint32_t runtime_address = 0x00003000u;
            const std::vector<std::uint8_t> bytes{0x0Bu, 0x00u, 0x09u, 0x00u};
            CpuState cpu;
            cpu.memory.write_bytes(runtime_address, bytes, CodeWriteSource::Copy);

            ExecutableModule loaded;
            loaded.id = "diagnostic-runtime-module";
            loaded.source_identity = "free-diagnostic-runtime-module-v1";
            loaded.guest_start = runtime_address;
            loaded.bytes = bytes;
            ExecutableModuleCatalog modules;
            modules.publish(loaded);

            RuntimeBlockTable blocks;
            ExecutableCodeTracker tracker;
            blocks.bind_code_tracker(&tracker);
            BlockMaterializationPolicy policy;
            policy.enabled = true;
            policy.max_blocks = 1u;
            policy.max_bytes = static_cast<std::uint64_t>(bytes.size());
            policy.max_memory_bytes = bytes.size();
            policy.max_materializations_per_run = 1u;
            policy.max_repeated_misses_per_target = 1u;
            DemandBlockMaterializer materializer(
                modules,
                blocks,
                &tracker,
                policy,
                [failure](const std::uint32_t,
                          const std::uint32_t,
                          const std::span<const std::uint8_t>,
                          const BlockVariantKey&) {
                    MaterializedBlockCandidate candidate;
                    candidate.rejection_failure = failure;
                    return candidate;
                });
            IndirectDispatchMetrics metrics;
            IndirectDispatchRequest request;
            request.kind = IndirectDispatchKind::TailJump;
            request.callsite = 0x90u;
            request.target = runtime_address;
            request.resolution_origin = DispatchResolutionOrigin::RuntimeOnly;
            request.dispatch_class = RuntimeDispatchClass::RuntimeOnly;
            request.metrics = &metrics;
            request.materializer = &materializer;

            std::string terminal_message;
            std::string terminal_metrics;
            DispatchDiagnosticError caught_error = DispatchDiagnosticError::None;
            try {
                static_cast<void>(dispatch_indirect(cpu, blocks, request));
            } catch (const IndirectDispatchError& error) {
                caught_error = error.error();
                terminal_message = error.what();
                terminal_metrics = error.metrics_json();
            }
            require(materializer.last_failure() == failure &&
                        metrics.first_error().has_value() &&
                        metrics.first_error()->error == expected_error &&
                        caught_error == expected_error &&
                        dispatch_diagnostic_error_name(expected_error) ==
                            std::string(expected_name) &&
                        terminal_message.find(expected_name) != std::string::npos &&
                        terminal_metrics.find(expected_name) != std::string::npos,
                    "Materializer-Identitaetsfehler verlor seinen eigenen terminalen "
                    "Dispatchdiagnosevertrag.");
        };

    expect_rejection(MaterializationFailure::ByteIdentityMismatch,
                     DispatchDiagnosticError::ByteIdentityMismatch,
                     "byte-identity-mismatch");
    expect_rejection(MaterializationFailure::AotTemplateMismatch,
                     DispatchDiagnosticError::AotTemplateMismatch,
                     "aot-template-mismatch");
}

void runtime_only_hit_hotloop_regression() {
    constexpr std::uint32_t callsite = 0x8C003002u;
    constexpr std::uint32_t target = 0x8C004000u;
    constexpr std::uint64_t hotloop_hits = 1'000'000u;

    RuntimeBlockTable blocks;
    const BlockVariantKey variant{};
    const auto registered = blocks.register_static({target,
                                                    canonical_physical_address(target),
                                                    2u,
                                                    BlockEndKind::Return,
                                                    variant,
                                                    block,
                                                    "runtime-only-hotloop",
                                                    false});
    require(static_cast<bool>(registered),
            "Runtime-only-Hotloopblock wurde nicht registriert.");
    ExecutableModuleCatalog modules;
    ExecutableCodeTracker tracker;
    blocks.bind_code_tracker(&tracker);
    BlockMaterializationPolicy disabled_materialization;
    DemandBlockMaterializer materializer(
        modules, blocks, &tracker, disabled_materialization, {});

    DispatchDiagnosticRecorder diagnostics;
    for (std::size_t index = 0u; index < diagnostics.capacity(); ++index) {
        const auto offset = static_cast<std::uint32_t>(index * 2u);
        DispatchDiagnosticEvent event;
        event.callsite = 0x8C100000u + offset;
        event.source_virtual = 0x8C110000u + offset;
        event.source_physical = canonical_physical_address(event.source_virtual);
        event.virtual_target = 0x8C120000u + offset;
        event.canonical_target = canonical_physical_address(*event.virtual_target);
        event.block_end = BlockEndKind::DynamicBranch;
        event.origin = DispatchResolutionOrigin::RuntimeOnly;
        event.exit_pc = *event.virtual_target;
        diagnostics.record(event);
    }
    const auto detail_occurrences_before = diagnostics.total_occurrences();
    const auto dropped_details_before = diagnostics.dropped_unique_events();

    CpuState cpu;
    cpu.write_sr(sr_md_mask);
    cpu.memory.set_guest_write_observer(
        [&](const GuestWriteEvent& event) {
            const auto invalidation = tracker.observe_write(
                event.address, event.size, event.source, event.bytes_changed);
            if (!invalidation.byte_identical)
                static_cast<void>(
                    blocks.erase_overlapping_physical(event.address, event.size));
        },
        GuestWriteObserverContract::StableForPrevalidatedLinearWrites);
    IndirectDispatchMetrics metrics;
    IndirectDispatchRequest request;
    request.kind = IndirectDispatchKind::TailJump;
    request.callsite = callsite;
    request.target = target;
    request.source = {callsite - 2u, canonical_physical_address(callsite - 2u)};
    request.variant = variant;
    request.resolution_origin = DispatchResolutionOrigin::RuntimeOnly;
    request.diagnostics = &diagnostics;
    request.dispatch_class = RuntimeDispatchClass::RuntimeOnly;
    request.metrics = &metrics;
    request.materializer = &materializer;

    blocks.reset_lookup_counters();
    for (std::uint64_t hit = 0u; hit < hotloop_hits; ++hit)
        static_cast<void>(dispatch_indirect(cpu, blocks, request));

    bool retained_details_unchanged =
        diagnostics.events().size() == diagnostics.capacity();
    for (const auto& event : diagnostics.events())
        retained_details_unchanged &= event.occurrences == 1u;
    const auto site = metrics.runtime_only_sites().find(callsite);
    require(metrics.hits() == hotloop_hits &&
                metrics.runtime_only_hits() == hotloop_hits && metrics.misses() == 0u &&
                site != metrics.runtime_only_sites().end() &&
                site->second.calls == hotloop_hits && site->second.hits == hotloop_hits &&
                site->second.misses == 0u && site->second.materializations == 0u &&
                site->second.invalidations == 0u && site->second.targets.size() == 1u &&
                site->second.targets.front() == target &&
                blocks.lookup_counters().direct_probes == 2u &&
                diagnostics.total_occurrences() == detail_occurrences_before &&
                diagnostics.dropped_unique_events() == dropped_details_before &&
                retained_details_unchanged,
            "Eine Million reine Runtime-only-Hits aktualisieren nicht nur Metriken oder "
            "betreten weiterhin den linearen Detail-Key-Vergleich.");
    IndirectDispatchMetrics polymorphic_metrics;
    polymorphic_metrics.record_hit(
        RuntimeDispatchClass::RuntimeOnly, callsite, target, false);
    polymorphic_metrics.record_hit(
        RuntimeDispatchClass::RuntimeOnly, callsite, target + 2u, false);
    const auto& polymorphic_site = polymorphic_metrics.runtime_only_sites().at(callsite);
    require(polymorphic_site.targets.size() == 1u &&
                polymorphic_site.targets.front() == target &&
                polymorphic_site.targets_truncated &&
                polymorphic_site.stability() == RuntimeTargetStability::Dynamic,
            "Runtime-only-Profil behaelt weiterhin eine detaillierte Zielhistorie.");

    DispatchDiagnosticRecorder alias_diagnostics;
    IndirectDispatchMetrics alias_metrics;
    request.target = 0xAC004000u;
    request.diagnostics = &alias_diagnostics;
    request.metrics = &alias_metrics;
    const auto alias = dispatch_indirect(cpu, blocks, request);
    require(alias.alias_lookup && alias.resulting_pc == target && cpu.pc == target &&
                alias_metrics.hits() == 1u && alias_metrics.misses() == 0u &&
                alias_diagnostics.total_occurrences() == 1u &&
                alias_diagnostics.events().size() == 1u &&
                alias_diagnostics.events().front().alias_origin ==
                    DispatchAliasOrigin::CanonicalPhysical &&
                alias_diagnostics.events().front().virtual_target ==
                    std::optional<std::uint32_t>{0xAC004000u},
            "Erster Runtime-only-P0/P1/P2-Aliastreffer verliert seine "
            "PC-Normalisierung oder Aliasdiagnose.");

    DispatchDiagnosticRecorder call_diagnostics;
    IndirectDispatchMetrics call_metrics;
    request.kind = IndirectDispatchKind::Call;
    request.target = target;
    request.return_address = 0x8C003006u;
    request.diagnostics = &call_diagnostics;
    request.metrics = &call_metrics;
    static_cast<void>(dispatch_indirect(cpu, blocks, request));
    require(cpu.pr == request.return_address && call_metrics.hits() == 1u &&
                call_diagnostics.total_occurrences() == 1u &&
                call_diagnostics.events().size() == 1u &&
                call_diagnostics.events().front().block_end == BlockEndKind::Call &&
                call_diagnostics.events().front().pr == request.return_address,
            "Exakter Runtime-only-Call verliert PR- oder Terminatorevidenz.");

    DispatchDiagnosticRecorder return_diagnostics;
    IndirectDispatchMetrics return_metrics;
    request.kind = IndirectDispatchKind::Return;
    request.target = 0u;
    cpu.pr = target;
    request.diagnostics = &return_diagnostics;
    request.metrics = &return_metrics;
    static_cast<void>(dispatch_indirect(cpu, blocks, request));
    require(cpu.pc == target && cpu.pr == target && return_metrics.hits() == 1u &&
                return_diagnostics.total_occurrences() == 1u &&
                return_diagnostics.events().size() == 1u &&
                return_diagnostics.events().front().block_end == BlockEndKind::Return &&
                return_diagnostics.events().front().pr == target,
            "Exakter Runtime-only-Return verliert PR- oder Terminatorevidenz.");

    DispatchDiagnosticRecorder miss_diagnostics;
    IndirectDispatchMetrics miss_metrics;
    request.kind = IndirectDispatchKind::TailJump;
    request.target = 0x8C005000u;
    request.diagnostics = &miss_diagnostics;
    request.metrics = &miss_metrics;
    bool rejected = false;
    try {
        static_cast<void>(dispatch_indirect(cpu, blocks, request));
    } catch (const IndirectDispatchError&) {
        rejected = true;
    }
    require(rejected && miss_metrics.misses() == 1u &&
                miss_diagnostics.total_occurrences() == 1u &&
                miss_diagnostics.events().size() == 1u &&
                miss_diagnostics.events().front().error ==
                    DispatchDiagnosticError::UnknownTarget,
            "Runtime-only-Miss verliert sein Detailereignis neben dem Hit-Hotpath.");

    const auto registered_block = blocks.resolve(registered);
    require(registered_block.has_value(), "Hotloopblock ging vor Codeguard-Test verloren.");
    const auto identity = stable_runtime_block_identity(registered_block->get());
    static_cast<void>(tracker.register_block(
        {identity,
         canonical_physical_address(target),
         2u,
         "runtime-only-hotloop",
         {},
         ExecutableBlockOrigin::ImageSegment,
         {}}));
    static_cast<void>(
        tracker.observe_write(target, 2u, CodeWriteSource::Cpu, true));
    request.kind = IndirectDispatchKind::TailJump;
    request.target = target;
    request.diagnostics = nullptr;
    request.metrics = &metrics;
    bool stale_cache_rejected = false;
    try {
        static_cast<void>(dispatch_indirect(cpu, blocks, request));
    } catch (const IndirectDispatchError&) {
        stale_cache_rejected = true;
    }
    require(stale_cache_rejected,
            "Block-/Codegeneration liess einen invalidierten Inline-Cachehit zu.");
}

void native_bringup_coverage_regression() {
    static_assert(!NativeBringupCoverageDispatchContext::release_eligible);
    constexpr std::string_view module_identity =
        "sha256:7af85194466a76bee16168ca8152d4560bd9bec17ade2525f267ed49a54f36a9";
    constexpr std::string_view static_source_identity =
        "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    constexpr std::string_view authority_identity =
        "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    constexpr std::string_view analysis_identity =
        "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    constexpr std::string_view pack_identity =
        "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    constexpr std::string_view runtime_image_identity = module_identity;
    constexpr std::string_view runtime_image_id = "fixture-runtime-image";
    constexpr std::uint32_t source_start = 0x80810000u;
    constexpr std::uint32_t target_source_start = 0x80820000u;
    constexpr std::uint32_t source_runtime_start = 0x8C910000u;
    constexpr std::uint32_t target_runtime_start = 0x8C920000u;
    constexpr std::uint32_t unrelated_fixed_runtime_start = 0x8C930000u;
    constexpr std::uint32_t runtime_image_source_start = 0x80830000u;
    constexpr std::uint32_t runtime_image_runtime_start = 0x8C940000u;
    constexpr std::uint64_t runtime_generation = 37u;
    constexpr std::uint64_t aot_pack_generation = 41u;
    const std::array<std::uint8_t, 4u> bytes{
        0x09u, 0x00u, 0x0Bu, 0x00u};

    const std::array source_source_bindings{
        NativePortLoadedAotSourceBindingView{
            NativePortLoadedAotSourceTransform::Identity,
            module_identity,
            0u,
            static_cast<std::uint32_t>(bytes.size()),
            source_runtime_start}};
    const std::array target_source_bindings{
        NativePortLoadedAotSourceBindingView{
            NativePortLoadedAotSourceTransform::Identity,
            module_identity,
            4u,
            static_cast<std::uint32_t>(bytes.size()),
            0u}};
    const std::array blocks{
        NativePortLoadedAotBlockIdentityView{
            0u, static_cast<std::uint32_t>(bytes.size()), module_identity}};
    const std::array modules{
        NativePortLoadedAotModuleView{
            source_start,
            static_cast<std::uint32_t>(bytes.size()),
            module_identity,
            source_source_bindings,
            blocks},
        NativePortLoadedAotModuleView{
            target_source_start,
            static_cast<std::uint32_t>(bytes.size()),
            module_identity,
            target_source_bindings,
            blocks}};
    const auto module_universe_identity =
        native_port_loaded_aot_module_universe_identity(modules);
    const std::array runtime_image_blocks{
        NativePortLoadedAotBlockIdentityView{
            0u, static_cast<std::uint32_t>(bytes.size()),
            runtime_image_identity}};
    const std::array runtime_images{
        NativePortRuntimeImageView{
            runtime_image_id,
            runtime_image_source_start,
            runtime_image_runtime_start,
            static_cast<std::uint32_t>(bytes.size()),
            runtime_image_identity,
            runtime_image_blocks}};
    const std::array immutable_ranges{
        NativePortImmutableRange{
            0x0C000000u,
            2u,
            native_port_immutable_range_mask(
                NativePortImmutableRangeKind::Executable)}};
    NativePortMemory memory;
    auto& cpu = memory.cpu();
    cpu.memory.write_bytes(
        canonical_physical_address(source_runtime_start),
        bytes,
        CodeWriteSource::Copy);
    cpu.memory.write_bytes(
        canonical_physical_address(target_runtime_start),
        bytes,
        CodeWriteSource::Copy);
    cpu.memory.write_bytes(
        canonical_physical_address(runtime_image_runtime_start),
        bytes,
        CodeWriteSource::Copy);
    NativePortImmutableWriteGuard immutable_guard(immutable_ranges);
    NativePortExecutableLifecycleLedger lifecycle_ledger(3u);
    NativePortRuntimeImageBindings runtime_image_bindings(
        cpu, runtime_images, immutable_guard, lifecycle_ledger);
    NativePortLoadedAotBinder binder(
        cpu, modules, immutable_guard, lifecycle_ledger,
        module_universe_identity, pack_identity);
    runtime_image_bindings.activate(runtime_image_id);
    const auto source_lifecycle = binder.stage_runtime_module(
        {module_identity,
         source_start,
         source_runtime_start,
         static_cast<std::uint32_t>(bytes.size())});
    require(binder.bind_entry(source_runtime_start),
            "Coverage-Fixture konnte den geladenen Source-Owner nicht aktivieren.");
    const auto target_lifecycle = binder.stage_runtime_module(
        {module_identity,
         target_source_start,
         target_runtime_start,
         static_cast<std::uint32_t>(bytes.size())});

    const NativeBringupDispatchStaticAotBinding source_binding{
        {source_start, canonical_physical_address(source_start)},
        static_cast<std::uint32_t>(bytes.size()),
        BlockEndKind::Call,
        module_identity};
    const NativeBringupDispatchStaticAotBinding target_binding{
        {target_source_start, canonical_physical_address(target_source_start)},
        static_cast<std::uint32_t>(bytes.size()),
        BlockEndKind::Return,
        module_identity};
    const NativeBringupDispatchStaticAotBinding runtime_image_binding{
        {runtime_image_source_start,
         canonical_physical_address(runtime_image_source_start)},
        static_cast<std::uint32_t>(bytes.size()),
        BlockEndKind::Call,
        runtime_image_identity};
    const NativeBringupDispatchStaticAotBinding target_runtime_static_binding{
        {target_runtime_start,
         canonical_physical_address(target_runtime_start)},
        static_cast<std::uint32_t>(bytes.size()),
        BlockEndKind::Return,
        static_source_identity};
    RuntimeBlockTable table;
    table.bind_code_tracker(
        nullptr, StaticAotInvalidationContract::Coordinated);
    const auto make_static_block = [=](
                                       const NativeBringupDispatchStaticAotBinding& binding) {
        RuntimeBlock native{binding.block.virtual_address,
                            binding.block.physical_address,
                            binding.size,
                            binding.end_kind,
                            {1u, 2u, 3u, 4u, runtime_generation},
                            block,
                            std::string(binding.block_code_identity),
                            false};
        native.static_variant_policy =
            StaticVariantPolicy::DirectP1P2RuntimeStateAgnostic;
        return native;
    };
    const auto source_handle =
        table.register_static(make_static_block(source_binding));
    const auto target_handle =
        table.register_static(make_static_block(target_binding));
    const auto runtime_image_handle =
        table.register_static(make_static_block(runtime_image_binding));
    const auto target_runtime_static_handle = table.register_static(
        make_static_block(target_runtime_static_binding));
    table.seal_static();
    const BlockVariantKey coverage_variant{0u, 0u, 0u, 0u,
                                           runtime_generation};
    const auto dynamic_shadow = table.register_runtime(
        {target_source_start,
         canonical_physical_address(target_source_start),
         static_cast<std::uint32_t>(bytes.size()),
         BlockEndKind::Return,
         coverage_variant,
         block,
         "coverage-dynamic-shadow",
         false});
    require(dynamic_shadow &&
                !table.lookup_static_aot(
                    canonical_physical_address(target_source_start),
                    target_source_start, coverage_variant) &&
                table.lookup_sealed_static_aot(
                    canonical_physical_address(target_source_start),
                    target_source_start, coverage_variant),
            "Coverage-Fixture konnte die normale Dynamic-Praezedenz vor "
            "dem versiegelten Static-AOT-Owner nicht herstellen.");

    const std::array source_transfers{
        NativeBringupCoverageSourceTransfer{
            NativeBringupTransferKind::CallRegister,
            source_start,
            source_start + 4u,
            NativeBringupCoverageSourceKind::LoadedAot,
            source_binding,
            module_identity,
            {},
            source_runtime_start,
            static_cast<std::uint32_t>(bytes.size()),
            0u}};
    const std::array coverage_entries{
        NativeBringupCoverageEntry{
            module_identity,
            static_cast<std::uint32_t>(bytes.size()),
            target_source_start,
            unrelated_fixed_runtime_start,
            0u,
            target_binding}};
    using CoverageOwner = NativeBringupCoverageOwnerKind;
    using CoverageCapability = NativeBringupCoverageTargetCapability;
    const std::array target_authorities{
        NativeBringupCoverageTargetAuthority{
            CoverageOwner::PrimaryStatic,
            CoverageCapability::Callable |
                CoverageCapability::TailJumpEntry,
            pack_identity,
            {},
            static_cast<std::uint32_t>(bytes.size()),
            target_source_start,
            target_source_start,
            0u,
            target_binding},
        NativeBringupCoverageTargetAuthority{
            CoverageOwner::LoadedAot,
            CoverageCapability::Callable |
                CoverageCapability::TailJumpEntry,
            module_identity,
            {},
            static_cast<std::uint32_t>(bytes.size()),
            target_source_start,
            target_runtime_start,
            0u,
            target_binding},
        NativeBringupCoverageTargetAuthority{
            CoverageOwner::RuntimeImage,
            CoverageCapability::Callable |
                CoverageCapability::TailJumpEntry,
            runtime_image_identity,
            runtime_image_id,
            static_cast<std::uint32_t>(bytes.size()),
            runtime_image_source_start,
            runtime_image_runtime_start,
            0u,
            runtime_image_binding}};
    const NativeBringupCoverageDispatchPack coverage_pack{
        {native_bringup_coverage_contract_version,
         authority_identity,
         "fixture-project",
         "fixture-v1",
         analysis_identity,
         pack_identity,
         module_universe_identity,
        aot_pack_generation},
        source_transfers,
        coverage_entries,
        target_authorities};
    NativeBringupCoverageObservations coverage_observations;
    const auto context = make_native_bringup_coverage_dispatch_context(
        table, runtime_image_bindings, binder, coverage_pack,
        runtime_generation,
        coverage_observations);
    NativeBringupCoveragePreflightRequest request{
        NativeBringupTransferKind::CallRegister,
        source_start,
        target_runtime_start,
        source_start + 4u,
        source_binding.block,
        {0u, 0u, 0u, 0u, runtime_generation},
        NativeBringupCoveragePreflightRequest::TargetHook::
            CallableFunctionEntry};

    auto wrong_runtime_sources = source_transfers;
    wrong_runtime_sources.front().source_runtime_start += 0x1000u;
    const NativeBringupCoverageDispatchPack wrong_runtime_pack{
        coverage_pack.identity, wrong_runtime_sources, coverage_entries,
        target_authorities};
    const auto wrong_runtime_context =
        make_native_bringup_coverage_dispatch_context(
            table, runtime_image_bindings, binder, wrong_runtime_pack,
            runtime_generation,
            coverage_observations);
    auto wrong_runtime_request = request;
    bool wrong_runtime_rejected = false;
    try {
        static_cast<void>(preflight_native_bringup_coverage_dispatch(
            table, runtime_image_bindings, binder, wrong_runtime_context,
            wrong_runtime_request));
    } catch (const NativeBringupDispatchError& error) {
        wrong_runtime_rejected =
            error.miss() ==
            NativeBringupDispatchMiss::LoadedModuleIdentityMismatch;
    }
    require(wrong_runtime_rejected &&
                !binder.active_entry_for_address(target_runtime_start)
                     .has_value() &&
                coverage_observations.total_occurrences() == 0u,
            "Coverage-Preflight akzeptierte eine falsche Source-Runtimebasis "
            "oder aktivierte vorher das Ziel.");

    const auto native_hook_admitted =
        preflight_native_bringup_coverage_dispatch(
            table, runtime_image_bindings, binder, context, request);
    const auto still_staged =
        binder.preflight_entry_for_address(target_runtime_start);
    require(!native_hook_admitted.block &&
                native_hook_admitted.execution.function == nullptr &&
                native_hook_admitted.target == target_runtime_start &&
                native_hook_admitted.physical_target ==
                    canonical_physical_address(target_runtime_start) &&
                native_hook_admitted.lifecycle_generation == 0u &&
                native_hook_admitted.owner_kind ==
                    CoverageOwner::NativeFunctionEntry &&
                native_hook_admitted.capabilities ==
                    CoverageCapability::Callable &&
                still_staged.has_value() && !still_staged->active &&
                still_staged->lifecycle_generation == target_lifecycle &&
                coverage_observations.total_occurrences() == 0u,
            "Coverage-Preflight band einen Native-FunctionEntry als AOT-Ziel "
            "oder erfand dafuer eine Coverage-Promotion.");

    auto instruction_hook_request = request;
    instruction_hook_request.target_hook =
        NativeBringupCoveragePreflightRequest::TargetHook::
            ConflictingInstruction;
    bool hook_conflict_rejected = false;
    try {
        static_cast<void>(preflight_native_bringup_coverage_dispatch(
            table, runtime_image_bindings, binder, context,
            instruction_hook_request));
    } catch (const NativeBringupDispatchError& error) {
        hook_conflict_rejected =
            error.miss() == NativeBringupDispatchMiss::HookReplacementConflict;
    }
    require(hook_conflict_rejected && still_staged.has_value() &&
                !still_staged->active &&
                still_staged->lifecycle_generation == target_lifecycle &&
                coverage_observations.total_occurrences() == 0u,
            "Coverage-Hookkonflikt aktivierte das Ziel trotz fail-closed Preflight.");

    auto internal_only_authorities = target_authorities;
    internal_only_authorities[1u].capabilities =
        CoverageCapability::TailJumpEntry |
        CoverageCapability::InternalBlock;
    const NativeBringupCoverageDispatchPack internal_only_pack{
        coverage_pack.identity, source_transfers, coverage_entries,
        internal_only_authorities};
    NativeBringupCoverageObservations internal_only_observations;
    const auto internal_only_context =
        make_native_bringup_coverage_dispatch_context(
            table, runtime_image_bindings, binder, internal_only_pack,
            runtime_generation, internal_only_observations);
    request.target_hook =
        NativeBringupCoveragePreflightRequest::TargetHook::None;
    bool internal_call_rejected = false;
    try {
        static_cast<void>(preflight_native_bringup_coverage_dispatch(
            table, runtime_image_bindings, binder, internal_only_context,
            request));
    } catch (const NativeBringupDispatchError& error) {
        internal_call_rejected =
            error.miss() ==
            NativeBringupDispatchMiss::TargetCapabilityMismatch;
    }
    require(internal_call_rejected &&
                !binder.active_entry_for_address(target_runtime_start)
                     .has_value() &&
                internal_only_observations.total_occurrences() == 0u,
            "CallRegister akzeptierte einen reinen InternalBlock-/Tail-Entry "
            "oder aktivierte ihn vor der Capability-Pruefung.");

    const std::array ambiguous_target_authorities{
        target_authorities[0u],
        target_authorities[1u],
        target_authorities[2u],
        NativeBringupCoverageTargetAuthority{
            CoverageOwner::PrimaryStatic,
            CoverageCapability::Callable |
                CoverageCapability::TailJumpEntry,
            pack_identity,
            {},
            static_cast<std::uint32_t>(bytes.size()),
            target_runtime_start,
            target_runtime_start,
            0u,
            target_runtime_static_binding}};
    const NativeBringupCoverageDispatchPack ambiguous_target_pack{
        coverage_pack.identity, source_transfers, coverage_entries,
        ambiguous_target_authorities};
    NativeBringupCoverageObservations ambiguous_target_observations;
    const auto ambiguous_target_context =
        make_native_bringup_coverage_dispatch_context(
            table, runtime_image_bindings, binder, ambiguous_target_pack,
            runtime_generation, ambiguous_target_observations);
    bool ambiguous_target_rejected = false;
    try {
        static_cast<void>(preflight_native_bringup_coverage_dispatch(
            table, runtime_image_bindings, binder,
            ambiguous_target_context, request));
    } catch (const NativeBringupDispatchError& error) {
        ambiguous_target_rejected =
            error.miss() ==
            NativeBringupDispatchMiss::AmbiguousTargetOwner;
    }
    require(ambiguous_target_rejected && target_runtime_static_handle &&
                !binder.active_entry_for_address(target_runtime_start)
                     .has_value() &&
                ambiguous_target_observations.total_occurrences() == 0u,
            "Coverage-Preflight waehlt bei Static-/Loaded-AOT-Mehrdeutigkeit "
            "weiterhin nach Suchreihenfolge statt fail-closed.");

    const std::array authority_without_loaded_target{
        target_authorities[2u]};
    const NativeBringupCoverageDispatchPack missing_target_authority_pack{
        coverage_pack.identity, source_transfers, coverage_entries,
        authority_without_loaded_target};
    NativeBringupCoverageObservations missing_target_observations;
    const auto missing_target_context =
        make_native_bringup_coverage_dispatch_context(
            table, runtime_image_bindings, binder,
            missing_target_authority_pack, runtime_generation,
            missing_target_observations);
    const auto identity_bound_without_target_row =
        preflight_native_bringup_coverage_dispatch(
            table, runtime_image_bindings, binder, missing_target_context,
            request);
    const auto identity_bound_active =
        binder.active_entry_for_address(target_runtime_start);
    require(identity_bound_without_target_row.block == target_handle &&
                identity_bound_without_target_row.owner_kind ==
                    CoverageOwner::LoadedAot &&
                identity_bound_without_target_row.lifecycle_generation ==
                    target_lifecycle &&
                identity_bound_without_target_row.capabilities ==
                    CoverageCapability::Callable &&
                identity_bound_without_target_row.owner_identity ==
                    module_identity &&
                identity_bound_without_target_row.block_identity ==
                    module_identity &&
                identity_bound_active.has_value() &&
                identity_bound_active->active &&
                missing_target_observations.total_occurrences() == 1u,
            "Exakter Loaded-AOT-Block blieb ohne redundante per-address "
            "TargetAuthority-Zeile kuenstlich gesperrt.");

    RuntimeBlockTable complete_dispatch_only_table;
    complete_dispatch_only_table.bind_code_tracker(
        nullptr, StaticAotInvalidationContract::Coordinated);
    static_cast<void>(complete_dispatch_only_table.register_static(
        make_static_block(source_binding)));
    complete_dispatch_only_table.seal_static();
    const std::array<NativeBringupCoverageEntry, 0u>
        no_generated_placement_rows{};
    const std::array<NativeBringupCoverageTargetAuthority, 0u>
        no_generated_target_rows{};
    const NativeBringupCoverageDispatchPack
        complete_dispatch_only_pack{
            coverage_pack.identity,
            source_transfers,
            no_generated_placement_rows,
            no_generated_target_rows};
    NativeBringupCoverageObservations
        complete_dispatch_only_observations;
    const auto complete_dispatch_only_context =
        make_native_bringup_coverage_dispatch_context(
            complete_dispatch_only_table, runtime_image_bindings, binder,
            complete_dispatch_only_pack, runtime_generation,
            complete_dispatch_only_observations);
    const auto complete_dispatch_only_admission =
        preflight_native_bringup_coverage_dispatch(
            complete_dispatch_only_table, runtime_image_bindings, binder,
            complete_dispatch_only_context, request);
    require(!complete_dispatch_only_admission.block &&
                complete_dispatch_only_admission.execution.function ==
                    nullptr &&
                complete_dispatch_only_admission.generated_entry_required &&
                complete_dispatch_only_admission.owner_kind ==
                    CoverageOwner::LoadedAot &&
                complete_dispatch_only_admission.target ==
                    target_runtime_start &&
                complete_dispatch_only_admission.dispatch_source ==
                    target_source_start &&
                complete_dispatch_only_admission.owner_identity ==
                    module_identity &&
                complete_dispatch_only_admission.block_identity ==
                    module_identity &&
                complete_dispatch_only_admission.lifecycle_generation ==
                    target_lifecycle &&
                complete_dispatch_only_observations.total_occurrences() ==
                    1u,
            "Ein exakter Eintrag des vollstaendigen generierten Dispatchers "
            "blieb gesperrt, weil er nicht zusaetzlich in der kleineren "
            "Bring-up-RuntimeBlockTable dupliziert war.");

    const auto admitted = preflight_native_bringup_coverage_dispatch(
        table, runtime_image_bindings, binder, context, request);
    const auto active_target =
        binder.active_entry_for_address(target_runtime_start);
    const auto active_source =
        binder.active_entry_for_address(source_runtime_start);
    const auto coverage_events = coverage_observations.events();
    const auto coverage_json = coverage_observations.serialize_json();
    require(source_handle && target_handle &&
                admitted.block == target_handle &&
                !admitted.cache_hit &&
                admitted.execution.function == block &&
                admitted.execution.virtual_start == target_source_start &&
                admitted.target == target_runtime_start &&
                admitted.physical_target ==
                    canonical_physical_address(target_runtime_start) &&
                admitted.lifecycle_generation == target_lifecycle &&
                admitted.owner_kind == CoverageOwner::LoadedAot &&
                native_bringup_coverage_has_capability(
                    admitted.capabilities,
                    CoverageCapability::Callable) &&
                admitted.owner_identity == module_identity &&
                admitted.block_identity == module_identity &&
                admitted.dispatch_source == target_source_start &&
                active_target.has_value() && active_target->active &&
                active_target->lifecycle_generation == target_lifecycle &&
                active_source.has_value() && active_source->active &&
                active_source->lifecycle_generation == source_lifecycle &&
                coverage_observations.total_occurrences() == 1u &&
                coverage_observations.dropped_events() == 0u &&
                coverage_events.size() == 1u &&
                coverage_events.front().transfer_kind ==
                    NativeBringupTransferKind::CallRegister &&
                coverage_events.front().callsite == source_start &&
                coverage_events.front().target == target_runtime_start &&
                coverage_events.front().source_kind ==
                    NativeBringupCoverageSourceKind::LoadedAot &&
                coverage_events.front().source_module_identity ==
                    module_identity &&
                coverage_events.front().source_runtime_start ==
                    source_runtime_start &&
                coverage_events.front().source_lifecycle_generation ==
                    source_lifecycle &&
                coverage_events.front().source_image_id.empty() &&
                coverage_events.front().target_module_identity ==
                    module_identity &&
                coverage_events.front().target_owner_kind ==
                    CoverageOwner::LoadedAot &&
                coverage_events.front().target_image_id.empty() &&
                coverage_events.front().target_lifecycle_generation ==
                    target_lifecycle &&
                coverage_events.front().occurrences == 1u &&
                coverage_json.find("\"coverage_only\":true") !=
                    std::string::npos &&
                coverage_json.find("\"static_proof_promotions\":0") !=
                    std::string::npos,
            "Coverage-Preflight verlor kompiliertes Ziel, Modulidentitaet "
            "oder atomare Lifecycle-Generation, weil der Loader eine beim "
            "Export noch unbekannte Platzierung gewaehlt hatte.");

    const auto cached_admitted =
        preflight_native_bringup_coverage_dispatch(
            table, runtime_image_bindings, binder, context, request);
    require(cached_admitted.cache_hit &&
                cached_admitted.lifecycle_generation == target_lifecycle &&
                coverage_observations.total_occurrences() == 2u &&
                coverage_observations.events().size() == 1u &&
                coverage_observations.events().front().occurrences == 2u,
            "Coverage-Preflight cachete die unveraenderte geladene Kante nicht "
            "oder verlor deren bounded Witnesszaehler.");

    const std::array<NativeBringupCoverageSourceTransfer, 0u>
        no_source_transfers{};
    const std::array<NativeBringupCoverageEntry, 0u> no_placement_entries{};
    const NativeBringupCoverageDispatchPack compile_universe_only_pack{
        coverage_pack.identity, no_source_transfers, no_placement_entries,
        target_authorities};
    NativeBringupCoverageObservations compile_universe_observations;
    const auto compile_universe_only_context =
        make_native_bringup_coverage_dispatch_context(
            table, runtime_image_bindings, binder,
            compile_universe_only_pack, runtime_generation,
            compile_universe_observations);
    const auto compile_universe_admitted =
        preflight_native_bringup_coverage_dispatch(
            table, runtime_image_bindings, binder,
            compile_universe_only_context, request);
    const auto compile_universe_events =
        compile_universe_observations.events();
    require(compile_universe_admitted.block == target_handle &&
                compile_universe_admitted.execution.function == block &&
                compile_universe_admitted.execution.virtual_start ==
                    target_source_start &&
                compile_universe_admitted.target == target_runtime_start &&
                compile_universe_admitted.lifecycle_generation ==
                    target_lifecycle &&
                compile_universe_events.size() == 1u &&
                compile_universe_events.front().source_kind ==
                    NativeBringupCoverageSourceKind::LoadedAot &&
                compile_universe_events.front().source_module_identity ==
                    module_identity &&
                compile_universe_events.front().source_lifecycle_generation ==
                    source_lifecycle &&
                compile_universe_events.front().target_module_identity ==
                    module_identity &&
                compile_universe_events.front().target_lifecycle_generation ==
                    target_lifecycle,
            "Coverage-Preflight verlangte fuer einen exakten aktiven "
            "Compile-Universe-Block weiterhin eine Callsite- oder "
            "Platzierungstabelle.");

    auto unknown_source = request;
    unknown_source.callsite += 2u;
    bool unknown_source_rejected = false;
    try {
        static_cast<void>(preflight_native_bringup_coverage_dispatch(
            table, runtime_image_bindings, binder, context, unknown_source));
    } catch (const NativeBringupDispatchError& error) {
        unknown_source_rejected =
            error.miss() == NativeBringupDispatchMiss::CoverageSourceMissing;
    }
    require(unknown_source_rejected,
            "Kein exakter Compile-Universe-Block erbte eine "
            "Coverage-Zulassung.");

    auto static_source_transfers = source_transfers;
    static_source_transfers.front().source_kind =
        NativeBringupCoverageSourceKind::StaticAot;
    static_source_transfers.front().source_module_identity = {};
    static_source_transfers.front().source_runtime_start = 0u;
    static_source_transfers.front().source_module_size = 0u;
    static_source_transfers.front().source_module_offset = 0u;
    const NativeBringupCoverageDispatchPack static_target_pack{
        coverage_pack.identity, static_source_transfers, coverage_entries,
        target_authorities};
    NativeBringupCoverageObservations static_target_observations;
    const auto static_target_context =
        make_native_bringup_coverage_dispatch_context(
            table, runtime_image_bindings, binder, static_target_pack,
            runtime_generation,
            static_target_observations);
    auto static_target_request = request;
    static_target_request.target = target_source_start;
    const auto static_admitted =
        preflight_native_bringup_coverage_dispatch(
            table, runtime_image_bindings, binder, static_target_context,
            static_target_request);
    const auto static_events = static_target_observations.events();
    require(static_admitted.block == target_handle &&
                !static_admitted.cache_hit &&
                static_admitted.execution.function == block &&
                static_admitted.execution.virtual_start ==
                    target_source_start &&
                static_admitted.target == target_source_start &&
                static_admitted.physical_target ==
                    canonical_physical_address(target_source_start) &&
                static_admitted.lifecycle_generation == 0u &&
                static_admitted.owner_kind == CoverageOwner::PrimaryStatic &&
                static_admitted.owner_identity == pack_identity &&
                static_admitted.dispatch_source == target_source_start &&
                static_events.size() == 1u &&
                static_events.front().source_kind ==
                    NativeBringupCoverageSourceKind::StaticAot &&
                static_events.front().target == target_source_start &&
                static_events.front().target_module_identity ==
                    pack_identity &&
                static_events.front().target_owner_kind ==
                    CoverageOwner::PrimaryStatic &&
                static_events.front().target_image_id.empty() &&
                static_events.front().target_block_identity ==
                    module_identity &&
                static_events.front().target_runtime_start ==
                    target_source_start &&
                static_events.front().target_module_offset == 0u &&
                static_events.front().target_lifecycle_generation == 0u,
            "Coverage-Preflight wies ein exakt gebundenes Static-AOT-Ziel "
            "faelschlich als fehlendes Loaded-AOT-Modul ab oder erfand eine "
            "Lifecycle-Generation.");

    const std::array unrelated_target_authority{target_authorities[2u]};
    const NativeBringupCoverageDispatchPack sealed_primary_fallback_pack{
        coverage_pack.identity, static_source_transfers, coverage_entries,
        unrelated_target_authority};
    NativeBringupCoverageObservations sealed_primary_fallback_observations;
    const auto sealed_primary_fallback_context =
        make_native_bringup_coverage_dispatch_context(
            table, runtime_image_bindings, binder,
            sealed_primary_fallback_pack, runtime_generation,
            sealed_primary_fallback_observations);
    const auto sealed_primary_fallback =
        preflight_native_bringup_coverage_dispatch(
            table, runtime_image_bindings, binder,
            sealed_primary_fallback_context, static_target_request);
    const auto sealed_primary_fallback_events =
        sealed_primary_fallback_observations.events();
    require(sealed_primary_fallback.block == target_handle &&
                sealed_primary_fallback.execution.function == block &&
                sealed_primary_fallback.execution.virtual_start ==
                    target_source_start &&
                sealed_primary_fallback.target == target_source_start &&
                sealed_primary_fallback.lifecycle_generation == 0u &&
                sealed_primary_fallback.owner_kind ==
                    CoverageOwner::PrimaryStatic &&
                sealed_primary_fallback.capabilities ==
                    CoverageCapability::Callable &&
                sealed_primary_fallback.owner_identity == pack_identity &&
                sealed_primary_fallback.block_identity ==
                    target_binding.block_code_identity &&
                sealed_primary_fallback.dispatch_source ==
                    target_source_start &&
                sealed_primary_fallback_events.size() == 1u &&
                sealed_primary_fallback_events.front().target_owner_kind ==
                    CoverageOwner::PrimaryStatic &&
                sealed_primary_fallback_events.front().target_module_identity ==
                    pack_identity &&
                sealed_primary_fallback_events.front().target_block_identity ==
                    target_binding.block_code_identity,
            "Ein exakter, alleiniger versiegelter Primary-Static-AOT-Block "
            "blieb ohne redundanten TargetAuthority-Tabelleneintrag "
            "unaufrufbar.");

    const std::array declared_nonprimary_authority{target_authorities[1u]};
    const NativeBringupCoverageDispatchPack declared_nonprimary_pack{
        coverage_pack.identity, static_source_transfers, coverage_entries,
        declared_nonprimary_authority};
    NativeBringupCoverageObservations declared_nonprimary_observations;
    const auto declared_nonprimary_context =
        make_native_bringup_coverage_dispatch_context(
            table, runtime_image_bindings, binder, declared_nonprimary_pack,
            runtime_generation, declared_nonprimary_observations);
    bool declared_nonprimary_rejected = false;
    try {
        static_cast<void>(preflight_native_bringup_coverage_dispatch(
            table, runtime_image_bindings, binder,
            declared_nonprimary_context, static_target_request));
    } catch (const NativeBringupDispatchError& error) {
        declared_nonprimary_rejected =
            error.miss() == NativeBringupDispatchMiss::CoverageTargetMissing;
    }
    require(declared_nonprimary_rejected &&
                declared_nonprimary_observations.total_occurrences() == 0u,
            "Der versiegelte Primary-Static-Fallback umging eine deklarierte "
            "aber nicht aktive movable TargetAuthority.");

    const std::array runtime_image_source_transfers{
        NativeBringupCoverageSourceTransfer{
            NativeBringupTransferKind::CallRegister,
            runtime_image_source_start,
            runtime_image_source_start + 4u,
            NativeBringupCoverageSourceKind::RuntimeImage,
            runtime_image_binding,
            runtime_image_identity,
            runtime_image_id,
            runtime_image_runtime_start,
            static_cast<std::uint32_t>(bytes.size()),
            0u}};
    const NativeBringupCoverageDispatchPack runtime_image_pack{
        coverage_pack.identity,
        runtime_image_source_transfers,
        no_placement_entries,
        target_authorities};
    NativeBringupCoverageObservations runtime_image_observations;
    const auto runtime_image_context =
        make_native_bringup_coverage_dispatch_context(
            table, runtime_image_bindings, binder, runtime_image_pack,
            runtime_generation, runtime_image_observations);
    NativeBringupCoveragePreflightRequest runtime_image_request{
        NativeBringupTransferKind::CallRegister,
        runtime_image_source_start,
        runtime_image_runtime_start,
        runtime_image_source_start + 4u,
        runtime_image_binding.block,
        {0u, 0u, 0u, 0u, runtime_generation},
        NativeBringupCoveragePreflightRequest::TargetHook::None};
    const auto runtime_image_entry = runtime_image_bindings
                                         .active_entry_for_address(
                                             runtime_image_runtime_start);
    const auto runtime_image_admitted =
        preflight_native_bringup_coverage_dispatch(
            table, runtime_image_bindings, binder, runtime_image_context,
            runtime_image_request);
    const auto runtime_image_events = runtime_image_observations.events();
    require(runtime_image_entry.has_value() && runtime_image_handle &&
                runtime_image_admitted.block == runtime_image_handle &&
                runtime_image_admitted.execution.function == block &&
                runtime_image_admitted.execution.virtual_start ==
                    runtime_image_source_start &&
                runtime_image_admitted.target == runtime_image_runtime_start &&
                runtime_image_admitted.lifecycle_generation ==
                    runtime_image_entry->lifecycle_generation &&
                runtime_image_admitted.owner_kind ==
                    CoverageOwner::RuntimeImage &&
                runtime_image_admitted.owner_identity ==
                    runtime_image_identity &&
                runtime_image_admitted.owner_image_id == runtime_image_id &&
                runtime_image_admitted.dispatch_source ==
                    runtime_image_source_start &&
                runtime_image_events.size() == 1u &&
                runtime_image_events.front().source_kind ==
                    NativeBringupCoverageSourceKind::RuntimeImage &&
                runtime_image_events.front().source_module_identity ==
                    runtime_image_identity &&
                runtime_image_events.front().source_runtime_start ==
                    runtime_image_runtime_start &&
                runtime_image_events.front().source_image_id ==
                    runtime_image_id &&
                runtime_image_events.front().target_module_identity ==
                    runtime_image_identity &&
                runtime_image_events.front().target_owner_kind ==
                    CoverageOwner::RuntimeImage &&
                runtime_image_events.front().target_image_id ==
                    runtime_image_id &&
                runtime_image_events.front().target_runtime_start ==
                    runtime_image_runtime_start,
            "Coverage-Preflight verlor die exakte RuntimeImage-Identitaet, "
            "deren aktive Generation oder den source-space AOT-Block.");
    require(runtime_image_bindings.deactivate_runtime_range(
                runtime_image_runtime_start, bytes.size()) == 1u,
            "RuntimeImage-Fixture konnte nicht retiret werden.");
    bool retired_runtime_image_rejected = false;
    try {
        static_cast<void>(preflight_native_bringup_coverage_dispatch(
            table, runtime_image_bindings, binder, runtime_image_context,
            runtime_image_request));
    } catch (const NativeBringupDispatchError& error) {
        retired_runtime_image_rejected =
            error.miss() == NativeBringupDispatchMiss::RuntimeImageInactive;
    }
    require(retired_runtime_image_rejected,
            "Coverage-Preflight verwendete ein retiertes RuntimeImage oder "
            "einen stalen Cachetreffer.");

    const auto stamp_before_retirement = binder.dispatch_stamp();
    require(binder.deactivate_runtime_range(
                target_runtime_start, bytes.size()) == 1u &&
                binder.dispatch_stamp() != stamp_before_retirement,
            "Loaded-AOT-Retirement aenderte den Dispatch-Cache-Stamp nicht.");
    bool retired_cache_rejected = false;
    try {
        static_cast<void>(preflight_native_bringup_coverage_dispatch(
            table, runtime_image_bindings, binder, context, request));
    } catch (const NativeBringupDispatchError& error) {
        retired_cache_rejected =
            error.miss() ==
            NativeBringupDispatchMiss::CoverageTargetMissing;
    }
    require(retired_cache_rejected,
            "Coverage-Preflight verwendete nach Lifecycle-Retirement einen "
            "stalen Cachetreffer.");

    auto bounded_event = coverage_observations.events().front();
    coverage_observations.clear();
    for (std::size_t index = 0u;
         index < native_bringup_coverage_observation_capacity + 1u;
         ++index) {
        bounded_event.callsite = static_cast<std::uint32_t>(index * 2u);
        coverage_observations.record(bounded_event);
    }
    require(coverage_observations.events().size() ==
                    native_bringup_coverage_observation_capacity &&
                coverage_observations.total_occurrences() ==
                    native_bringup_coverage_observation_capacity + 1u &&
                coverage_observations.dropped_events() == 1u,
            "Coverage-Witnessindex ueberschritt seine feste 4096er Grenze.");
}

void native_bringup_allowlist_regression() {
    static_assert(!NativeBringupDispatchContext::release_eligible);
    constexpr std::string_view sha_a =
        "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    constexpr std::string_view sha_b =
        "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    constexpr std::string_view sha_c =
        "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    constexpr std::string_view sha_d =
        "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    constexpr std::string_view sha_e =
        "sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    constexpr std::string_view sha_f =
        "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    constexpr std::string_view sha_1 =
        "sha256:1111111111111111111111111111111111111111111111111111111111111111";
    constexpr std::string_view sha_2 =
        "sha256:2222222222222222222222222222222222222222222222222222222222222222";
    constexpr std::string_view sha_3 =
        "sha256:3333333333333333333333333333333333333333333333333333333333333333";
    constexpr std::string_view sha_4 =
        "sha256:4444444444444444444444444444444444444444444444444444444444444444";
    constexpr std::string_view sha_5 =
        "sha256:5555555555555555555555555555555555555555555555555555555555555555";
    constexpr std::string_view sha_6 =
        "sha256:6666666666666666666666666666666666666666666666666666666666666666";
    constexpr std::string_view sha_7 =
        "sha256:7777777777777777777777777777777777777777777777777777777777777777";
    constexpr std::string_view sha_8 =
        "sha256:8888888888888888888888888888888888888888888888888888888888888888";
    constexpr std::string_view sha_9 =
        "sha256:9999999999999999999999999999999999999999999999999999999999999999";
    constexpr std::string_view sha_0 =
        "sha256:0000000000000000000000000000000000000000000000000000000000000000";
    constexpr std::string_view aot_pack_identity =
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    constexpr std::uint32_t callsite = 0x8C006004u;
    constexpr std::uint32_t target = 0x8C007000u;
    constexpr std::uint32_t second_callsite = 0x8C006104u;
    constexpr std::uint32_t second_target = 0x8C008000u;
    constexpr std::uint64_t aot_pack_generation = 13u;
    constexpr std::uint64_t runtime_generation = 29u;
    const BlockAddress source{callsite - 4u,
                              canonical_physical_address(callsite - 4u)};
    const BlockAddress second_source{
        second_callsite - 4u,
        canonical_physical_address(second_callsite - 4u)};

    NativeBringupDispatchEntry allowed;
    allowed.admission.stage = NativeBringupEvidenceStage::Proven;
    allowed.admission.transfer_kind =
        NativeBringupTransferKind::CallRegister;
    allowed.admission.source_owner = source.virtual_address - 0x20u;
    allowed.admission.source_owner_size = 0x40u;
    allowed.admission.source_block = source.virtual_address;
    allowed.admission.source_block_size = 8u;
    allowed.admission.callsite = callsite;
    allowed.admission.continuation = callsite + 4u;
    allowed.admission.source_owner_code_identity = sha_c;
    allowed.admission.source_block_code_identity = sha_9;
    allowed.admission.callsite_code_identity = sha_d;
    allowed.admission.target = target;
    allowed.admission.target_block_size = 4u;
    allowed.admission.target_owner = target;
    allowed.admission.target_owner_size = 0x20u;
    allowed.admission.target_block_code_identity = sha_e;
    allowed.admission.target_owner_code_identity = sha_f;
    allowed.admission.source_image_id = "fixture-source-image-a";
    allowed.admission.target_image_id = "fixture-target-image-a";
    allowed.admission.source_module_identity = sha_1;
    allowed.admission.target_module_identity = sha_2;
    allowed.admission.source_generation = aot_pack_generation;
    allowed.admission.target_generation = aot_pack_generation;
    allowed.admission.proposed_promotion =
        NativeBringupPromotionType::StaticCompiledTarget;
    allowed.source = {source,
                      allowed.admission.source_block_size,
                      BlockEndKind::Call,
                      allowed.admission.source_block_code_identity};
    allowed.target = {{target, canonical_physical_address(target)},
                      allowed.admission.target_block_size,
                      BlockEndKind::Return,
                      allowed.admission.target_block_code_identity};

    auto second_allowed = allowed;
    second_allowed.admission.stage = NativeBringupEvidenceStage::Candidate;
    second_allowed.admission.proposed_promotion =
        NativeBringupPromotionType::AnalyzerReproof;
    second_allowed.admission.missing_proof =
        "strict-frontier-proof-remains-open";
    second_allowed.admission.static_correlation =
        "export-revalidated-static-correlation";
    second_allowed.admission.analyzer_path =
        "control-flow/indirect-dispatch";
    second_allowed.admission.source_owner =
        second_source.virtual_address - 0x20u;
    second_allowed.admission.source_block = second_source.virtual_address;
    second_allowed.admission.transfer_kind =
        NativeBringupTransferKind::TailJumpRegister;
    second_allowed.admission.callsite = second_callsite;
    second_allowed.admission.continuation = 0u;
    second_allowed.admission.source_owner_code_identity = sha_3;
    second_allowed.admission.source_block_code_identity = sha_0;
    second_allowed.admission.callsite_code_identity = sha_4;
    second_allowed.admission.target = second_target;
    second_allowed.admission.target_owner = second_target;
    second_allowed.admission.target_block_code_identity = sha_5;
    second_allowed.admission.target_owner_code_identity = sha_6;
    second_allowed.admission.source_image_id = "fixture-source-image-b";
    second_allowed.admission.target_image_id = "fixture-target-image-b";
    second_allowed.admission.source_module_identity = sha_7;
    second_allowed.admission.target_module_identity = sha_8;
    second_allowed.admission.source_generation = aot_pack_generation;
    second_allowed.admission.target_generation = aot_pack_generation;
    second_allowed.source = {second_source,
                             second_allowed.admission.source_block_size,
                             BlockEndKind::DynamicBranch,
                             second_allowed.admission.source_block_code_identity};
    second_allowed.target = {
        {second_target, canonical_physical_address(second_target)},
        second_allowed.admission.target_block_size,
        BlockEndKind::Return,
        second_allowed.admission.target_block_code_identity};

    const NativeBringupDispatchPackIdentity pack_identity{
        native_bringup_evidence_contract_version,
        sha_a,
        "fixture-project",
        "fixture-v1",
        sha_b,
        aot_pack_identity,
        aot_pack_generation};
    const std::array allowlist{allowed, second_allowed};
    const NativeBringupDispatchPack pack{pack_identity, allowlist};

    IndirectDispatchRequest base_request;
    base_request.kind = IndirectDispatchKind::Call;
    base_request.callsite = callsite;
    base_request.target = target;
    base_request.return_address = callsite + 4u;
    base_request.source = source;
    base_request.variant.runtime_generation = runtime_generation;
    base_request.resolution_origin = DispatchResolutionOrigin::RuntimeOnly;
    base_request.dispatch_class = RuntimeDispatchClass::RuntimeOnly;

    {
        RuntimeBlockTable strict_table;
        CpuState strict_cpu;
        strict_cpu.write_sr(sr_md_mask);
        strict_cpu.pc = 0x8C000100u;
        strict_cpu.pr = 0x8C000200u;
        const auto pc_before = strict_cpu.pc;
        const auto pr_before = strict_cpu.pr;
        bool rejected = false;
        try {
            static_cast<void>(
                dispatch_indirect(strict_cpu, strict_table, base_request));
        } catch (const IndirectDispatchError& error) {
            rejected = error.error() == DispatchDiagnosticError::UnknownTarget &&
                       error.native_bringup_miss() == NativeBringupDispatchMiss::None;
        }
        require(rejected && strict_cpu.pc == pc_before &&
                    strict_cpu.pr == pr_before,
                "Strict-Product-Dispatch akzeptiert ohne expliziten Bring-up-Kontext ein "
                "fehlendes statisches Ziel oder mutiert den CPU-Zustand.");
    }

    const auto make_static_block = [](
                                       const NativeBringupDispatchStaticAotBinding& binding,
                                       const std::uint64_t active_runtime_generation) {
        RuntimeBlock native{binding.block.virtual_address,
                            binding.block.physical_address,
                            binding.size,
                            binding.end_kind,
                            {0u, 0u, 0u, 0u, active_runtime_generation},
                            block,
                            std::string(binding.block_code_identity),
                            false};
        native.static_variant_policy =
            StaticVariantPolicy::DirectP1P2RuntimeStateAgnostic;
        return native;
    };

    RuntimeBlockTable bringup_table;
    bringup_table.bind_code_tracker(
        nullptr, StaticAotInvalidationContract::Coordinated);
    const auto source_registered = bringup_table.register_static(
        make_static_block(allowed.source, runtime_generation));
    const auto registered = bringup_table.register_static(
        make_static_block(allowed.target, runtime_generation));
    const auto second_source_registered = bringup_table.register_static(
        make_static_block(second_allowed.source, runtime_generation));
    const auto second_registered = bringup_table.register_static(
        make_static_block(second_allowed.target, runtime_generation));
    bringup_table.seal_static();
    const auto table_generation_before = bringup_table.dispatch_generation();
    NativeBringupDispatchObservations observations;
    const auto context = make_native_bringup_dispatch_context(
        bringup_table, pack, runtime_generation, observations);
    IndirectDispatchMetrics metrics;
    auto request = base_request;
    request.metrics = &metrics;
    request.native_bringup = &context;

    const auto preflight = preflight_native_bringup_dispatch(
        bringup_table,
        context,
        {NativeBringupTransferKind::CallRegister,
         base_request.callsite,
         base_request.target,
         base_request.return_address,
         base_request.source,
         base_request.variant});
    require(preflight.block == registered &&
                preflight.execution.function == block &&
                preflight.target == target &&
                preflight.physical_target ==
                    canonical_physical_address(target) &&
                bringup_table.static_dispatch_generation_guard_current(
                    preflight.execution.generation_guard) &&
                metrics.hits() == 0u && metrics.misses() == 0u &&
                observations.total_occurrences() == 0u &&
                bringup_table.dispatch_generation() == table_generation_before,
            "Reiner Bring-up-Preflight mutiert Telemetrie/Tabelle oder verliert "
            "den validierten Target-Guard.");
    auto missing_preflight_request = NativeBringupDispatchPreflightRequest{
        NativeBringupTransferKind::CallRegister,
        base_request.callsite,
        base_request.target + 2u,
        base_request.return_address,
        base_request.source,
        base_request.variant};
    bool pure_preflight_miss = false;
    try {
        static_cast<void>(preflight_native_bringup_dispatch(
            bringup_table, context, missing_preflight_request));
    } catch (const NativeBringupDispatchError& error) {
        pure_preflight_miss =
            error.miss() ==
            NativeBringupDispatchMiss::UnknownCompiledTarget;
    }
    require(pure_preflight_miss && metrics.hits() == 0u &&
                metrics.misses() == 0u &&
                observations.total_occurrences() == 0u &&
                bringup_table.dispatch_generation() == table_generation_before,
            "Bring-up-Preflight-Miss schreibt Telemetrie oder mutiert die Tabelle.");

    RuntimeBlockTable alternate_table;
    alternate_table.bind_code_tracker(
        nullptr, StaticAotInvalidationContract::Coordinated);
    static_cast<void>(alternate_table.register_static(
        make_static_block(allowed.source, runtime_generation)));
    static_cast<void>(alternate_table.register_static(
        make_static_block(allowed.target, runtime_generation)));
    static_cast<void>(alternate_table.register_static(
        make_static_block(second_allowed.source, runtime_generation)));
    static_cast<void>(alternate_table.register_static(
        make_static_block(second_allowed.target, runtime_generation)));
    alternate_table.seal_static();
    bool alternate_table_rejected = false;
    try {
        static_cast<void>(preflight_native_bringup_dispatch(
            alternate_table,
            context,
            {NativeBringupTransferKind::CallRegister,
             base_request.callsite,
             base_request.target,
             base_request.return_address,
             base_request.source,
             base_request.variant}));
    } catch (const NativeBringupDispatchError& error) {
        alternate_table_rejected =
            error.miss() ==
            NativeBringupDispatchMiss::InvalidEntry;
    }
    require(alternate_table_rejected &&
                observations.total_occurrences() == 0u,
            "Validierter Bring-up-Context konnte gegen eine andere versiegelte "
            "Static-AOT-Tabelle verwendet werden.");

    CpuState cpu;
    cpu.write_sr(sr_md_mask);
    cpu.pc = source.virtual_address;
    cpu.pr = 0x8C000300u;
    const auto first = dispatch_indirect(cpu, bringup_table, request);

    auto second_request = base_request;
    second_request.kind = IndirectDispatchKind::TailJump;
    second_request.callsite = second_callsite;
    second_request.target = second_target;
    second_request.return_address = 0u;
    second_request.source = second_source;
    second_request.metrics = &metrics;
    second_request.native_bringup = &context;
    CpuState second_cpu;
    second_cpu.write_sr(sr_md_mask);
    second_cpu.pc = second_source.virtual_address;
    constexpr std::uint32_t second_pr = 0x8C000320u;
    second_cpu.pr = second_pr;
    const auto second =
        dispatch_indirect(second_cpu, bringup_table, second_request);
    require(first.native_bringup && second.native_bringup &&
                source_registered && second_source_registered &&
                first.block == registered && second.block == second_registered &&
                first.execution.function == block &&
                first.execution.virtual_start == target &&
                first.execution.physical_origin ==
                    canonical_physical_address(target) &&
                first.execution.variant.runtime_generation == runtime_generation &&
                first.execution.provenance ==
                    allowed.target.block_code_identity &&
                first.execution.generation_guard.kind ==
                    BlockDispatchGenerationGuardKind::StaticAot &&
                first.execution.generation_guard_reusable &&
                bringup_table.static_dispatch_generation_guard_current(
                    first.execution.generation_guard) &&
                first.resulting_pc == target &&
                first.resulting_pr == base_request.return_address &&
                second.resulting_pc == second_target &&
                second.resulting_pr == second_pr &&
                cpu.pc == target && cpu.pr == base_request.return_address &&
                second_cpu.pc == second_target &&
                second_cpu.pr == second_pr &&
                bringup_table.size() == 4u &&
                bringup_table.dispatch_generation() == table_generation_before &&
                metrics.hits() == 2u && metrics.misses() == 0u &&
                metrics.runtime_only_dispatch_share_ppm() == 1'000'000u &&
                observations.total_occurrences() == 2u &&
                observations.events().size() == 2u &&
                observations.events().front().executed &&
                observations.events().front().aot_pack_generation ==
                    aot_pack_generation &&
                observations.events().front().runtime_generation == runtime_generation &&
                observations.events().front().occurrences == 1u &&
                allowed.admission.source_owner_code_identity !=
                    second_allowed.admission.source_owner_code_identity &&
                allowed.admission.target_owner_code_identity !=
                    second_allowed.admission.target_owner_code_identity &&
                second_allowed.admission.stage ==
                    NativeBringupEvidenceStage::Candidate &&
                second_allowed.admission.proposed_promotion ==
                    NativeBringupPromotionType::AnalyzerReproof &&
                !second_allowed.admission.missing_proof.empty(),
            "Multi-Owner-Pack verwendet nicht ausschliesslich aktive versiegelte "
            "Source-/Target-Static-AOT-Handles oder mutiert Tabelle/Beobachtung.");
    const auto observation_json = observations.serialize_json();
    require(observation_json.find("katana-native-bringup-dispatch-v1") !=
                    std::string::npos &&
                observation_json.find("\"non_release\":true") !=
                    std::string::npos &&
                observation_json.find("\"proof\":\"incomplete\"") !=
                    std::string::npos &&
                observation_json.find("\"static_proof_promotions\":0") !=
                    std::string::npos &&
                observation_json.find("\"aot_pack_generation\":13") !=
                    std::string::npos &&
                observation_json.find("\"runtime_generation\":29") !=
                    std::string::npos,
            "Kompakter Bring-up-Snapshot verliert Non-Release-/No-Proof-Vertrag.");

    RuntimeBlockTable rebuilt_table;
    rebuilt_table.bind_code_tracker(
        nullptr, StaticAotInvalidationContract::Coordinated);
    static_cast<void>(rebuilt_table.register_static(
        make_static_block(allowed.source, runtime_generation + 1u)));
    const auto rebuilt_registered = rebuilt_table.register_static(
        make_static_block(allowed.target, runtime_generation + 1u));
    static_cast<void>(rebuilt_table.register_static(
        make_static_block(second_allowed.source, runtime_generation + 1u)));
    static_cast<void>(rebuilt_table.register_static(
        make_static_block(second_allowed.target, runtime_generation + 1u)));
    rebuilt_table.seal_static();
    NativeBringupDispatchObservations rebuilt_observations;
    const auto rebuilt_context = make_native_bringup_dispatch_context(
        rebuilt_table,
        pack,
        runtime_generation + 1u,
        rebuilt_observations);
    auto rebuilt_request = base_request;
    rebuilt_request.variant.runtime_generation += 1u;
    rebuilt_request.native_bringup = &rebuilt_context;
    CpuState rebuilt_cpu;
    rebuilt_cpu.write_sr(sr_md_mask);
    const auto rebuilt =
        dispatch_indirect(rebuilt_cpu, rebuilt_table, rebuilt_request);
    require(rebuilt.native_bringup && rebuilt.block == rebuilt_registered &&
                rebuilt.execution.function == block &&
                rebuilt.execution.variant.runtime_generation ==
                    runtime_generation + 1u &&
                rebuilt_table.static_dispatch_generation_guard_current(
                    rebuilt.execution.generation_guard) &&
                rebuilt_observations.events().size() == 1u &&
                rebuilt_observations.events().front().runtime_generation ==
                    runtime_generation + 1u,
            "Unveraenderter AOT-Pack kann nach Runtime-/Adapter-Rebuild nicht gegen die "
            "neu versiegelte aktive Static-AOT-Generation gebunden werden.");

    const auto expect_context_rejection =
        [&](const RuntimeBlockTable& rejected_table,
            const NativeBringupDispatchPack& candidate_pack,
            const std::uint64_t active_runtime_generation) {
        NativeBringupDispatchObservations rejected_observations;
        const auto table_size_before = rejected_table.size();
        const auto table_generation_before =
            rejected_table.dispatch_generation();
        bool rejected = false;
        try {
            static_cast<void>(make_native_bringup_dispatch_context(
                rejected_table,
                candidate_pack,
                active_runtime_generation,
                rejected_observations));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected && rejected_observations.total_occurrences() == 0u &&
                    rejected_table.size() == table_size_before &&
                    rejected_table.dispatch_generation() ==
                        table_generation_before,
                "Ungueltiger Bring-up-Pack/Tabellen-Seal wurde nicht rein bei "
                "Context-Erzeugung verworfen.");
    };

    const auto expect_rejection =
        [&](RuntimeBlockTable& rejected_table,
            const NativeBringupDispatchPack& candidate_pack,
            const std::uint64_t active_runtime_generation,
            IndirectDispatchRequest rejected_request,
            const NativeBringupDispatchMiss expected_miss,
            const DispatchDiagnosticError expected_error) {
        NativeBringupDispatchObservations rejected_observations;
        const auto rejected_context = make_native_bringup_dispatch_context(
            rejected_table,
            candidate_pack,
            active_runtime_generation,
            rejected_observations);
        IndirectDispatchMetrics rejected_metrics;
        rejected_request.metrics = &rejected_metrics;
        rejected_request.native_bringup = &rejected_context;
        CpuState rejected_cpu;
        rejected_cpu.write_sr(sr_md_mask);
        rejected_cpu.pc = 0x8C000400u;
        rejected_cpu.pr = 0x8C000500u;
        const auto pc_before = rejected_cpu.pc;
        const auto pr_before = rejected_cpu.pr;
        const auto table_size_before = rejected_table.size();
        const auto table_generation_before_rejection =
            rejected_table.dispatch_generation();
        try {
            static_cast<void>(dispatch_indirect(
                rejected_cpu, rejected_table, rejected_request));
        } catch (const IndirectDispatchError& error) {
            require(error.error() == expected_error &&
                        error.native_bringup_miss() == expected_miss &&
                        rejected_cpu.pc == pc_before &&
                        rejected_cpu.pr == pr_before &&
                        rejected_table.size() == table_size_before &&
                        rejected_table.dispatch_generation() ==
                            table_generation_before_rejection &&
                        rejected_metrics.hits() == 0u &&
                        rejected_metrics.misses() == 1u &&
                        rejected_observations.total_occurrences() == 1u &&
                        rejected_observations.events().size() == 1u &&
                        !rejected_observations.events().front().executed &&
                        rejected_observations.events().front().miss == expected_miss,
                    "Bring-up-Miss ist nicht fatal, transaktional oder strukturiert.");
            return;
        }
        throw std::runtime_error("Ungueltiges Bring-up-Ziel wurde akzeptiert.");
    };

    auto unknown_request = base_request;
    unknown_request.target += 2u;
    expect_rejection(bringup_table,
                     pack,
                     runtime_generation,
                     unknown_request,
                     NativeBringupDispatchMiss::UnknownCompiledTarget,
                     DispatchDiagnosticError::UnknownTarget);

    auto missing_pack_identity = pack_identity;
    missing_pack_identity.authoring_artifact_identity = {};
    const NativeBringupDispatchPack missing_identity_pack{
        missing_pack_identity, allowlist};
    expect_context_rejection(
        bringup_table, missing_identity_pack, runtime_generation);

    auto raw_hash_pack_identity = pack_identity;
    raw_hash_pack_identity.authoring_artifact_identity = sha_a.substr(7u);
    const NativeBringupDispatchPack raw_hash_pack{
        raw_hash_pack_identity, allowlist};
    expect_context_rejection(bringup_table, raw_hash_pack, runtime_generation);
    auto wrong_runtime_request = base_request;
    ++wrong_runtime_request.variant.runtime_generation;
    expect_rejection(bringup_table,
                     pack,
                     runtime_generation,
                     wrong_runtime_request,
                     NativeBringupDispatchMiss::GenerationMismatch,
                     DispatchDiagnosticError::GenerationMismatch);

    auto wrong_pack_generation_entry = allowed;
    ++wrong_pack_generation_entry.admission.target_generation;
    const std::array wrong_generation_allowlist{wrong_pack_generation_entry};
    const NativeBringupDispatchPack wrong_generation_pack{
        pack_identity, wrong_generation_allowlist};
    expect_context_rejection(
        bringup_table, wrong_generation_pack, runtime_generation);

    const std::array unsorted_allowlist{second_allowed, allowed};
    const NativeBringupDispatchPack unsorted_pack{
        pack_identity, unsorted_allowlist};
    expect_context_rejection(bringup_table, unsorted_pack, runtime_generation);

    std::vector<NativeBringupDispatchEntry> oversized_allowlist(
        native_bringup_dispatch_maximum_entries + 1u, allowed);
    const NativeBringupDispatchPack oversized_pack{
        pack_identity, oversized_allowlist};
    expect_context_rejection(bringup_table, oversized_pack, runtime_generation);

    auto nonterminal_callsite = allowed;
    nonterminal_callsite.admission.source_block_size += 2u;
    const std::array nonterminal_allowlist{nonterminal_callsite};
    const NativeBringupDispatchPack nonterminal_pack{
        pack_identity, nonterminal_allowlist};
    expect_context_rejection(
        bringup_table, nonterminal_pack, runtime_generation);

    auto crossing_alias = allowed;
    crossing_alias.admission.target = 0x9FFFFFFCu;
    crossing_alias.admission.target_block_size = 6u;
    crossing_alias.admission.target_owner = crossing_alias.admission.target;
    crossing_alias.admission.target_owner_size = 6u;
    crossing_alias.target = {
        {crossing_alias.admission.target,
         canonical_physical_address(crossing_alias.admission.target)},
        crossing_alias.admission.target_block_size,
        BlockEndKind::Return,
        crossing_alias.admission.target_block_code_identity};
    const std::array crossing_alias_allowlist{crossing_alias};
    const NativeBringupDispatchPack crossing_alias_pack{
        pack_identity, crossing_alias_allowlist};
    expect_context_rejection(
        bringup_table, crossing_alias_pack, runtime_generation);

    auto wrong_source_request = base_request;
    wrong_source_request.source.virtual_address += 2u;
    wrong_source_request.source.physical_address += 2u;
    expect_rejection(bringup_table,
                     pack,
                     runtime_generation,
                     wrong_source_request,
                     NativeBringupDispatchMiss::SourceIdentityMismatch,
                     DispatchDiagnosticError::ByteIdentityMismatch);
    auto wrong_continuation_request = base_request;
    wrong_continuation_request.return_address += 2u;
    expect_rejection(bringup_table,
                     pack,
                     runtime_generation,
                     wrong_continuation_request,
                     NativeBringupDispatchMiss::SourceIdentityMismatch,
                     DispatchDiagnosticError::ByteIdentityMismatch);

    auto runtime_contract = allowed;
    runtime_contract.admission.stage =
        NativeBringupEvidenceStage::RuntimeContract;
    runtime_contract.admission.proposed_promotion =
        NativeBringupPromotionType::ValidatedRuntimeContract;
    runtime_contract.admission.runtime_contract_identity = sha_8;
    const std::array runtime_contract_allowlist{runtime_contract};
    const NativeBringupDispatchPack runtime_contract_pack{
        pack_identity, runtime_contract_allowlist};
    expect_context_rejection(
        bringup_table, runtime_contract_pack, runtime_generation);

    auto incomplete_candidate = second_allowed;
    incomplete_candidate.admission.missing_proof = {};
    const std::array incomplete_candidate_allowlist{incomplete_candidate};
    const NativeBringupDispatchPack incomplete_candidate_pack{
        pack_identity, incomplete_candidate_allowlist};
    expect_context_rejection(
        bringup_table, incomplete_candidate_pack, runtime_generation);

    auto observed = allowed;
    observed.admission.stage = NativeBringupEvidenceStage::Observed;
    const std::array observed_allowlist{observed};
    const NativeBringupDispatchPack observed_pack{
        pack_identity, observed_allowlist};
    expect_context_rejection(bringup_table, observed_pack, runtime_generation);

    auto unresolved = allowed;
    unresolved.admission.stage = NativeBringupEvidenceStage::Unresolved;
    const std::array unresolved_allowlist{unresolved};
    const NativeBringupDispatchPack unresolved_pack{
        pack_identity, unresolved_allowlist};
    expect_context_rejection(
        bringup_table, unresolved_pack, runtime_generation);

    auto malformed_duplicate = allowed;
    malformed_duplicate.admission.target_block_code_identity = {};
    const std::array duplicate_allowlist{allowed, malformed_duplicate};
    const NativeBringupDispatchPack duplicate_pack{
        pack_identity, duplicate_allowlist};
    expect_context_rejection(bringup_table, duplicate_pack, runtime_generation);

    RuntimeBlockTable wrong_source_table;
    wrong_source_table.bind_code_tracker(
        nullptr, StaticAotInvalidationContract::Coordinated);
    auto wrong_source = make_static_block(allowed.source, runtime_generation);
    wrong_source.provenance = "wrong-source-provenance";
    static_cast<void>(wrong_source_table.register_static(std::move(wrong_source)));
    static_cast<void>(wrong_source_table.register_static(
        make_static_block(allowed.target, runtime_generation)));
    wrong_source_table.seal_static();
    const std::array first_only_allowlist{allowed};
    const NativeBringupDispatchPack first_only_pack{
        pack_identity, first_only_allowlist};
    expect_context_rejection(
        wrong_source_table, first_only_pack, runtime_generation);

    RuntimeBlockTable wrong_target_table;
    wrong_target_table.bind_code_tracker(
        nullptr, StaticAotInvalidationContract::Coordinated);
    static_cast<void>(wrong_target_table.register_static(
        make_static_block(allowed.source, runtime_generation)));
    auto wrong_target = make_static_block(allowed.target, runtime_generation);
    wrong_target.provenance = "wrong-target-provenance";
    static_cast<void>(wrong_target_table.register_static(std::move(wrong_target)));
    wrong_target_table.seal_static();
    expect_context_rejection(
        wrong_target_table, first_only_pack, runtime_generation);

    RuntimeBlockTable missing_target_table;
    missing_target_table.bind_code_tracker(
        nullptr, StaticAotInvalidationContract::Coordinated);
    static_cast<void>(missing_target_table.register_static(
        make_static_block(allowed.source, runtime_generation)));
    missing_target_table.seal_static();
    expect_context_rejection(
        missing_target_table, first_only_pack, runtime_generation);

    const auto stale_guard = first.execution.generation_guard;
    require(bringup_table.erase_overlapping_physical(
                allowed.target.block.physical_address,
                allowed.target.size) == 1u &&
                !bringup_table.static_dispatch_generation_guard_current(stale_guard),
            "Static-AOT-Invalidierung laesst den vorherigen Bring-up-Guard aktiv.");
    const auto observations_before_stale_preflight =
        observations.total_occurrences();
    bool stale_preflight_rejected = false;
    try {
        static_cast<void>(preflight_native_bringup_dispatch(
            bringup_table,
            context,
            {NativeBringupTransferKind::CallRegister,
             base_request.callsite,
             base_request.target,
             base_request.return_address,
             base_request.source,
             base_request.variant}));
    } catch (const NativeBringupDispatchError& error) {
        stale_preflight_rejected =
            error.miss() ==
            NativeBringupDispatchMiss::InvalidEntry;
    }
    require(stale_preflight_rejected &&
                observations.total_occurrences() ==
                    observations_before_stale_preflight,
            "Staler Preflight-Guard bleibt gueltig oder schreibt Observationen.");
    const auto pc_before_invalidation = cpu.pc;
    const auto pr_before_invalidation = cpu.pr;
    try {
        static_cast<void>(dispatch_indirect(cpu, bringup_table, request));
        throw std::runtime_error(
            "Invalidierter Static-AOT-Block wurde im Bring-up erneut ausgefuehrt.");
    } catch (const IndirectDispatchError& error) {
        require(error.native_bringup_miss() ==
                        NativeBringupDispatchMiss::InvalidEntry &&
                    cpu.pc == pc_before_invalidation &&
                    cpu.pr == pr_before_invalidation,
                "Invalidierter Static-AOT-Block bleibt dispatchbar oder mutiert CPU-Zustand.");
    }

    NativeBringupDispatchObservations bounded;
    for (std::size_t index = 0u;
         index < native_bringup_dispatch_observation_capacity + 1u;
         ++index) {
        bounded.record(false,
                       NativeBringupDispatchMiss::UnknownCompiledTarget,
                       NativeBringupTransferKind::TailJumpRegister,
                       static_cast<std::uint32_t>(index * 2u),
                       static_cast<std::uint32_t>(0x1000u + index * 2u),
                       aot_pack_generation,
                       runtime_generation);
    }
    require(bounded.events().size() ==
                    native_bringup_dispatch_observation_capacity &&
                bounded.total_occurrences() ==
                    native_bringup_dispatch_observation_capacity + 1u &&
                bounded.dropped_events() == 1u,
            "Bring-up-Beobachtungen wachsen ueber ihre feste Kompaktgrenze.");
}
} // namespace

int main() {
    try {
        static_aot_p2_alias_regression();
        materializer_lifecycle_regression();
        runtime_aot_alias_lifetime_regression();
        missing_aot_dispatch_regression();
        materialization_identity_diagnostic_regression();
        runtime_only_hit_hotloop_regression();
        native_bringup_coverage_regression();
        native_bringup_allowlist_regression();
        RuntimeBlockTable table;
        const BlockVariantKey variant{1u, 0u, 0u, 0u, 0u};
        static_cast<void>(table.register_static({0x8C001000u,
                                                 0x0C001000u,
                                                 4u,
                                                 BlockEndKind::Return,
                                                 variant,
                                                 block,
                                                 "compiled",
                                                 false}));
        CpuState cpu;
        cpu.write_sr(sr_md_mask);
        cpu.pr = 0xDEADBEEFu;
        const BlockAddress source{0x8C000100u, 0x0C000100u};

        const auto call = dispatch_indirect(
            cpu,
            table,
            {IndirectDispatchKind::Call, 0x8C000102u, 0xAC001000u, 0x8C000106u, source, variant});
        require(call.block && table.resolve(call.block).has_value() && call.alias_lookup &&
                    call.physical_target == 0x0C001000u,
                "P2-Alias erreichte den physischen P1-Block nicht.");
        require(cpu.pc == 0x8C001000u && call.diagnostic_target == 0xAC001000u &&
                    call.resulting_pc == 0x8C001000u && cpu.pr == 0x8C000106u,
                "Alias-Call normalisiert den nativen Ausfuehrungs-PC oder bewahrt PR nicht.");

        const auto jump = dispatch_indirect(
            cpu,
            table,
            {IndirectDispatchKind::TailJump, 0x8C000200u, 0x8C001000u, 0u, source, variant});
        require(!jump.alias_lookup && cpu.pc == 0x8C001000u && cpu.pr == 0x8C000106u,
                "Tail-Jump veraendert PR oder verfehlt den exakten Lookup.");

        cpu.pr = 0xAC001000u;
        const auto returned = dispatch_indirect(
            cpu,
            table,
            {IndirectDispatchKind::Return, 0x8C000300u, 0x11111111u, 0u, source, variant});
        require(
            returned.alias_lookup && returned.diagnostic_target == cpu.pr && cpu.pc == 0x8C001000u,
            "Alias-Return bewahrt das Diagnoseziel oder normalisiert den Ausfuehrungs-PC nicht.");

        bool failed = false;
        try {
            static_cast<void>(dispatch_indirect(
                cpu,
                table,
                {IndirectDispatchKind::TailJump, 0x8C000400u, 0x8C999000u, 0u, source, variant}));
        } catch (const IndirectDispatchError& error) {
            const std::string text = error.what();
            failed = text.find("8c000400") != std::string::npos &&
                     text.find("8c999000") != std::string::npos &&
                     text.find("source=") != std::string::npos;
        }
        require(failed, "Unbekanntes Ziel wurde nicht mit Callsite, Ziel und Quelle abgelehnt.");

        IndirectDispatchMetrics metrics;
        cpu.pc = 0x11111110u;
        cpu.pr = 0x22222220u;
        const auto runtime_hit = dispatch_indirect(cpu,
                                                   table,
                                                   {IndirectDispatchKind::TailJump,
                                                    0x8C000500u,
                                                    0xAC001000u,
                                                    0u,
                                                    source,
                                                    variant,
                                                    DispatchResolutionOrigin::RuntimeOnly,
                                                    nullptr,
                                                    RuntimeDispatchClass::RuntimeOnly,
                                                    &metrics});
        require(runtime_hit.alias_lookup && metrics.hits() == 1u &&
                    metrics.runtime_only_hits() == 1u && metrics.misses() == 0u,
                "Gueltiger physischer Runtime-only-Alias wird nicht getrennt gezaehlt.");

        constexpr std::uint32_t terminator = 0x8C0005F0u;
        constexpr std::uint32_t dynamic_target = 0xAC001000u;
        static_assert(terminator != dynamic_target);
        BlockExit dynamic_exit;
        dynamic_exit.kind = BlockEndKind::DynamicBranch;
        dynamic_exit.source = {terminator, canonical_physical_address(terminator)};
        dynamic_exit.target =
            BlockAddress{dynamic_target, canonical_physical_address(dynamic_target)};
        const auto continuation = make_indirect_dispatch_continuation(
            dynamic_exit, DynamicDispatchSiteClass::RuntimeOnly);
        IndirectDispatchMetrics continuation_metrics;
        DispatchDiagnosticRecorder continuation_diagnostics;
        cpu.pc = dynamic_target;
        const auto continued = dispatch_indirect(
            cpu,
            table,
            {continuation.kind,
             continuation.callsite,
             cpu.pc,
             cpu.pr,
             continuation.source,
             variant,
             continuation.resolution_origin,
             continuation.record_diagnostics ? &continuation_diagnostics : nullptr,
             continuation.dispatch_class,
             &continuation_metrics});
        const auto continuation_profile =
            continuation_metrics.runtime_only_sites().find(terminator);
        require(continued.diagnostic_target == dynamic_target &&
                    continuation.callsite == terminator &&
                    continuation.source == dynamic_exit.source &&
                    continuation.kind == IndirectDispatchKind::TailJump &&
                    continuation.dispatch_class == RuntimeDispatchClass::RuntimeOnly &&
                    continuation.resolution_origin == DispatchResolutionOrigin::RuntimeOnly &&
                    continuation.record_diagnostics && continuation_metrics.hits() == 1u &&
                    continuation_metrics.runtime_only_hits() == 1u &&
                    continuation_metrics.runtime_only_site_count() == 1u &&
                    continuation_profile != continuation_metrics.runtime_only_sites().end() &&
                    continuation_profile->second.calls == 1u &&
                    continuation_profile->second.targets.size() == 1u &&
                    continuation_profile->second.targets.front() == dynamic_target &&
                    !continuation_metrics.runtime_only_sites().contains(dynamic_target) &&
                    continuation_diagnostics.events().size() == 1u &&
                    continuation_diagnostics.total_occurrences() == 1u &&
                    continuation_diagnostics.events().front().alias_origin ==
                        DispatchAliasOrigin::CanonicalPhysical,
                "Runtime-only-Fortsetzung verliert Terminator, Ziel oder Site-Profil.");

        dynamic_exit.kind = BlockEndKind::Call;
        const auto guarded_call = make_indirect_dispatch_continuation(
            dynamic_exit, DynamicDispatchSiteClass::Guarded);
        const auto static_call = make_indirect_dispatch_continuation(
            dynamic_exit, DynamicDispatchSiteClass::NotDynamic);
        const auto unresolved_call = make_indirect_dispatch_continuation(
            dynamic_exit, DynamicDispatchSiteClass::Unresolved);
        require(guarded_call.kind == IndirectDispatchKind::Call &&
                    guarded_call.dispatch_class == RuntimeDispatchClass::GuardedFallback &&
                    guarded_call.resolution_origin == DispatchResolutionOrigin::TableLookup &&
                    guarded_call.record_diagnostics &&
                    static_call.kind == IndirectDispatchKind::Call &&
                    static_call.resolution_origin == DispatchResolutionOrigin::StaticProof &&
                    !static_call.record_diagnostics &&
                    unresolved_call.kind == IndirectDispatchKind::Call &&
                    unresolved_call.dispatch_class == RuntimeDispatchClass::GuardedFallback &&
                    unresolved_call.resolution_origin == DispatchResolutionOrigin::Fallback &&
                    unresolved_call.record_diagnostics,
                "Call-Fortsetzung verliert Aufrufart oder Guarded-/Static-Vertrag.");

        static_cast<void>(table.register_static({0x00000100u,
                                                 0x00000100u,
                                                 2u,
                                                 BlockEndKind::Return,
                                                 variant,
                                                 block,
                                                 "address-error-handler",
                                                 false}));
        IndirectDispatchMetrics address_error_metrics;
        cpu.vbr = 0u;
        cpu.pc = 0x11111110u;
        cpu.pr = 0x22222220u;
        const auto generation_before_odd_target = cpu.exception_generation;
        const auto attempts_before_odd_target = cpu.attempted_guest_instructions;
        const auto cycles_before_odd_target = elapsed_guest_cycles(cpu);
        const auto odd_target = dispatch_indirect(
            cpu,
            table,
            {IndirectDispatchKind::Call,
             0x8C0005F0u,
             0x8C001001u,
             0x33333330u,
             source,
             variant,
             DispatchResolutionOrigin::RuntimeOnly,
             nullptr,
             RuntimeDispatchClass::RuntimeOnly,
             &address_error_metrics});
        require(odd_target.diagnostic_target == 0x00000100u &&
                    cpu.last_exception_cause == ExceptionCause::AddressErrorRead &&
                    cpu.tea == 0x8C001001u &&
                    cpu.spc == 0x8C001001u &&
                    cpu.last_exception_instruction_pc == 0x8C001001u &&
                    cpu.last_exception_owner_pc == 0x8C001001u &&
                    !cpu.exception_in_delay_slot &&
                    cpu.exception_generation == generation_before_odd_target + 1u &&
                    cpu.attempted_guest_instructions == attempts_before_odd_target + 1u &&
                    elapsed_guest_cycles(cpu) == cycles_before_odd_target + 1u &&
                    cpu.pr == 0x33333330u,
                "Ungerades indirektes Ziel meldet den Ziel-Fetch nicht mit exaktem "
                 "Fault-/Owner-PC, Versuch/Zeit oder verliert die abgeschlossene "
                 "Call-/Delay-Slot-PR.");

        static_cast<void>(table.register_static({0xA0000000u,
                                                 0x00000000u,
                                                 2u,
                                                 BlockEndKind::Return,
                                                 variant,
                                                 block,
                                                 "manual-reset-vector",
                                                 false}));
        CpuState blocked_handler_fetch;
        blocked_handler_fetch.write_sr(sr_md_mask);
        blocked_handler_fetch.vbr = 1u;
        blocked_handler_fetch.pc = 0x44444440u;
        blocked_handler_fetch.pr = 0x55555550u;
        const auto blocked_generation_before = blocked_handler_fetch.exception_generation;
        const auto blocked_attempts_before =
            blocked_handler_fetch.attempted_guest_instructions;
        const auto blocked_retired_before =
            blocked_handler_fetch.retired_guest_instructions;
        const auto blocked_cycles_before = elapsed_guest_cycles(blocked_handler_fetch);
        const auto reset_dispatch = dispatch_indirect(
            blocked_handler_fetch,
            table,
            {IndirectDispatchKind::Call,
             0x8C0005F2u,
             0x8C001001u,
             0x66666660u,
             source,
             variant,
             DispatchResolutionOrigin::RuntimeOnly,
             nullptr,
             RuntimeDispatchClass::RuntimeOnly,
             nullptr});
        require(reset_dispatch.diagnostic_target == 0xA0000000u &&
                    reset_dispatch.physical_target == 0u &&
                    blocked_handler_fetch.pc == 0xA0000000u &&
                    blocked_handler_fetch.pr == 0u &&
                    blocked_handler_fetch.last_exception_cause ==
                        ExceptionCause::AddressErrorRead &&
                    blocked_handler_fetch.last_exception_instruction_pc == 0x00000101u &&
                    blocked_handler_fetch.last_exception_owner_pc == 0x00000101u &&
                    blocked_handler_fetch.tea == 0x00000101u &&
                    blocked_handler_fetch.exception_generation ==
                        blocked_generation_before + 2u &&
                    blocked_handler_fetch.attempted_guest_instructions ==
                        blocked_attempts_before + 2u &&
                    blocked_handler_fetch.retired_guest_instructions ==
                        blocked_retired_before &&
                    elapsed_guest_cycles(blocked_handler_fetch) ==
                        blocked_cycles_before + 2u,
                "Faultender Exception-Handler-Fetch entkommt roh, ueberspringt den "
                "Manual-Reset-Pfad oder wird faelschlich retired.");

        const auto expect_runtime_miss = [&](const std::uint32_t target,
                                              const DispatchDiagnosticError expected) {
            const auto pc_before = cpu.pc;
            const auto pr_before = cpu.pr;
            try {
                static_cast<void>(dispatch_indirect(cpu,
                                                    table,
                                                    {IndirectDispatchKind::Call,
                                                     0x8C000600u,
                                                     target,
                                                     0x33333330u,
                                                     source,
                                                     variant,
                                                     DispatchResolutionOrigin::RuntimeOnly,
                                                     nullptr,
                                                     RuntimeDispatchClass::RuntimeOnly,
                                                     &metrics}));
            } catch (const IndirectDispatchError& error) {
                require(
                    cpu.pc == pc_before && cpu.pr == pr_before &&
                        metrics.first_error().has_value() &&
                        std::string(error.what()).find(dispatch_diagnostic_error_name(expected)) !=
                            std::string::npos &&
                        error.metrics_json().find("\"runtime_only_misses\":") != std::string::npos,
                    "Runtime-only-Miss mutiert CPU oder verliert Fehlermetriken.");
                return;
            }
            throw std::runtime_error("Ungueltiges Runtime-only-Ziel wurde akzeptiert.");
        };
        expect_runtime_miss(0x8C001002u, DispatchDiagnosticError::UnknownTarget);
        expect_runtime_miss(0x12345000u, DispatchDiagnosticError::UnknownTarget);
        require(metrics.misses() == 2u && metrics.runtime_only_misses() == 2u &&
                     metrics.fallbacks() == 0u && metrics.runtime_only_fallbacks() == 0u &&
                     metrics.first_error()->target == 0x8C001002u &&
                    metrics.serialize_json().find("\"class\":\"runtime-only\"") !=
                        std::string::npos,
                "Runtime-only-Zaehler oder erster Fehler sind nicht stabil.");

        RuntimeBlockTable mmu_table;
        static_cast<void>(mmu_table.register_static({0x00001000u,
                                                     0x0C001000u,
                                                     4u,
                                                     BlockEndKind::Return,
                                                     {},
                                                     block,
                                                     "mmu-static",
                                                     false}));
        CpuState mmu_cpu;
        mmu_cpu.write_sr(sr_md_mask);
        mmu_cpu.address_space = std::make_shared<RuntimeAddressSpace>();
        mmu_cpu.address_space->set_mode(AddressTranslationMode::Mmu);
        mmu_cpu.address_space->write_mmucr(1u);
        mmu_cpu.address_space->ldtlb(
            {0x00001000u, 0x0C001000u, 4096u, 0u, 0u, true, true, true, true, true, true, false});
        const auto mmu_dispatch = dispatch_indirect(
            mmu_cpu,
            mmu_table,
            {IndirectDispatchKind::TailJump, 0x00000000u, 0x00001000u, 0u, {}, {}});
        const auto mmu_block = mmu_table.resolve(mmu_dispatch.block);
        require(mmu_block && mmu_block->get().variant.mmu_generation != 0u &&
                    mmu_dispatch.physical_target == 0x0C001000u,
                "Statischer AOT-Block wird nicht an die aktive MMU-Variante gebunden.");

        mmu_cpu.address_space->ldtlb(
            {0x00001000u, 0x0D001000u, 4096u, 0u, 0u, true, true, true, true, true, true, false});
        bool remap_rejected = false;
        try {
            static_cast<void>(dispatch_indirect(
                mmu_cpu,
                mmu_table,
                {IndirectDispatchKind::TailJump, 0x00000002u, 0x00001000u, 0u, {}, {}}));
        } catch (const IndirectDispatchError& error) {
            remap_rejected =
                std::string(error.what()).find("unknown-target") != std::string::npos;
        }
        require(remap_rejected,
                "Physisch remappter Code verwendet einen stale statischen AOT-Block wieder.");

        RuntimeBlockTable invalid_table;
        static_cast<void>(invalid_table.register_static({0x8C002000u,
                                                         0x0C002000u,
                                                         1u,
                                                         BlockEndKind::Return,
                                                         variant,
                                                         block,
                                                         "too-small",
                                                         false}));
        const auto pc_before_invalid = cpu.pc;
        try {
            static_cast<void>(dispatch_indirect(cpu,
                                                invalid_table,
                                                {IndirectDispatchKind::TailJump,
                                                 0x8C000700u,
                                                 0x8C002000u,
                                                 0u,
                                                 source,
                                                 variant,
                                                 DispatchResolutionOrigin::RuntimeOnly,
                                                 nullptr,
                                                 RuntimeDispatchClass::RuntimeOnly,
                                                 &metrics}));
            throw std::runtime_error("Zu kleiner Runtimeblock wurde akzeptiert.");
        } catch (const IndirectDispatchError& error) {
            require(cpu.pc == pc_before_invalid &&
                        std::string(error.what()).find("invalid-boundary") != std::string::npos,
                    "Ungueltige Blockgrenze wird nicht vor PC-Mutation abgewiesen.");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

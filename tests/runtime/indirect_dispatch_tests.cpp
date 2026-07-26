#include "katana/runtime/indirect_dispatch.hpp"
#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/executable_modules.hpp"
#include "katana/runtime/native_aot_template.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace katana::runtime;

namespace {
BlockExit block(CpuState&, BlockExecutionContext&) {
    return {};
}
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
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
    require(!cached.materialized && cached.block == first.block &&
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
                diagnostics.total_occurrences() == detail_occurrences_before &&
                diagnostics.dropped_unique_events() == dropped_details_before &&
                retained_details_unchanged,
            "Eine Million reine Runtime-only-Hits aktualisieren nicht nur Metriken oder "
            "betreten weiterhin den linearen Detail-Key-Vergleich.");

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
}
} // namespace

int main() {
    try {
        materializer_lifecycle_regression();
        runtime_aot_alias_lifetime_regression();
        missing_aot_dispatch_regression();
        runtime_only_hit_hotloop_regression();
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

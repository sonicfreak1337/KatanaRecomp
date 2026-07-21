#include "katana/runtime/block_dispatch.hpp"
#include "katana/runtime/executable_modules.hpp"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace katana::runtime;

BlockExit block(CpuState&, BlockExecutionContext&) {
    return {};
}

void require(const bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

MaterializedBlockCandidate valid_candidate(const std::uint32_t target,
                                           const BlockVariantKey& variant) {
    MaterializedBlockCandidate candidate;
    candidate.block = {target,
                       target,
                       2u,
                       BlockEndKind::Return,
                       variant,
                       block,
                       "bounded-synthetic-codegen",
                       true};
    candidate.decode_candidate_validated = true;
    candidate.bounded_analysis_complete = true;
    candidate.ir_verified = true;
    candidate.code_generated = true;
    candidate.guest_cycles = 1u;
    candidate.instructions = 1u;
    candidate.recursive_seeds = 1u;
    candidate.analysis_time_ms = 1u;
    candidate.peak_memory_bytes = 1024u;
    return candidate;
}

struct Fixture {
    CpuState cpu;
    ExecutableModuleCatalog modules;
    RuntimeBlockTable blocks;
    ExecutableCodeTracker tracker;
    BlockVariantKey variant{1u, 2u, 3u, 4u, 5u};
    ExecutableModule module;

    explicit Fixture(const std::uint32_t address = 0x100u, const bool publish_module = true) {
        module.id = "fixture-module";
        module.source_identity = "free-synthetic-fixture-v1";
        module.guest_start = address;
        module.bytes = {0x09u, 0x00u, 0x0Bu, 0x00u, 0x09u, 0x00u};
        if (cpu.memory.contains(address, module.bytes.size()))
            cpu.memory.write_bytes(address, module.bytes, CodeWriteSource::Copy);
        blocks.bind_code_tracker(&tracker);
        if (publish_module) modules.publish(module);
    }

    IndirectDispatchRequest
    request(const std::uint32_t target,
            DemandBlockMaterializer* materializer,
            const DispatchResolutionOrigin origin = DispatchResolutionOrigin::RuntimeOnly) {
        return {IndirectDispatchKind::TailJump,
                0x80u,
                target,
                0u,
                {0x80u, 0x80u},
                variant,
                origin,
                nullptr,
                RuntimeDispatchClass::RuntimeOnly,
                nullptr,
                materializer};
    }
};

MaterializationFailure dispatch_failure(Fixture& fixture,
                                        const std::uint32_t target,
                                        BlockMaterializationPolicy policy,
                                        BlockMaterializeCallback callback) {
    DemandBlockMaterializer materializer(
        fixture.modules, fixture.blocks, &fixture.tracker, policy, std::move(callback));
    const auto pc = fixture.cpu.pc;
    const auto pr = fixture.cpu.pr;
    try {
        static_cast<void>(
            dispatch_indirect(fixture.cpu, fixture.blocks, fixture.request(target, &materializer)));
    } catch (const IndirectDispatchError&) {
        require(fixture.cpu.pc == pc && fixture.cpu.pr == pr,
                "Abgelehntes Ziel hatte bereits Gastwirkung.");
        require(fixture.blocks.dispatch_status(target, fixture.variant).state ==
                    RuntimeBlockDispatchState::Rejected,
                "Abgelehntes Ziel besitzt keinen expliziten Rejected-Status.");
        return materializer.last_failure();
    }
    throw std::runtime_error("Ungueltiges Ziel wurde dispatcht.");
}

void target_validation_regressions() {
    {
        Fixture fixture;
        bool callback_called = false;
        const auto failure =
            dispatch_failure(fixture, 0x101u, {true, 4u, 64u}, [&](auto, auto, const auto&) {
                callback_called = true;
                return valid_candidate(0x101u, fixture.variant);
            });
        require(failure == MaterializationFailure::Misaligned && !callback_called,
                "Ungueltige Zielausrichtung erreichte Analyse oder Codegen.");
    }
    {
        Fixture fixture(0x200000u);
        const auto failure =
            dispatch_failure(fixture, 0x200000u, {true, 4u, 64u}, [&](auto, auto, const auto&) {
                return valid_candidate(0x200000u, fixture.variant);
            });
        require(failure == MaterializationFailure::Uncommitted,
                "Ziel ausserhalb committed Gastbytes wurde nicht abgelehnt.");
    }
    {
        Fixture fixture;
        fixture.modules.unload(fixture.module.id, fixture.blocks, fixture.tracker);
        auto data = fixture.module;
        data.id = "data-module";
        data.range_roles = {{0u, 4u, ExecutableStorageRole::ProvenData}};
        fixture.modules.publish(data);
        bool callback_called = false;
        const auto failure =
            dispatch_failure(fixture, 0x100u, {true, 4u, 64u}, [&](auto, auto, const auto&) {
                callback_called = true;
                return valid_candidate(0x100u, fixture.variant);
            });
        require(failure == MaterializationFailure::ProvenNonCode && !callback_called,
                "Bewiesener Datenbereich erreichte die Materialisierung.");
    }
    {
        Fixture fixture;
        fixture.modules.unload(fixture.module.id, fixture.blocks, fixture.tracker);
        auto denied = fixture.module;
        denied.id = "permission-denied";
        denied.executable_permission = false;
        fixture.modules.publish(denied);
        const auto failure =
            dispatch_failure(fixture, 0x100u, {true, 4u, 64u}, [&](auto, auto, const auto&) {
                return valid_candidate(0x100u, fixture.variant);
            });
        require(failure == MaterializationFailure::PermissionDenied,
                "Speicherberechtigung wurde als Ausfuehrungsabdeckung missverstanden.");
    }
    {
        Fixture fixture(0x100u, false);
        const auto failure =
            dispatch_failure(fixture, 0x100u, {true, 4u, 64u}, [&](auto, auto, const auto&) {
                return valid_candidate(0x100u, fixture.variant);
            });
        require(failure == MaterializationFailure::UnknownSource,
                "Unbekannte Speicherbytes wurden pauschal runtime-materialisierbar.");
    }
}

void revalidation_regressions() {
    {
        Fixture fixture;
        const auto failure =
            dispatch_failure(fixture, 0x100u, {true, 4u, 64u}, [&](auto, auto, const auto&) {
                fixture.cpu.memory.write_u8(0x100u, 0x0Bu);
                return valid_candidate(0x100u, fixture.variant);
            });
        require(failure == MaterializationFailure::ByteIdentityMismatch,
                "Byteaenderung zwischen Snapshot und Registrierung blieb unsichtbar.");
    }
    {
        Fixture fixture;
        const auto failure =
            dispatch_failure(fixture, 0x100u, {true, 4u, 64u}, [&](auto, auto, const auto&) {
                fixture.modules.unload(fixture.module.id, fixture.blocks, fixture.tracker);
                return valid_candidate(0x100u, fixture.variant);
            });
        require(failure == MaterializationFailure::ModuleUnloaded,
                "Modul-Unload zwischen Analyse und Registrierung blieb unsichtbar.");
    }
    {
        Fixture fixture;
        const auto failure =
            dispatch_failure(fixture, 0x100u, {true, 4u, 64u}, [&](auto, auto, const auto&) {
                auto replacement = fixture.module;
                fixture.modules.replace(replacement, fixture.blocks, fixture.tracker);
                return valid_candidate(0x100u, fixture.variant);
            });
        require(failure == MaterializationFailure::GenerationMismatch,
                "Modulgeneration wurde nach Analyse nicht revalidiert.");
    }
    {
        Fixture fixture;
        const auto failure =
            dispatch_failure(fixture, 0x100u, {true, 4u, 64u}, [&](auto, auto, const auto&) {
                fixture.modules.update_relocations(
                    fixture.module.id, {{0u, 1u, 0}}, fixture.blocks, fixture.tracker);
                return valid_candidate(0x100u, fixture.variant);
            });
        require(failure == MaterializationFailure::RelocationMismatch,
                "Relocationgeneration wurde nach Analyse nicht revalidiert.");
    }
}

void budget_and_dispatch_regressions() {
    {
        Fixture fixture;
        DemandBlockMaterializer materializer(
            fixture.modules,
            fixture.blocks,
            &fixture.tracker,
            {true, 4u, 64u},
            [&](const auto target, auto, const auto& variant) {
                auto candidate = valid_candidate(target, variant);
                candidate.interpreter_backed = true;
                candidate.bounded_analysis_complete = false;
                candidate.ir_verified = false;
                candidate.code_generated = false;
                candidate.block.provenance = "bounded-synthetic-interpreter";
                return candidate;
            });
        const auto result = dispatch_indirect(
            fixture.cpu, fixture.blocks, fixture.request(0x100u, &materializer));
        require(result.block && materializer.metrics().materializations == 1u &&
                    materializer.metrics().interpreter_materializations == 1u,
                "Ehrlich markierter Interpreterblock wird abgelehnt oder als Codegen gezaehlt.");
    }
    {
        Fixture fixture;
        BlockMaterializationPolicy policy{true, 4u, 64u};
        policy.max_instructions = 1u;
        const auto failure =
            dispatch_failure(fixture, 0x100u, policy, [&](auto, auto, const auto&) {
                auto candidate = valid_candidate(0x100u, fixture.variant);
                candidate.instructions = 2u;
                return candidate;
            });
        require(failure == MaterializationFailure::BudgetExhausted,
                "Analysebudget wurde als stiller Miss behandelt.");
    }
    {
        Fixture fixture;
        DemandBlockMaterializer materializer(fixture.modules,
                                             fixture.blocks,
                                             &fixture.tracker,
                                             {true, 4u, 64u},
                                             [&](const auto target, auto, const auto& variant) {
                                                 return valid_candidate(target, variant);
                                             });
        const auto first =
            dispatch_indirect(fixture.cpu, fixture.blocks, fixture.request(0x100u, &materializer));
        require(fixture.blocks.dispatch_status(0x100u, fixture.variant).state ==
                        RuntimeBlockDispatchState::RuntimeMaterialized &&
                    first.block,
                "Demand-Materialisierung erzeugte keinen expliziten Runtime-Status.");
        fixture.cpu.memory.write_u8(0x100u, 0x0Bu);
        const auto pc = fixture.cpu.pc;
        try {
            static_cast<void>(dispatch_indirect(
                fixture.cpu,
                fixture.blocks,
                fixture.request(0x100u, &materializer, DispatchResolutionOrigin::InlineCache)));
            throw std::runtime_error("Inline-Cache dispatchte stale Bytes.");
        } catch (const IndirectDispatchError&) {
            require(fixture.cpu.pc == pc &&
                        materializer.last_failure() ==
                            MaterializationFailure::ByteIdentityMismatch &&
                        materializer.metrics().dispatch_validation_failures == 1u,
                    "Inline-Cache umging die Dispatchrevalidierung.");
        }
    }
    {
        RuntimeBlockTable table;
        const BlockVariantKey variant{};
        const auto handle = table.register_static(
            {0x100u, 0x100u, 2u, BlockEndKind::Return, variant, block, "static-proof", false});
        CpuState cpu;
        const auto result = dispatch_indirect(
            cpu,
            table,
            {IndirectDispatchKind::TailJump, 0x80u, 0x100u, 0u, {0x80u, 0x80u}, variant});
        require(result.block == handle && table.dispatch_status(0x100u, variant).state ==
                                              RuntimeBlockDispatchState::StaticCompiled,
                "Statisch kompilierter Block wurde erneut materialisiert.");
    }
}

std::vector<BlockMaterializationEvent> deterministic_events() {
    Fixture fixture;
    DemandBlockMaterializer materializer(fixture.modules,
                                         fixture.blocks,
                                         &fixture.tracker,
                                         {true, 4u, 64u},
                                         [&](const auto target, auto, const auto& variant) {
                                             return valid_candidate(target, variant);
                                         });
    try {
        static_cast<void>(
            dispatch_indirect(fixture.cpu, fixture.blocks, fixture.request(0x101u, &materializer)));
    } catch (const IndirectDispatchError&) {
    }
    static_cast<void>(
        dispatch_indirect(fixture.cpu, fixture.blocks, fixture.request(0x100u, &materializer)));
    const auto public_json =
        format_block_materialization_metrics_json(materializer.metrics(), materializer.events());
    require(public_json.find("\"target\"") == std::string::npos &&
                public_json.find("\"materialization_attempts\":2") != std::string::npos,
            "Oeffentliche Materialisierungsmetriken enthalten Ziele oder falsche Zaehler.");
    return materializer.events();
}

} // namespace

int main() {
    try {
        target_validation_regressions();
        revalidation_regressions();
        budget_and_dispatch_regressions();
        require(deterministic_events() == deterministic_events(),
                "Identische Eingabe erzeugt keine deterministische Ereignisfolge.");
        std::cout << "KR-4704 sichere Ausfuehrungsabdeckung erfolgreich.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

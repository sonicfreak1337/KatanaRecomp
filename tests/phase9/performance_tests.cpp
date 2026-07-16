#include "katana/codegen/backend.hpp"
#include "katana/phase9/performance.hpp"
#include "katana/runtime/abi.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

katana::runtime::BlockExit block(katana::runtime::CpuState&,
                                 katana::runtime::BlockExecutionContext&) {
    return {};
}

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function> bool throws_invalid(Function&& function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

} // namespace

int main() {
    try {
        using namespace katana::phase9;
        using namespace katana::runtime;
        const ExecutionProfileIdentity identity{
            std::string(64u, 'a'), abi_version, katana::codegen::backend_interface_abi_version};
        ExecutionProfiler profiler(ProfilingMode::Exact, identity);
        profiler.record_block(0x8C002000u);
        profiler.record_block(0x8C001000u);
        profiler.record_block(0x8C002000u);
        profiler.record_edge(0x8C001000u, 0x8C002000u);
        profiler.record_edge(0x8C001000u, 0x8C002000u);
        profiler.record_edge(0x8C002000u, 0x8C003000u);
        profiler.record_fallback("unknown-code");
        profiler.record_invalidation(0x0C001000u);
        require_profile_identity(profiler.snapshot(), identity);
        const auto blocks = hot_blocks(profiler.snapshot());
        const auto edges = hot_edges(profiler.snapshot());
        require(blocks.size() == 2u && blocks[0].first == 0x8C002000u && blocks[0].second == 2u &&
                    edges.size() == 2u && edges[0].second == 2u,
                "Hot-Block- oder Hot-Edge-Sortierung ist nicht deterministisch.");
        const auto profile_json = format_execution_profile_json(profiler.snapshot());
        require(profile_json.find("katana-execution-profile") != std::string::npos &&
                    profile_json.find("unknown-code") != std::string::npos &&
                    profile_json.find("C:\\") == std::string::npos,
                "Profilformat verliert Schema, Zaehler oder Portabilitaet.");
        auto wrong_identity = identity;
        wrong_identity.input_sha256[0] = 'b';
        require(
            throws_invalid([&] { require_profile_identity(profiler.snapshot(), wrong_identity); }),
            "Profil mit falscher Eingabeidentitaet wurde akzeptiert.");

        ExecutionProfiler sampled(ProfilingMode::Sampled, identity, 2u);
        sampled.record_block(1u);
        sampled.record_block(1u);
        sampled.record_block(1u);
        require(sampled.snapshot().blocks.at(1u) == 2u,
                "Samplingmodus waehlt nicht die stabile Ereignisfolge.");
        ExecutionProfiler disabled(ProfilingMode::Disabled, identity);
        disabled.record_block(1u);
        require(disabled.snapshot().blocks.empty(),
                "Deaktiviertes Profiling beeinflusst oder erfasst Gastzustand.");

        Memory memory(0u, MemoryAlignmentPolicy::Strict);
        auto linear = std::make_shared<LinearMemoryDevice>(64u);
        memory.map_region("phase9-linear-ram", 0x1000u, linear);
        memory.write_u32(0x1000u, 0x11223344u);
        ExecutableCodeTracker tracker;
        static_cast<void>(tracker.register_block(
            {"fastpath-code", 0x1000u, 16u, "synthetic", {}, ExecutableBlockOrigin::RuntimeWrite}));
        GuardedMemoryFastpath fastpath(memory, linear, 0x1000u, &tracker, &profiler);
        const FastMemoryGuard valid{true, true, true, true, true, true, true, true, 3u, 3u, 0u, 0u};
        require(fastpath.read_u32(0x1000u, valid) == 0x11223344u,
                "Bewiesener linearer RAM-Lesefastpath liefert falschen Wert.");
        fastpath.write_u32(0x1004u, 0xAABBCCDDu, valid);
        require(memory.read_u32(0x1004u) == 0xAABBCCDDu && fastpath.hits() == 2u &&
                    tracker.invalidation_count() == 1u,
                "RAM-Schreibfastpath umgeht Speicherzustand oder Codeinvalidierung.");

        std::uint64_t watched = 0u;
        const auto watchpoint = memory.add_watchpoint(
            0x1000u, 4u, MemoryWatchpointAccess::Read, [&](const auto&) { ++watched; });
        auto guarded = valid;
        guarded.watchpoints_absent = false;
        require(fastpath.read_u32(0x1000u, guarded) == 0x11223344u && watched == 1u &&
                    fastpath.misses() == 1u,
                "Watchpoint-Waechter faellt nicht in den generischen Speicherpfad zurueck.");
        static_cast<void>(memory.remove_watchpoint(watchpoint));
        guarded = valid;
        guarded.mmu_disabled = false;
        static_cast<void>(fastpath.read_u32(0x1000u, guarded));
        guarded = valid;
        guarded.expected_code_generation = 1u;
        static_cast<void>(fastpath.read_u32(0x1000u, guarded));
        require(fastpath.misses() == 3u,
                "MMU- oder Codegenerationswaechter wird vom Fastpath umgangen.");

        RuntimeBlockTable table;
        const BlockVariantKey variant{1u, 0u, 0u, 0u, 0u};
        table.register_static({0x8C001000u,
                               0x0C001000u,
                               4u,
                               BlockEndKind::Return,
                               variant,
                               block,
                               "phase9-target",
                               false});
        CpuState cpu;
        const IndirectDispatchRequest request{IndirectDispatchKind::Call,
                                              0x8C000100u,
                                              0x8C001000u,
                                              0x8C000104u,
                                              {0x8C000100u, 0x0C000100u},
                                              variant};
        MonomorphicDispatchCache cache;
        const auto generic = cache.dispatch(cpu, table, request, 7u, &profiler);
        const auto cached = cache.dispatch(cpu, table, request, 7u, &profiler);
        require(generic.block == cached.block && cache.misses() == 1u && cache.hits() == 1u &&
                    cached.diagnostic == "inline-cache" && cpu.pr == 0x8C000104u,
                "Monomorphe Callsite behaelt Ziel- oder Callsemantik nicht.");
        static_cast<void>(cache.dispatch(cpu, table, request, 8u, &profiler));
        require(cache.misses() == 2u,
                "Blockgenerationswechsel verwendet einen stale Inline-Cache.");
        cache.invalidate(cache.entry()->block_identity);
        require(!cache.entry().has_value(),
                "Explizite Blockinvalidierung leert den Inline-Cache nicht.");

        require(decide_inline(16u, 8u, false, 32u).inline_call &&
                    !decide_inline(2u, 8u, false, 32u).inline_call &&
                    !decide_inline(16u, 40u, false, 64u).inline_call &&
                    !decide_inline(16u, 8u, true, 32u).inline_call,
                "Inliningstrategie respektiert Hitze, Groesse, Rekursion oder Codebudget nicht.");

        std::cout << "KR-3901 bis KR-3907 Performancepfade erfolgreich.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

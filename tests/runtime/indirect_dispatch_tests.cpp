#include "katana/runtime/indirect_dispatch.hpp"
#include "katana/runtime/block_guards.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace katana::runtime;

namespace {
BlockExit block(CpuState&, BlockExecutionContext&) {
    return {};
}
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
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
        expect_runtime_miss(0x8C001001u, DispatchDiagnosticError::Misaligned);
        expect_runtime_miss(0x8C001002u, DispatchDiagnosticError::UnknownTarget);
        expect_runtime_miss(0x12345000u, DispatchDiagnosticError::UnknownTarget);
        require(metrics.misses() == 3u && metrics.runtime_only_misses() == 3u &&
                    metrics.fallbacks() == 0u && metrics.runtime_only_fallbacks() == 0u &&
                    metrics.first_error()->target == 0x8C001001u &&
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

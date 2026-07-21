#include "katana/runtime/block_guards.hpp"

#include <iostream>
#include <stdexcept>

using namespace katana::runtime;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
} // namespace
namespace {
BlockExit guarded_block(CpuState&, BlockExecutionContext&) {
    return {};
}
} // namespace

int main() {
    try {
        RuntimeAddressSpace space;
        const auto fast = space.translate(0xAC001234u, TranslationAccess::Instruction);
        require(fast.no_mmu_fastpath && fast.physical_address == 0x0C001234u,
                "No-MMU-Fastpath ist nicht getrennt oder behauptet falsche Kanonisierung.");

        space.set_mode(AddressTranslationMode::Mmu);
        space.write_mmucr(1u);
        space.ldtlb({0x1000u, 0x0C002000u, 4096u, 0u, 0u, true, true, true, true, true, true, false});
        const auto first = space.guard_for(0x1234u, fpscr_pr_mask);
        require(first.physical_page == 0x0C002000u &&
                    !space.translate(0x1234u, TranslationAccess::Read).no_mmu_fastpath,
                "MMU-Instruktions- und Datenuebersetzung nutzt nicht den expliziten Vertrag.");
        space.ldtlb({0x1000u, 0x0D004000u, 4096u, 0u, 0u, true, true, false, true, true, true, false});
        const auto remapped = space.guard_for(0x1234u, fpscr_pr_mask);
        require(remapped.physical_page == 0x0D004000u &&
                    remapped.mmu_generation != first.mmu_generation,
                "TLB-Aenderung redispatcht dieselbe virtuelle Adresse nicht auf den neuen Block.");

        bool instruction_error = false;
        space.ldtlb({0x3000u, 0x0C003000u, 4096u, 0u, 1u, true, true, true, false, true, true, false});
        try {
            static_cast<void>(space.translate(0x3000u, TranslationAccess::Instruction));
        } catch (const TranslationError& error) {
            instruction_error = error.cause() == ExceptionCause::TlbProtectionRead &&
                                error.access() == TranslationAccess::Instruction &&
                                error.address() == 0x3000u;
        }
        require(instruction_error,
                "Ungueltige Instruktionsuebersetzung erzeugt keine strukturierte SH-4-Ausnahme.");

        RuntimeAddressSpace multiple_space;
        multiple_space.set_mode(AddressTranslationMode::Mmu);
        multiple_space.write_mmucr(1u);
        multiple_space.ldtlb({0x4000u, 0x0C004000u, 4096u, 0u, 2u, true, true, true, true, true, true, false});
        multiple_space.ldtlb({0x4000u, 0x0D004000u, 4096u, 0u, 3u, true, true, true, true, true, true, false});
        bool multiple_hit = false;
        try {
            static_cast<void>(multiple_space.translate(0x4000u, TranslationAccess::Instruction));
        } catch (const TranslationError& error) {
            multiple_hit = error.cause() == ExceptionCause::TlbMultipleHit;
        }
        require(multiple_hit, "Mehrere passende UTLB-Eintraege erzeugen keinen Multiple-Hit.");

        CpuState tlb_cpu;
        tlb_cpu.address_space = std::make_shared<RuntimeAddressSpace>();
        tlb_cpu.pteh = 0x5000u;
        tlb_cpu.ptel = 0x0C005100u;
        tlb_cpu.mmucr = (1u << 10u) | (3u << 18u);
        load_tlb(tlb_cpu);
        require(((tlb_cpu.mmucr >> 10u) & 0x3Fu) == 2u,
                "LDTLB erhoeht URC innerhalb der URB-Grenze nicht.");
        load_tlb(tlb_cpu);
        require(((tlb_cpu.mmucr >> 10u) & 0x3Fu) == 0u,
                "LDTLB laesst URC an URB nicht auf null umlaufen.");

        const auto single = space.guard_for(0x1234u, 0u);
        const auto double_precision = space.guard_for(0x1234u, fpscr_pr_mask | fpscr_fr_mask | 1u);
        require(!(single == double_precision),
                "FPSCR PR/FR/RM verwendet eine inkompatible Blockvariante wieder.");
        require(space.block_fits_translation_page(0x1FF0u, 16u) &&
                    !space.block_fits_translation_page(0x1FF0u, 18u),
                "Aktive MMU schneidet Bloecke nicht konservativ an Seitengrenzen.");
        const auto old_guard = space.guard_for(0x1234u, fpscr_pr_mask);
        RuntimeBlockTable table;
        static_cast<void>(table.register_static({0x1234u,
                                                 old_guard.physical_page + 0x234u,
                                                 2u,
                                                 BlockEndKind::Fallthrough,
                                                 block_variant_key(old_guard),
                                                 guarded_block,
                                                 "guarded-old",
                                                 false}));
        require(table.lookup(0x1234u, block_variant_key(old_guard)).has_value(),
                "Ausgangsvariante wurde nicht in der Laufzeittabelle registriert.");
        space.bump_watchpoints();
        const auto changed_guard = space.guard_for(0x1234u, fpscr_pr_mask);
        require(!(changed_guard == old_guard) &&
                    !table.lookup(0x1234u, block_variant_key(changed_guard)).has_value(),
                "Watchpointgeneration erreicht den Tabellenlookup nicht; alte Variante wurde "
                "wiederverwendet.");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

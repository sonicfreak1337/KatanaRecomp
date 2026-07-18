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
        space.ldtlb({0x1000u, 0x0C002000u, true, true, true, true});
        const auto first = space.guard_for(0x1234u, fpscr_pr_mask);
        require(first.physical_page == 0x0C002000u &&
                    !space.translate(0x1234u, TranslationAccess::Read).no_mmu_fastpath,
                "MMU-Instruktions- und Datenuebersetzung nutzt nicht den expliziten Vertrag.");
        space.ldtlb({0x1000u, 0x0D004000u, true, false, true, true});
        const auto remapped = space.guard_for(0x1234u, fpscr_pr_mask);
        require(remapped.physical_page == 0x0D004000u &&
                    remapped.mmu_generation != first.mmu_generation,
                "TLB-Aenderung redispatcht dieselbe virtuelle Adresse nicht auf den neuen Block.");

        bool instruction_error = false;
        space.ldtlb({0x3000u, 0x0C003000u, true, true, false, true});
        try {
            static_cast<void>(space.translate(0x3000u, TranslationAccess::Instruction));
        } catch (const TranslationError& error) {
            instruction_error = error.cause() == ExceptionCause::AddressErrorRead &&
                                error.access() == TranslationAccess::Instruction &&
                                error.address() == 0x3000u;
        }
        require(instruction_error,
                "Ungueltige Instruktionsuebersetzung erzeugt keine strukturierte SH-4-Ausnahme.");

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

#include "katana/runtime/block_guards.hpp"
#include "katana/runtime/cache_control.hpp"
#include "katana/runtime/mmu_control.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
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
        const auto on_chip_ram =
            space.translate(0x7E000FFCu, TranslationAccess::Write, true);
        require(on_chip_ram.no_mmu_fastpath &&
                    on_chip_ram.physical_address == 0x7E000FFCu &&
                    canonical_physical_address(0x7E000FFCu) == 0x7E000FFCu,
                "SH-4-On-Chip-RAM wird faelschlich als externer physischer Bus kanonisiert.");

        space.set_mode(AddressTranslationMode::Mmu);
        space.write_mmucr(1u);
        require(space.translate(0x7E000FFCu, TranslationAccess::Read, true).physical_address ==
                    0x7E000FFCu,
                "Aktive MMU uebersetzt den architektonisch ausgenommenen OCRAM-Bereich.");
        space.ldtlb({0x1000u, 0x0C002000u, 4096u, 0u, 0u, true, true, true, true, true, true, false});
        const auto first = space.guard_for(0x1234u, fpscr_pr_mask);
        require(first.physical_page == 0x0C002000u &&
                    !space.translate(0x1234u, TranslationAccess::Read).no_mmu_fastpath,
                "MMU-Instruktions- und Datenuebersetzung nutzt nicht den expliziten Vertrag.");
        space.ldtlb({0x1000u, 0x0D004000u, 4096u, 0u, 0u, true, true, false, true, true, true, false});
        const auto remapped = space.guard_for(0x1234u, fpscr_pr_mask);
        require(remapped.physical_page == 0x0D004000u &&
                    remapped.mmu_generation != first.mmu_generation &&
                    space.prove_instruction_mapping(
                        0x1234u, 0x0D004234u, 2u, true) &&
                    !space.prove_instruction_mapping(
                        0x1234u, 0x0C002234u, 2u, true),
                "TLB-Aenderung redispatcht dieselbe virtuelle Adresse nicht auf den neuen Block.");
        const auto address_space_snapshot = space.snapshot();
        require(address_space_snapshot.mode == AddressTranslationMode::Mmu &&
                    address_space_snapshot.mmucr == 1u &&
                    address_space_snapshot.asid == 0u &&
                    address_space_snapshot.mappings.size() == 1u &&
                    address_space_snapshot.mappings.front().physical_page == 0x0D004000u &&
                    address_space_snapshot.mmu_generation == remapped.mmu_generation,
                "Adressraum-Snapshot verliert MMU-Modus, Generation oder dynamisches Mapping.");

        RuntimeAddressSpace observed_space;
        observed_space.set_mode(AddressTranslationMode::Mmu);
        observed_space.write_mmucr(1u);
        observed_space.ldtlb(
            {0x1000u,
             0x0C001000u,
             4096u,
             0u,
             0u,
             true,
             true,
             true,
             true,
             true,
             true,
             false});
        const auto observed_before = observed_space.snapshot();
        static_cast<void>(observed_space.guard_for(0x1000u, 0u));
        require(observed_space.prove_instruction_mapping(
                    0x1000u, 0x0C001000u, 16u, true) &&
                    observed_space.block_fits_translation_page(0x1000u, 16u) &&
                    observed_space.snapshot() == observed_before,
                "Guard-/Herkunftsnachweis mutiert ITLB-Refill oder LRU-Metadaten.");

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

        RuntimeAddressSpace sv_space;
        sv_space.set_mode(AddressTranslationMode::Mmu);
        sv_space.write_mmucr(0x00000101u);
        sv_space.write_pteh(0x11u);
        sv_space.ldtlb({0x8000u,
                        0x0C008000u,
                        4096u,
                        0x22u,
                        4u,
                        true,
                        true,
                        true,
                        true,
                        true,
                        true,
                        false});
        require(sv_space.translate(0x8000u, TranslationAccess::Read, true).physical_address ==
                    0x0C008000u,
                "MMUCR.SV unterdrueckt den ASID-Vergleich im privilegierten Modus nicht.");
        bool user_asid_miss = false;
        try {
            static_cast<void>(sv_space.translate(0x8000u, TranslationAccess::Read, false));
        } catch (const TranslationError& error) {
            user_asid_miss = error.cause() == ExceptionCause::TlbMissRead;
        }
        require(user_asid_miss,
                "MMUCR.SV unterdrueckt den ASID-Vergleich faelschlich im User-Modus.");

        RuntimeAddressSpace sv_transition_space;
        sv_transition_space.set_mode(AddressTranslationMode::Mmu);
        sv_transition_space.write_mmucr(1u);
        sv_transition_space.write_pteh(0x11u);
        sv_transition_space.ldtlb({0x9000u,
                                   0x0C009000u,
                                   4096u,
                                   0x11u,
                                   5u,
                                   true,
                                   true,
                                   true,
                                   true,
                                   true,
                                   true,
                                   false});
        sv_transition_space.ldtlb({0x9000u,
                                   0x0D009000u,
                                   4096u,
                                   0x22u,
                                   6u,
                                   true,
                                   true,
                                   true,
                                   true,
                                   true,
                                   true,
                                   false});
        const auto asid_scoped_fill = sv_transition_space.translate(
            0x9000u, TranslationAccess::Instruction, true);
        require(asid_scoped_fill.itlb_refilled &&
                    asid_scoped_fill.physical_address == 0x0C009000u,
                "ASID-begrenzter Instruktionszugriff fuellt die ITLB nicht eindeutig.");
        sv_transition_space.write_mmucr(0x00000101u);
        const auto after_sv = sv_transition_space.snapshot();
        bool sv_multiple_hit = false;
        try {
            static_cast<void>(sv_transition_space.translate(
                0x9000u, TranslationAccess::Instruction, true));
        } catch (const TranslationError& error) {
            sv_multiple_hit = error.cause() == ExceptionCause::TlbMultipleHit;
        }
        require(std::none_of(after_sv.itlb_valid.begin(),
                             after_sv.itlb_valid.end(),
                             [](const bool valid) { return valid; }) &&
                    sv_multiple_hit,
                "MMUCR.SV behaelt einen ASID-abhaengigen ITLB-Hit und verdeckt einen "
                "UTLB-Multiple-Hit.");

        CpuState tlb_cpu;
        tlb_cpu.write_sr(sr_md_mask);
        tlb_cpu.address_space = std::make_shared<RuntimeAddressSpace>();
        tlb_cpu.pteh = 0x5000u;
        tlb_cpu.ptel = 0x0C005100u;
        tlb_cpu.mmucr = 1u | (1u << 10u) | (3u << 18u);
        tlb_cpu.address_space->set_mode(AddressTranslationMode::Mmu);
        tlb_cpu.address_space->write_mmucr(tlb_cpu.mmucr);
        tlb_cpu.address_space->write_pteh(tlb_cpu.pteh);
        load_tlb(tlb_cpu);
        require(((tlb_cpu.mmucr >> 10u) & 0x3Fu) == 1u,
                "LDTLB darf URC nicht erhoehen.");
        static_cast<void>(translate_guest_address(tlb_cpu,
                                                  0x5000u,
                                                  MemoryAccessOperation::Read,
                                                  MemoryAccessWidth::Byte));
        require(((tlb_cpu.mmucr >> 10u) & 0x3Fu) == 2u,
                "Ein UTLB-Zugriff erhoeht URC innerhalb der URB-Grenze nicht.");
        static_cast<void>(translate_guest_address(tlb_cpu,
                                                  0x5000u,
                                                  MemoryAccessOperation::Read,
                                                  MemoryAccessWidth::Byte));
        require(((tlb_cpu.mmucr >> 10u) & 0x3Fu) == 0u,
                "UTLB-Zugriff laesst URC an URB nicht auf null umlaufen.");
        const auto bulk_generation = tlb_cpu.address_space->snapshot().mmu_generation;
        advance_utlb_access_counter(tlb_cpu, 130u);
        require(((tlb_cpu.mmucr >> 10u) & 0x3Fu) == 1u &&
                    tlb_cpu.address_space->snapshot().mmucr == tlb_cpu.mmucr &&
                    tlb_cpu.address_space->snapshot().mmu_generation == bulk_generation,
                "Gebatchte UTLB-Zugriffe bilden URC/URB oder den MMU-Spiegel nicht exakt ab.");
        const auto bulk_mmucr = tlb_cpu.mmucr;
        advance_utlb_access_counter(tlb_cpu, 0u);
        require(tlb_cpu.mmucr == bulk_mmucr,
                "Null UTLB-Zugriffe veraendern den MMU-Zustand.");

        CpuState out_of_range_urc_cpu;
        out_of_range_urc_cpu.address_space = std::make_shared<RuntimeAddressSpace>();
        out_of_range_urc_cpu.mmucr = 1u | (63u << 10u) | (3u << 18u);
        out_of_range_urc_cpu.address_space->set_mode(AddressTranslationMode::Mmu);
        out_of_range_urc_cpu.address_space->write_mmucr(out_of_range_urc_cpu.mmucr);
        advance_utlb_access_counter(out_of_range_urc_cpu, 5u);
        require(((out_of_range_urc_cpu.mmucr >> 10u) & 0x3Fu) == 1u,
                "Gebatchtes URC normalisiert einen Wert oberhalb URB nicht wie Einzelzugriffe.");

        CpuState maximum_bulk_urc_cpu;
        maximum_bulk_urc_cpu.mmucr = 1u | (1u << 10u) | (3u << 18u);
        advance_utlb_access_counter(
            maximum_bulk_urc_cpu, std::numeric_limits<std::uint64_t>::max());
        require(((maximum_bulk_urc_cpu.mmucr >> 10u) & 0x3Fu) == 1u,
                "Maximale gebatchte UTLB-Zugriffszahl laeuft vor der URB-Modulooperation ueber.");

        CpuState full_boundary_urc_cpu;
        full_boundary_urc_cpu.mmucr = 63u << 10u;
        advance_utlb_access_counter(full_boundary_urc_cpu, 1u);
        require(((full_boundary_urc_cpu.mmucr >> 10u) & 0x3Fu) == 0u,
                "URB=0 verwendet fuer gebatchte UTLB-Zugriffe nicht die 64er-Grenze.");

        CpuState sq_cpu;
        sq_cpu.address_space = std::make_shared<RuntimeAddressSpace>();
        sq_cpu.mmucr = 1u | (1u << 10u) | (3u << 18u);
        sq_cpu.address_space->set_mode(AddressTranslationMode::Mmu);
        sq_cpu.address_space->write_mmucr(sq_cpu.mmucr);
        sq_cpu.address_space->ldtlb({0xE0000000u,
                                    0x10000000u,
                                    1048576u,
                                    0u,
                                    2u,
                                    true,
                                    true,
                                    true,
                                    true,
                                    true,
                                    true,
                                    false});
        const auto sq_translation = translate_store_queue_prefetch(sq_cpu, 0xE0000020u);
        require(sq_translation.addressing == StoreQueueAddressingMode::Utlb &&
                    sq_translation.target_address == 0x10000020u &&
                    ((sq_cpu.mmucr >> 10u) & 0x3Fu) == 2u,
                "MMU-SQ-PREF aktualisiert URC nicht exakt einmal nach dem UTLB-Zugriff.");
        CpuState qacr_cpu;
        qacr_cpu.mmucr = 7u << 10u;
        const auto qacr_translation =
            translate_store_queue_prefetch(qacr_cpu, 0xE0000020u);
        require(qacr_translation.addressing == StoreQueueAddressingMode::Qacr &&
                    ((qacr_cpu.mmucr >> 10u) & 0x3Fu) == 7u,
                "No-MMU-QACR-PREF wird faelschlich als UTLB-Zugriff gezaehlt.");

        const auto instruction_refill = tlb_cpu.address_space->translate(
            0x5000u, TranslationAccess::Instruction, true);
        const auto instruction_hit = tlb_cpu.address_space->translate(
            0x5000u, TranslationAccess::Instruction, true);
        require(instruction_refill.itlb_refilled &&
                    instruction_refill.utlb_slot != 0xFFu &&
                    instruction_hit.itlb_slot == instruction_refill.itlb_slot &&
                    !instruction_hit.itlb_refilled,
                "Instruktionszugriff fuellt die ITLB nicht deterministisch aus der UTLB nach.");

        RuntimeAddressSpace lru_space;
        lru_space.set_mode(AddressTranslationMode::Mmu);
        lru_space.write_mmucr(1u);
        for (std::uint8_t slot = 0u; slot < 5u; ++slot) {
            const auto address = 0x1000u + static_cast<std::uint32_t>(slot) * 0x1000u;
            lru_space.ldtlb({address,
                            0x0C001000u + static_cast<std::uint32_t>(slot) * 0x1000u,
                            4096u,
                            0u,
                            slot,
                            true,
                            true,
                            true,
                            true,
                            true,
                            true,
                            false});
        }
        for (std::uint8_t slot = 0u; slot < 4u; ++slot) {
            const auto address = 0x1000u + static_cast<std::uint32_t>(slot) * 0x1000u;
            static_cast<void>(
                lru_space.translate(address, TranslationAccess::Instruction, true));
        }
        const auto filled_lru = lru_space.snapshot();
        require(filled_lru.itlb_valid ==
                        std::array<bool, 4u>{true, true, true, true} &&
                    filled_lru.itlb_lru ==
                        std::array<std::uint8_t, 4u>{0u, 1u, 2u, 3u} &&
                    filled_lru.itlb_source_slots ==
                        std::array<std::uint8_t, 4u>{0u, 1u, 2u, 3u},
                "ITLB-Refill in freie Slots zerstoert die eindeutige LRU-Rangfolge.");
        static_cast<void>(
            lru_space.translate(0x1000u, TranslationAccess::Instruction, true));
        static_cast<void>(
            lru_space.translate(0x5000u, TranslationAccess::Instruction, true));
        const auto replaced_lru = lru_space.snapshot();
        require(replaced_lru.itlb_lru ==
                        std::array<std::uint8_t, 4u>{2u, 3u, 0u, 1u} &&
                    replaced_lru.itlb_source_slots ==
                        std::array<std::uint8_t, 4u>{0u, 4u, 2u, 3u},
                "ITLB ersetzt nach Touch/Refill nicht den echten LRU-Eintrag.");

        Sh4MmuControl mmu_control(tlb_cpu, *tlb_cpu.address_space);
        mmu_control.write(0x10u, tlb_cpu.mmucr | 0x4u);
        const auto invalidated = tlb_cpu.address_space->snapshot();
        require(std::none_of(invalidated.itlb_valid.begin(),
                             invalidated.itlb_valid.end(),
                             [](const bool valid) { return valid; }) &&
                    invalidated.mappings.empty(),
                "MMUCR.TI invalidiert ITLB und UTLB nicht gemeinsam.");

        const auto single = space.guard_for(0x1234u, 0u);
        const auto double_precision = space.guard_for(0x1234u, fpscr_pr_mask | fpscr_fr_mask | 1u);
        require(!(single == double_precision),
                "FPSCR PR/FR/RM verwendet eine inkompatible Blockvariante wieder.");
        require(space.block_fits_translation_page(0x1FF0u, 16u) &&
                    !space.block_fits_translation_page(0x1FF0u, 18u),
                "Aktive MMU schneidet Bloecke nicht konservativ an Seitengrenzen.");
        RuntimeAddressSpace contiguous_pages;
        contiguous_pages.set_mode(AddressTranslationMode::Mmu);
        contiguous_pages.write_mmucr(1u);
        contiguous_pages.ldtlb(
            {0x1000u,
             0x0C001000u,
             4096u,
             0u,
             0u,
             true,
             true,
             true,
             true,
             true,
             true,
             false});
        contiguous_pages.ldtlb(
            {0x2000u,
             0x0C002000u,
             4096u,
             0u,
             1u,
             true,
             true,
             true,
             true,
             true,
             true,
             false});
        require(!contiguous_pages.block_fits_translation_page(0x1FF0u, 32u) &&
                    contiguous_pages.prove_instruction_mapping(
                        0x1FF0u, 0x0C001FF0u, 32u, true) &&
                    !contiguous_pages.prove_instruction_mapping(
                        0x1FF0u, 0x0C001FF0u, 31u, true),
                "Seitengrenze und mehrseitiger physischer Herkunftsnachweis werden vermischt.");
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

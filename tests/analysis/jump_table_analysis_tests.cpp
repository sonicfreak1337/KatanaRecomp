#include "katana/analysis/jump_table_analysis.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

katana::sh4::DisassemblyLine line(const std::uint32_t address,
                                  const katana::sh4::InstructionKind kind,
                                  const std::uint8_t source = 0u,
                                  const std::uint8_t destination = 0u) {
    katana::sh4::DisassemblyLine result;
    result.address = address;
    result.instruction.kind = kind;
    result.instruction.source_register = source;
    result.instruction.destination_register = destination;
    return result;
}

} // namespace

int main() {
    katana::io::ExecutableImage image;
    image.add_segment({".text",
                       0x1000u,
                       0u,
                       8u,
                       katana::io::SegmentKind::Code,
                       {true, false, true},
                       {0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u}});
    image.add_segment(
        {".jumptable",
         0x2000u,
         8u,
         12u,
         katana::io::SegmentKind::Data,
         {true, false, false},
         {0x00u, 0x10u, 0x00u, 0x00u, 0x04u, 0x10u, 0x00u, 0x00u, 0x01u, 0x10u, 0x00u, 0x00u}});

    const auto resolved = katana::analysis::analyze_jump_table(image, 0x1010u, 0x2000u, 2u);
    require(resolved.resolved && resolved.entries.size() == 2u,
            "Gueltige Tabelle wurde nicht aufgeloest.");
    require(resolved.entries[0].target == 0x1000u && resolved.entries[1].target == 0x1004u,
            "Absolute Tabellenziele wurden falsch gelesen.");

    katana::io::ExecutableImage aliased;
    aliased.set_address_model(katana::io::ImageAddressModel::Sh4DirectMapped);
    aliased.add_segment({".text",
                         0x8C001000u,
                         0u,
                         4u,
                         katana::io::SegmentKind::Code,
                         {true, false, true},
                         {0x09u, 0x00u, 0x09u, 0x00u}});
    aliased.add_segment({".jumptable",
                         0x8C002000u,
                         4u,
                         4u,
                         katana::io::SegmentKind::Data,
                         {true, false, false},
                         {0x00u, 0x10u, 0x00u, 0xACu}});
    const auto aliased_target =
        katana::analysis::analyze_jump_table(aliased, 0x8C001000u, 0x8C002000u, 1u);
    require(aliased_target.resolved && aliased_target.entries[0].target == 0x8C001000u,
            "Ein P2-Jump-Table-Ziel wurde nicht auf die kanonische P1-Codeadresse normalisiert.");
    katana::analysis::JumpTableSnapshotCache snapshot_cache(2u);
    const auto cached_first =
        katana::analysis::analyze_jump_table(image, 0x1010u, 0x2000u, 2u, &snapshot_cache);
    const auto cached_second =
        katana::analysis::analyze_jump_table(image, 0x1010u, 0x2000u, 2u, &snapshot_cache);
    require(cached_first.resolved == cached_second.resolved &&
                cached_first.entries.size() == cached_second.entries.size() &&
                snapshot_cache.counters().misses == 1u && snapshot_cache.counters().hits == 1u,
            "Begrenzter Jump-Table-Snapshotcache verliert Ergebnis oder Hitstatus.");

    const auto rejected = katana::analysis::analyze_jump_table(image, 0x1010u, 0x2000u, 3u);
    require(!rejected.resolved, "Ungerades Sprungziel wurde als sicher markiert.");
    require(!rejected.entries[2].accepted && rejected.entries[2].reason == "odd-address",
            "Abgelehnter Tabelleneintrag hat keinen stabilen Grund.");

    const auto unbounded = katana::analysis::analyze_jump_table(image, 0x1010u, 0x2000u, 0u);
    require(!unbounded.resolved && unbounded.reason == "entry-count-out-of-range",
            "Unbegrenzte Tabelle wurde nicht abgelehnt.");

    katana::io::ExecutableImage absolute_pointer_run;
    std::vector<std::uint8_t> absolute_bytes(0x40u, 0u);
    const auto put_u32 = [&absolute_bytes](const std::size_t offset,
                                          const std::uint32_t value) {
        absolute_bytes[offset] = static_cast<std::uint8_t>(value);
        absolute_bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        absolute_bytes[offset + 2u] = static_cast<std::uint8_t>(value >> 16u);
        absolute_bytes[offset + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    absolute_bytes[0x00u] = 0x03u;
    absolute_bytes[0x01u] = 0xD3u;
    absolute_bytes[0x02u] = 0x3Eu;
    absolute_bytes[0x03u] = 0x03u;
    absolute_bytes[0x04u] = 0xFCu;
    absolute_bytes[0x05u] = 0x70u;
    absolute_bytes[0x06u] = 0x2Bu;
    absolute_bytes[0x07u] = 0x43u;
    absolute_bytes[0x08u] = 0x09u;
    absolute_bytes[0x09u] = 0x00u;
    put_u32(0x10u, 0x80001020u);
    put_u32(0x20u, 0x80001030u);
    put_u32(0x24u, 0x80001034u);
    absolute_bytes[0x30u] = 0x0Bu;
    absolute_bytes[0x31u] = 0x00u;
    absolute_bytes[0x32u] = 0x09u;
    absolute_bytes[0x33u] = 0x00u;
    absolute_bytes[0x34u] = 0x0Bu;
    absolute_bytes[0x35u] = 0x00u;
    absolute_bytes[0x36u] = 0x09u;
    absolute_bytes[0x37u] = 0x00u;
    absolute_pointer_run.add_segment({".bootstrap",
                                      0xA0001000u,
                                      0u,
                                      absolute_bytes.size(),
                                      katana::io::SegmentKind::Code,
                                      {true, true, true},
                                      std::move(absolute_bytes),
                                      katana::io::ImageSourceKind::DiscBootFile,
                                      katana::io::ImageLoadPhase::Initial,
                                      "synthetic-bootstrap"});
    absolute_pointer_run.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
    absolute_pointer_run.set_address_model(katana::io::ImageAddressModel::Sh4DirectMapped);
    const auto absolute_lines = katana::sh4::disassemble(
        absolute_pointer_run.segments()[0].bytes, 0xA0001000u);
    const auto absolute = katana::analysis::recognize_snapshot_absolute_jump_table_candidates(
        absolute_pointer_run, absolute_lines, 3u);
    require(absolute.has_value() && absolute->resolved &&
                absolute->aot_candidates_only &&
                absolute->evidence == katana::analysis::ControlFlowEvidence::GuardedPartial &&
                absolute->entries.size() == 2u &&
                absolute->entries[0].target == 0xA0001030u &&
                absolute->entries[1].target == 0xA0001034u &&
                absolute->reason == "snapshot-absolute-pointer-candidates",
            "RWX-Disc-Snapshotkandidaten einer absoluten SH-4-Sprungtabelle wurden nicht "
            "konservativ erkannt.");

    auto no_snapshot_contract = absolute_pointer_run;
    no_snapshot_contract.set_initial_snapshot_policy(
        katana::io::InitialSnapshotPolicy::ImmutableOnly);
    require(!katana::analysis::recognize_snapshot_absolute_jump_table_candidates(
                 no_snapshot_contract, absolute_lines, 3u)
                 .has_value(),
            "Schreibbare Sprungtabellendaten wurden ohne partiellen Snapshotvertrag verwendet.");

    constexpr std::uint32_t call_island_base = 0x00400000u;
    const auto call_island_image = [](const std::size_t return_handler_count,
                                      const bool broken_return,
                                      const katana::io::ImageSourceKind source_kind,
                                      const bool call_dispatch,
                                      const bool memory_derived,
                                      const bool terminal_tail,
                                      const bool executable = true) {
        constexpr std::uint32_t base = call_island_base;
        constexpr std::size_t first_handler = 0x20u;
        constexpr std::size_t stride = 6u;
        std::vector<std::uint8_t> bytes(0x60u, 0u);
        const auto put_u16 = [&bytes](const std::size_t offset,
                                      const std::uint16_t value) {
            bytes[offset] = static_cast<std::uint8_t>(value);
            bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
        };

        // mov.w @r0+,r3; add #12,r3; bsrf r3; nop; rts; nop
        put_u16(0x00u, memory_derived ? 0x6305u : 0xE300u);
        put_u16(0x02u, 0x730Cu);
        put_u16(0x04u, call_dispatch ? 0x0303u : 0x0323u);
        put_u16(0x06u, 0x0009u);
        put_u16(0x08u, 0x000Bu);
        put_u16(0x0Au, 0x0009u);

        for (std::size_t index = 0u; index < return_handler_count; ++index) {
            const auto offset = first_handler + index * stride;
            // mov #0,r1; rts; mov.l r1,@r2 (delay slot)
            put_u16(offset, 0xE100u);
            put_u16(offset + 2u,
                    broken_return && index == return_handler_count / 2u ? 0x0009u : 0x000Bu);
            put_u16(offset + 4u, 0x2212u);
        }

        if (terminal_tail) {
            const auto offset = first_handler + return_handler_count * stride;
            constexpr std::size_t tail_target = 0x50u;
            const auto displacement = static_cast<std::uint16_t>(
                (tail_target - (offset + 4u)) / 2u);
            put_u16(offset, static_cast<std::uint16_t>(0xA000u | displacement));
            put_u16(offset + 2u, 0x0009u);
            put_u16(offset + 4u, 0x0009u);
            put_u16(tail_target, 0x000Bu);
            put_u16(tail_target + 2u, 0x0009u);
        }

        katana::io::ExecutableImage result;
        result.set_initial_snapshot_policy(
            katana::io::InitialSnapshotPolicy::EntryPointStraightLineQuiescent);
        result.add_segment({".synthetic-call-island",
                            base,
                            0u,
                            bytes.size(),
                            katana::io::SegmentKind::Mixed,
                            {true, true, executable},
                            std::move(bytes),
                            source_kind,
                            katana::io::ImageLoadPhase::Initial,
                            "synthetic-call-island"});
        result.add_entry_point(base);
        return result;
    };

    auto call_island = call_island_image(
        4u, false, katana::io::ImageSourceKind::DiscBootFile, true, true, true);
    const auto call_island_lines =
        katana::sh4::disassemble(call_island.segments()[0].bytes, call_island_base);
    const auto call_island_candidates =
        katana::analysis::recognize_snapshot_relative_call_island_candidates(
            call_island, call_island_lines, 2u);
    const std::vector<std::uint32_t> expected_call_island_targets{
        call_island_base + 0x20u,
        call_island_base + 0x26u,
        call_island_base + 0x2Cu,
        call_island_base + 0x32u,
        call_island_base + 0x38u};
    require(call_island_candidates.has_value() &&
                call_island_candidates->dispatch_address == call_island_base + 4u &&
                call_island_candidates->first_target == call_island_base + 0x20u &&
                call_island_candidates->stride == 6u &&
                call_island_candidates->targets == expected_call_island_targets &&
                call_island_candidates->terminal_tail_transfer &&
                call_island_candidates->reason ==
                    "snapshot-relative-call-island-candidates",
            "Eine speicherabgeleitete BSRF-Handlerinsel wurde nicht begrenzt erkannt.");

    const auto recognize_call_island = [&](katana::io::ExecutableImage candidate) {
        const auto lines =
            katana::sh4::disassemble(candidate.segments()[0].bytes, call_island_base);
        return katana::analysis::recognize_snapshot_relative_call_island_candidates(
            candidate, lines, 2u);
    };
    require(!recognize_call_island(call_island_image(
                 3u, false, katana::io::ImageSourceKind::DiscBootFile, true, true, true))
                 .has_value(),
            "Nur drei gleichfoermige Return-Handler wurden als belastbare Insel akzeptiert.");
    require(!recognize_call_island(call_island_image(
                 4u, true, katana::io::ImageSourceKind::DiscBootFile, true, true, true))
                 .has_value(),
            "Eine Handlerinsel mit kaputtem RTS-Muster wurde akzeptiert.");
    require(!recognize_call_island(call_island_image(
                 4u, false, katana::io::ImageSourceKind::RuntimeMemory, true, true, true))
                 .has_value(),
            "Laufzeitspeicher wurde als statische BSRF-Handlerinsel eingefroren.");
    require(!recognize_call_island(call_island_image(
                 4u, false, katana::io::ImageSourceKind::DiscBootFile, false, true, true))
                 .has_value(),
            "Ein BRAF wurde faelschlich als BSRF-Handlerinsel klassifiziert.");
    require(!recognize_call_island(call_island_image(
                 4u, false, katana::io::ImageSourceKind::DiscBootFile, true, false, true))
                 .has_value(),
            "Ein BSRF ohne speicherabgeleitetes relatives Register wurde eingefroren.");
    require(!recognize_call_island(call_island_image(
                 4u, false, katana::io::ImageSourceKind::DiscBootFile, true, true, false))
                 .has_value(),
            "Eine nicht terminierte BSRF-Insel wurde am Scanende akzeptiert.");
    require(!recognize_call_island(call_island_image(
                 4u,
                 false,
                 katana::io::ImageSourceKind::DiscBootFile,
                 true,
                 true,
                 true,
                 false))
                 .has_value(),
            "Nicht ausfuehrbare Snapshotbytes wurden als BSRF-Codeinsel akzeptiert.");

    katana::io::ExecutableImage boundaries;
    boundaries.add_segment({".code",
                            0x3000u,
                            0u,
                            8u,
                            katana::io::SegmentKind::Code,
                            {true, false, true},
                            {0x09u, 0x00u, 0x0Bu, 0x00u}});
    boundaries.add_segment({".data",
                            0x4000u,
                            4u,
                            28u,
                            katana::io::SegmentKind::Data,
                            {true, false, false},
                            {0x00u, 0x30u, 0x00u, 0x00u, 0x04u, 0x30u, 0x00u, 0x00u, 0x02u, 0x30u,
                             0x00u, 0x00u, 0x00u, 0x40u, 0x00u, 0x00u, 0x01u, 0x30u, 0x00u, 0x00u,
                             0x00u, 0x50u, 0x00u, 0x00u, 0x00u, 0x30u, 0x00u, 0x00u}});
    const std::array expected_reasons{"bounded-table",
                                      "outside-committed-data",
                                      "bounded-table",
                                      "not-code-segment",
                                      "odd-address",
                                      "outside-segments"};
    for (std::size_t index = 0u; index < expected_reasons.size(); ++index) {
        const auto check = katana::analysis::analyze_jump_table(
            boundaries, 0x3000u, 0x4000u + static_cast<std::uint32_t>(index * 4u), 1u);
        const auto actual = check.resolved ? check.reason : check.entries[0].reason;
        require(actual == expected_reasons[index],
                "Committed-Code-Grenzfall wurde falsch klassifiziert.");
    }

    boundaries.add_segment({".top",
                            0xFFFFFFFCu,
                            32u,
                            4u,
                            katana::io::SegmentKind::Data,
                            {true, false, false},
                            {0x00u, 0x30u, 0x00u, 0x00u}});
    const auto top_one = katana::analysis::analyze_jump_table(boundaries, 0x3000u, 0xFFFFFFFCu, 1u);
    require(top_one.resolved,
            "Ein Eintrag bei 0xFFFFFFFC wurde als arithmetischer Ueberlauf abgelehnt.");
    const auto top_two = katana::analysis::analyze_jump_table(boundaries, 0x3000u, 0xFFFFFFFCu, 2u);
    require(top_two.reason == "table-range-invalid",
            "Ueberlauf ueber den Adressraum wurde nicht abgelehnt.");
    const auto too_many = katana::analysis::analyze_jump_table(boundaries, 0x3000u, 0x4000u, 4097u);
    require(too_many.reason == "entry-count-out-of-range", "Maximalgrenze 4096 ist instabil.");
    const auto misaligned = katana::analysis::analyze_jump_table(boundaries, 0x3000u, 0x4002u, 1u);
    require(misaligned.reason == "table-range-invalid",
            "Unausgerichtete Tabellenbasis wurde akzeptiert.");

    boundaries.add_segment({".sparse-table",
                            0x6000u,
                            36u,
                            8u,
                            katana::io::SegmentKind::Data,
                            {true, false, false},
                            {0x00u, 0x30u, 0x00u, 0x00u}});
    const auto sparse = katana::analysis::analyze_jump_table(boundaries, 0x3000u, 0x6000u, 2u);
    require(!sparse.resolved && sparse.entries.empty() &&
                sparse.reason == "table-range-not-immutable",
            "Teilweise committed Tabelle wurde nicht als gesamter Snapshot abgelehnt.");

    katana::io::ExecutableImage writable;
    writable.add_segment({".text",
                          0x3000u,
                          0u,
                          8u,
                          katana::io::SegmentKind::Code,
                          {true, false, true},
                          {0x09u, 0x00u, 0x0Bu, 0x00u}});
    writable.add_segment({".ram-table",
                          0x7000u,
                          4u,
                          8u,
                          katana::io::SegmentKind::Data,
                          {true, true, false},
                          {0x00u, 0x30u, 0x00u, 0x00u, 0x02u, 0x30u, 0x00u, 0x00u}});
    const auto writable_absolute =
        katana::analysis::analyze_jump_table(writable, 0x3000u, 0x7000u, 2u);
    require(!writable_absolute.resolved && writable_absolute.entries.empty() &&
                writable_absolute.reason == "table-segment-writable",
            "Beschreibbare Absolute32-Tabelle wurde statisch eingefroren.");

    katana::io::ExecutableImage relative;
    relative.add_segment({".text",
                          0u,
                          0u,
                          0x80u,
                          katana::io::SegmentKind::Code,
                          {true, false, true},
                          std::vector<std::uint8_t>(0x80u, 0x09u)});
    relative.add_segment({".relative",
                          0x100u,
                          0x80u,
                          8u,
                          katana::io::SegmentKind::Data,
                          {true, false, false},
                          {// -4, 0, -3, -4
                           0xFCu,
                           0xFFu,
                           0x00u,
                           0x00u,
                           0xFDu,
                           0xFFu,
                           0xFCu,
                           0xFFu}});
    const auto signed_entries =
        katana::analysis::analyze_relative_jump_table(relative, 0x20u, 0x100u, 0x20u, 2u);
    require(signed_entries.resolved && signed_entries.entries[0].target == 0x1Cu &&
                signed_entries.entries[1].target == 0x20u,
            "Negative MOV.W-Tabelleneintraege wurden nicht vorzeichenerweitert.");
    const auto conflicting =
        katana::analysis::analyze_relative_jump_table(relative, 0x20u, 0x104u, 0x20u, 2u);
    require(!conflicting.resolved && conflicting.entries.size() == 2u &&
                conflicting.entries[0].reason == "odd-address" && conflicting.entries[1].accepted,
            "Widerspruechliche Mehrfachziele wurden nicht als ganze Tabelle abgelehnt.");
    const auto duplicate =
        katana::analysis::analyze_relative_jump_table(relative, 0x20u, 0x106u, 0x20u, 1u);
    require(duplicate.resolved && duplicate.entries[0].target == 0x1Cu,
            "Doppeltes gueltiges Ziel wurde inkonsistent bewertet.");
    const auto negative_overflow =
        katana::analysis::analyze_relative_jump_table(relative, 0x20u, 0x100u, 2u, 1u);
    require(!negative_overflow.resolved &&
                negative_overflow.entries[0].reason == "target-address-overflow",
            "Negativer relativer Zielwert lief unbemerkt durch Adresse null.");

    auto bt_lines = std::vector<katana::sh4::DisassemblyLine>{
        line(0u, katana::sh4::InstructionKind::MovImmediate, 0u, 1u),
        line(2u, katana::sh4::InstructionKind::CompareHigherOrSame, 1u, 2u),
        line(4u, katana::sh4::InstructionKind::Bt),
        line(6u, katana::sh4::InstructionKind::ShiftLogicalLeftOne, 0u, 2u),
        line(8u, katana::sh4::InstructionKind::MovRegister, 2u, 3u),
        line(10u, katana::sh4::InstructionKind::MoveAddressPcRelative),
        line(12u, katana::sh4::InstructionKind::MovWordLoadR0Indexed, 3u, 4u),
        line(14u, katana::sh4::InstructionKind::Braf)};
    bt_lines[0].instruction.immediate = 2;
    bt_lines[2].target_address = 0x40u;
    bt_lines[5].instruction.displacement = 0xF4;
    bt_lines[7].instruction.branch_register = 4u;
    const auto bt_table =
        katana::analysis::recognize_bounded_relative_jump_table(relative, bt_lines, 7u);
    require(bt_table.has_value() && bt_table->resolved && bt_table->entries.size() == 2u,
            "BT-begrenzte relative Tabelle wurde nicht erkannt.");

    auto bf_lines = std::vector<katana::sh4::DisassemblyLine>{
        line(0u, katana::sh4::InstructionKind::MovImmediate, 0u, 1u),
        line(2u, katana::sh4::InstructionKind::CompareHigherOrSame, 1u, 2u),
        line(4u, katana::sh4::InstructionKind::Bf),
        line(6u, katana::sh4::InstructionKind::Bra),
        line(8u, katana::sh4::InstructionKind::Nop),
        line(10u, katana::sh4::InstructionKind::ShiftLogicalLeftOne, 0u, 2u),
        line(12u, katana::sh4::InstructionKind::MovRegister, 2u, 3u),
        line(14u, katana::sh4::InstructionKind::MoveAddressPcRelative),
        line(16u, katana::sh4::InstructionKind::MovWordLoadR0Indexed, 3u, 4u),
        line(18u, katana::sh4::InstructionKind::Braf)};
    bf_lines[0].instruction.immediate = 2;
    bf_lines[2].target_address = 10u;
    bf_lines[3].target_address = 0x40u;
    bf_lines[7].instruction.displacement = 0xF0;
    bf_lines[9].instruction.branch_register = 4u;
    const auto bf_table =
        katana::analysis::recognize_bounded_relative_jump_table(relative, bf_lines, 9u);
    require(bf_table.has_value() && bf_table->resolved && bf_table->entries.size() == 2u,
            "BF-plus-BRA-begrenzte relative Tabelle wurde nicht erkannt.");

    katana::io::ExecutableImage mutable_relative;
    mutable_relative.add_segment({".rwx",
                                  0u,
                                  0u,
                                  0x108u,
                                  katana::io::SegmentKind::Code,
                                  {true, true, true},
                                  std::vector<std::uint8_t>(0x108u, 0u)});
    const auto mutable_auto =
        katana::analysis::recognize_bounded_relative_jump_table(mutable_relative, bt_lines, 7u);
    require(mutable_auto.has_value() && !mutable_auto->resolved &&
                mutable_auto->reason == "table-segment-writable",
            "Automatisch erkannte RWX-Relative16-Tabelle wurde statisch eingefroren.");

    std::cout << "KR-1804/KR-4712 Jump-Table-Analyse erfolgreich.\n";
    return EXIT_SUCCESS;
}

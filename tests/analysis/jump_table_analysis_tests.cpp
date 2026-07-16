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

    const auto rejected = katana::analysis::analyze_jump_table(image, 0x1010u, 0x2000u, 3u);
    require(!rejected.resolved, "Ungerades Sprungziel wurde als sicher markiert.");
    require(!rejected.entries[2].accepted && rejected.entries[2].reason == "odd-address",
            "Abgelehnter Tabelleneintrag hat keinen stabilen Grund.");

    const auto unbounded = katana::analysis::analyze_jump_table(image, 0x1010u, 0x2000u, 0u);
    require(!unbounded.resolved && unbounded.reason == "entry-count-out-of-range",
            "Unbegrenzte Tabelle wurde nicht abgelehnt.");

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
                conflicting.entries[0].reason == "odd-address" &&
                conflicting.entries[1].accepted,
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
    const auto mutable_auto = katana::analysis::recognize_bounded_relative_jump_table(
        mutable_relative, bt_lines, 7u);
    require(mutable_auto.has_value() && !mutable_auto->resolved &&
                mutable_auto->reason == "table-segment-writable",
            "Automatisch erkannte RWX-Relative16-Tabelle wurde statisch eingefroren.");

    std::cout << "KR-1804/KR-4712 Jump-Table-Analyse erfolgreich.\n";
    return EXIT_SUCCESS;
}

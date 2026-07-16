#include "katana/analysis/jump_table_analysis.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
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
    require(!sparse.resolved && sparse.entries.size() == 2u && sparse.entries[0].accepted &&
                sparse.entries[1].reason == "entry-not-committed",
            "Nicht committed Tabelleneintrag wurde nicht getrennt abgelehnt.");
    std::cout << "KR-1804 Jump-Table-Analyse erfolgreich.\n";
    return EXIT_SUCCESS;
}

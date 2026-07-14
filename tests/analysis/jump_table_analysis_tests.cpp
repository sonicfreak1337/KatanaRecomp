#include "katana/analysis/jump_table_analysis.hpp"

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

}

int main() {
    katana::io::ExecutableImage image;
    image.add_segment({
        ".text", 0x1000u, 0u, 8u, katana::io::SegmentKind::Code, {true, false, true},
        {0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u, 0x09u, 0x00u}
    });
    image.add_segment({
        ".jumptable", 0x2000u, 8u, 12u, katana::io::SegmentKind::Data, {true, false, false},
        {0x00u, 0x10u, 0x00u, 0x00u, 0x04u, 0x10u, 0x00u, 0x00u,
         0x01u, 0x10u, 0x00u, 0x00u}
    });

    const auto resolved = katana::analysis::analyze_jump_table(image, 0x1010u, 0x2000u, 2u);
    require(resolved.resolved && resolved.entries.size() == 2u, "Gueltige Tabelle wurde nicht aufgeloest.");
    require(
        resolved.entries[0].target == 0x1000u && resolved.entries[1].target == 0x1004u,
        "Absolute Tabellenziele wurden falsch gelesen."
    );

    const auto rejected = katana::analysis::analyze_jump_table(image, 0x1010u, 0x2000u, 3u);
    require(!rejected.resolved, "Ungerades Sprungziel wurde als sicher markiert.");
    require(
        !rejected.entries[2].accepted
            && rejected.entries[2].reason == "target-not-committed-executable-code",
        "Abgelehnter Tabelleneintrag hat keinen stabilen Grund."
    );

    const auto unbounded = katana::analysis::analyze_jump_table(image, 0x1010u, 0x2000u, 0u);
    require(
        !unbounded.resolved && unbounded.reason == "entry-count-out-of-range",
        "Unbegrenzte Tabelle wurde nicht abgelehnt."
    );
    std::cout << "KR-1804 Jump-Table-Analyse erfolgreich.\n";
    return EXIT_SUCCESS;
}

#include "katana/analysis/recursive_analysis.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
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
    using namespace katana::io;
    ExecutableImage image("recursive-fixture.bin");
    image.add_segment({
        ".text",
        0x8C010000u,
        0u,
        14u,
        SegmentKind::Code,
        {true, false, true},
        {
            0x02u, 0xB0u, 0x09u, 0x00u,
            0x0Bu, 0x00u, 0x09u, 0x00u,
            0x0Bu, 0x00u, 0x09u, 0x00u,
            0xFFu, 0xFFu
        }
    });
    image.add_segment({
        ".data", 0x8C020000u, 14u, 4u, SegmentKind::Data,
        {true, true, false}, {1u, 2u, 3u, 4u}
    });
    image.add_segment({
        ".mystery", 0x8C030000u, 18u, 4u, SegmentKind::Unknown,
        {true, false, false}, {5u, 6u, 7u, 8u}
    });
    image.add_entry_point(0x8C010000u);
    image.add_symbol({
        "subroutine", 0x8C010008u, 4u, SymbolKind::Function, SymbolBinding::Global
    });

    const auto result = katana::analysis::analyze_reachable_code(image);
    require(result.instructions.size() == 6u, "Die Worklist hat nicht genau den erreichbaren Code entdeckt.");
    require(result.instructions.front().address == 0x8C010000u, "Der Einstiegspunkt fehlt.");
    require(result.instructions[1].is_delay_slot, "Der Call-Delay-Slot wurde nicht markiert.");
    require(result.instructions[4].address == 0x8C010008u, "Das direkte Callziel wurde nicht verfolgt.");
    require(result.instructions.back().address == 0x8C01000Au, "Der Delay-Slot des Callziels fehlt.");

    for (const auto& line : result.instructions) {
        require(line.address != 0x8C01000Cu, "Nicht erreichbare Bytes wurden linear dekodiert.");
    }
    require(result.ranges.size() == 4u, "Die Klassifikationsbereiche wurden nicht normalisiert.");
    require(
        result.ranges[0].start_address == 0x8C010000u
            && result.ranges[0].size == 12u
            && result.ranges[0].kind == katana::analysis::DiscoveredByteKind::Code,
        "Erreichbarer Code wurde falsch klassifiziert."
    );
    require(
        result.ranges[1].start_address == 0x8C01000Cu
            && result.ranges[1].size == 2u
            && result.ranges[1].kind == katana::analysis::DiscoveredByteKind::Unknown,
        "Nicht erreichbarer Codebereich wurde nicht als unknown erhalten."
    );
    require(result.ranges[2].kind == katana::analysis::DiscoveredByteKind::Data, "Datensegment wurde falsch klassifiziert.");
    require(result.ranges[3].kind == katana::analysis::DiscoveredByteKind::Unknown, "Unknown-Segment wurde falsch klassifiziert.");
    require(
        std::string(katana::analysis::discovered_byte_kind_name(result.ranges[2].kind)) == "data",
        "Klassifikationsname ist instabil."
    );
    require(result.functions.size() == 2u, "Funktionskandidaten wurden nicht zusammengefuehrt.");
    require(
        result.functions[0].address == 0x8C010000u
            && result.functions[0].confidence == katana::analysis::AnalysisConfidence::Certain
            && result.functions[0].origins == std::vector<katana::analysis::FunctionOrigin>{katana::analysis::FunctionOrigin::EntryPoint},
        "Einstiegspunkt-Herkunft oder Konfidenz ist falsch."
    );
    require(
        result.functions[1].address == 0x8C010008u
            && result.functions[1].confidence == katana::analysis::AnalysisConfidence::High
            && result.functions[1].origins.size() == 2u,
        "Call- und Symbolherkunft wurden nicht kombiniert."
    );
    require(
        std::string(katana::analysis::function_origin_name(result.functions[1].origins[0])) == "direct-call"
            && std::string(katana::analysis::analysis_confidence_name(result.functions[0].confidence)) == "certain",
        "Herkunfts- oder Konfidenzname ist instabil."
    );

    ExecutableImage invalid;
    invalid.add_segment({".data", 0x1000u, 0u, 2u, SegmentKind::Data, {true, true, false}, {0u, 0u}});
    invalid.add_entry_point(0x1000u);
    bool rejected = false;
    try {
        static_cast<void>(katana::analysis::analyze_reachable_code(invalid));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "Ein Einstiegspunkt in Daten wurde akzeptiert.");

    std::cout << "KR-1701 Worklist ab Einstiegspunkten erfolgreich.\n";
    return EXIT_SUCCESS;
}

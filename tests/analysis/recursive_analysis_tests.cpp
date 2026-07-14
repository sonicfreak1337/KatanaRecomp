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
    image.add_entry_point(0x8C010000u);

    const auto result = katana::analysis::analyze_reachable_code(image);
    require(result.instructions.size() == 6u, "Die Worklist hat nicht genau den erreichbaren Code entdeckt.");
    require(result.instructions.front().address == 0x8C010000u, "Der Einstiegspunkt fehlt.");
    require(result.instructions[1].is_delay_slot, "Der Call-Delay-Slot wurde nicht markiert.");
    require(result.instructions[4].address == 0x8C010008u, "Das direkte Callziel wurde nicht verfolgt.");
    require(result.instructions.back().address == 0x8C01000Au, "Der Delay-Slot des Callziels fehlt.");

    for (const auto& line : result.instructions) {
        require(line.address != 0x8C01000Cu, "Nicht erreichbare Bytes wurden linear dekodiert.");
    }

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

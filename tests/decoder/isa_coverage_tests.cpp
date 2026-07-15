#include "katana/sh4/isa_coverage.hpp"

#include <algorithm>
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
    using namespace katana::sh4;
    const auto report = build_isa_coverage_report();

    require(report.instructions.size() == 150u, "Nicht jede implementierte Instruktionsart erscheint im Bericht.");
    require(report.known_opcode_count + report.unknown_opcode_count == 65536u, "Der Bericht deckt den 16-Bit-Opcode-Raum nicht vollstaendig ab.");
    require(report.known_opcode_count != 0u, "Der Bericht zaehlt keine bekannten Opcodes.");
    require(report.unknown_opcode_count != 0u, "Der Bericht verschweigt unbekannte Opcodes.");

    const auto missing = std::find_if(
        report.instructions.begin(),
        report.instructions.end(),
        [](const IsaCoverageEntry& entry) {
            return entry.encoding_rule_count == 0u || entry.decoded_opcode_count == 0u || entry.name.empty();
        }
    );
    require(missing == report.instructions.end(), "Eine implementierte Instruktion ist ohne Regel, Namen oder Opcode sichtbar.");

    const auto text = format_isa_coverage_report(report);
    require(text.find("ReturnFromException") != std::string::npos, "RTE fehlt im formatierten Bericht.");
    require(text.find("StoreSpecialRegister") != std::string::npos, "Systemregistertransfers fehlen im formatierten Bericht.");
    require(text.find("FcnvSingleToDouble") != std::string::npos, "FPU-Instruktionen fehlen im formatierten Bericht.");
    require(text.find("Unbekannte Opcodes:") != std::string::npos, "Die Unknown-Abdeckung fehlt im Bericht.");

    std::cout << "KR-1503 ISA-Abdeckungsbericht erfolgreich.\n";
    return EXIT_SUCCESS;
}

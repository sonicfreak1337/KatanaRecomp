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

} // namespace

int main() {
    using namespace katana::sh4;
    const auto report = build_isa_coverage_report();

    require(report.instructions.size() == 158u,
            "Nicht jede implementierte Instruktionsart erscheint im Bericht.");
    require(report.known_opcode_count + report.unknown_opcode_count == 65536u,
            "Der Bericht deckt den 16-Bit-Opcode-Raum nicht vollstaendig ab.");
    require(report.known_opcode_count != 0u, "Der Bericht zaehlt keine bekannten Opcodes.");
    require(report.unknown_opcode_count != 0u, "Der Bericht verschweigt unbekannte Opcodes.");
    require(report.families.size() == 7u,
            "Der Alpha-ISA-Vertrag besitzt nicht alle benannten Familien.");

    const auto family = [&](const std::string& id) -> const AlphaIsaFamilyEntry& {
        const auto found = std::find_if(report.families.begin(),
                                        report.families.end(),
                                        [&](const auto& entry) { return entry.id == id; });
        require(found != report.families.end(), "Alpha-ISA-Familie fehlt: " + id);
        return *found;
    };
    require(family("integer-core").support == AlphaIsaSupport::Supported &&
                family("system-control").support == AlphaIsaSupport::Restricted &&
                family("system-control").layers.runtime == AlphaIsaSupport::Restricted &&
                family("fpu").support == AlphaIsaSupport::Restricted &&
                family("unknown-opcode").support == AlphaIsaSupport::Rejected,
            "Alpha-ISA-Familien verschweigen Unterstuetzung, Einschraenkung oder Ablehnung.");
    require(std::all_of(report.families.begin(),
                        report.families.end(),
                        [](const auto& entry) {
                            return !entry.id.empty() && !entry.name.empty() &&
                                   !entry.semantic_contract.empty() &&
                                   !entry.test_requirement.empty() &&
                                   (entry.support == AlphaIsaSupport::Supported ||
                                    !entry.limitation.empty()) &&
                                   (entry.support != AlphaIsaSupport::Supported ||
                                    (entry.layers.decoder == AlphaIsaSupport::Supported &&
                                     entry.layers.ir == AlphaIsaSupport::Supported &&
                                     entry.layers.backend == AlphaIsaSupport::Supported &&
                                     entry.layers.runtime == AlphaIsaSupport::Supported));
                        }),
            "Alpha-ISA-Behauptung besitzt keine Semantik, Grenze oder Testanforderung.");

    const auto missing = std::find_if(
        report.instructions.begin(), report.instructions.end(), [](const IsaCoverageEntry& entry) {
            return entry.encoding_rule_count == 0u || entry.decoded_opcode_count == 0u ||
                   entry.name.empty();
        });
    require(missing == report.instructions.end(),
            "Eine implementierte Instruktion ist ohne Regel, Namen oder Opcode sichtbar.");
    require(std::all_of(report.instructions.begin(),
                        report.instructions.end(),
                        [&](const auto& entry) {
                            const auto& contract = family(entry.family_id);
                            return entry.support == contract.support &&
                                   entry.test_requirement == contract.test_requirement;
                        }),
            "Instruktionsart widerspricht ihrem gemeinsamen Alpha-Familienvertrag.");

    const auto text = format_isa_coverage_report(report);
    require(text.find("ReturnFromException") != std::string::npos,
            "RTE fehlt im formatierten Bericht.");
    require(text.find("StoreSpecialRegister") != std::string::npos,
            "Systemregistertransfers fehlen im formatierten Bericht.");
    require(text.find("FcnvSingleToDouble") != std::string::npos,
            "FPU-Instruktionen fehlen im formatierten Bericht.");
    require(text.find("Unbekannte Opcodes:") != std::string::npos,
            "Die Unknown-Abdeckung fehlt im Bericht.");
    require(text.find("restricted") != std::string::npos &&
                text.find("SLEEP wakeup") != std::string::npos &&
                text.find("user-mode privilege violations trap") != std::string::npos &&
                text.find("Privilege violations and complete") == std::string::npos,
            "Textbericht verschweigt Alpha-Einschraenkungen oder ihre Semantik.");

    const auto json = format_alpha_isa_json(report);
    require(json.find("\"schema\":\"katana-alpha-isa\"") != std::string::npos &&
                json.find("\"contract_version\":1") != std::string::npos &&
                json.find("\"runtime\":\"restricted\"") != std::string::npos &&
                json.find("\"id\":\"unknown-opcode\"") != std::string::npos &&
                json.find("\"status\":\"rejected\"") != std::string::npos &&
                json.find("\"test_requirement\"") != std::string::npos,
            "Maschinenlesbarer Alpha-ISA-Bericht verliert Schichten, Grenzen oder Tests.");

    std::cout << "KR-1503/KR-4501 messbarer Alpha-ISA-Vertrag erfolgreich.\n";
    return EXIT_SUCCESS;
}

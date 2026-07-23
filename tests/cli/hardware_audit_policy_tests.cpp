#include "katana/analysis/hardware_audit.hpp"
#include "katana/cli/hardware_audit_policy.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    katana::analysis::DreamcastHardwareAudit audit;
    require(!katana::cli::hardware_audit_failed(audit, false, false) &&
                !katana::cli::hardware_audit_failed(audit, true, false) &&
                !katana::cli::hardware_audit_failed(audit, false, true),
            "Leeres Hardware-Audit darf keinen Fehlermodus ausloesen.");

    katana::analysis::HardwareNaturalLoop unknown;
    unknown.classification = katana::analysis::HardwareLoopClassification::Unknown;
    audit.loops.push_back(unknown);
    audit.unresolved_poll_guard_loops =
        katana::analysis::count_unresolved_poll_guard_loops(audit.loops);
    require(audit.unresolved_poll_guard_loops == 0u &&
                !katana::cli::hardware_audit_failed(audit, false, true),
            "Reine Unknown-Loop ohne Read-/Guard-Evidenz wird im Strict-Modus hart gemacht.");

    auto overwritten_guard = unknown;
    overwritten_guard.unresolved_guard_access = true;
    overwritten_guard.accesses.push_back(
        {.kind = katana::analysis::HardwareAccessKind::Read});
    audit.loops.push_back(overwritten_guard);
    audit.unresolved_poll_guard_loops =
        katana::analysis::count_unresolved_poll_guard_loops(audit.loops);
    require(audit.unresolved_poll_guard_loops == 0u &&
                !katana::cli::hardware_audit_failed(audit, false, true),
            "Unabhaengiger Read wird durch pauschale Guard-Unsicherheit faelschlich hart.");

    auto unresolved_poll = unknown;
    unresolved_poll.unresolved_guard_access = true;
    unresolved_poll.unresolved_guard_read_instruction_addresses.push_back(0x100u);
    audit.loops.push_back(unresolved_poll);
    audit.unresolved_poll_guard_loops =
        katana::analysis::count_unresolved_poll_guard_loops(audit.loops);
    require(audit.unresolved_poll_guard_loops == 1u &&
                !katana::cli::hardware_audit_failed(audit, true, false) &&
                katana::cli::hardware_audit_failed(audit, false, true),
            "Ungeloeste Poll-/Guard-Loop ist nicht exklusiv im Strict-Modus hart.");
    const auto text = katana::analysis::format_hardware_audit_text(audit);
    const auto json = katana::analysis::format_hardware_audit_json(audit);
    require(text.find("Unresolved poll/guard loops: 1") != std::string::npos &&
                json.find("\"unresolved_poll_guard_loops\":1") != std::string::npos,
            "Hardware-Audit-Text oder JSON verliert die Strict-Loop-Metrik.");

    katana::analysis::DreamcastHardwareAudit definite_gap;
    definite_gap.known_gap_addresses = 1u;
    require(katana::cli::hardware_audit_failed(definite_gap, true, false) &&
                katana::cli::hardware_audit_failed(definite_gap, false, true),
            "Bekannte Hardwareluecke verliert den bisherigen Fehlermodus.");

    katana::analysis::DreamcastHardwareAudit partial;
    partial.partial_addresses = 1u;
    require(!katana::cli::hardware_audit_failed(partial, true, false) &&
                katana::cli::hardware_audit_failed(partial, false, true),
            "Partielle Hardwareunterstuetzung verliert den bisherigen Strict-Vertrag.");

    std::cout << "Hardware-Audit-Strict-Policy erfolgreich.\n";
}

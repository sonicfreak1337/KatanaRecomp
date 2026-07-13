#include "katana/sh4/isa_coverage.hpp"

#include "katana/sh4/decoder.hpp"
#include "katana/sh4/instruction_metadata.hpp"

#include <iomanip>
#include <map>
#include <sstream>

namespace katana::sh4 {
namespace {

struct MutableCoverage {
    std::string name;
    std::size_t rule_count = 0;
    std::uint32_t opcode_count = 0;
    bool privileged = false;
};

std::string special_kind_name(const InstructionKind kind) {
    switch (kind) {
        case InstructionKind::StoreSpecialRegister: return "StoreSpecialRegister";
        case InstructionKind::StoreSpecialRegisterPreDecrement: return "StoreSpecialRegisterPreDecrement";
        case InstructionKind::LoadSpecialRegister: return "LoadSpecialRegister";
        case InstructionKind::LoadSpecialRegisterPostIncrement: return "LoadSpecialRegisterPostIncrement";
        default: return "Unknown";
    }
}

}

IsaCoverageReport build_isa_coverage_report() {
    std::map<InstructionKind, MutableCoverage> entries;

    for (const auto& metadata : instruction_metadata()) {
        auto& entry = entries[metadata.kind];
        entry.name = std::string(metadata.name);
        ++entry.rule_count;
        entry.privileged = entry.privileged || metadata.is_privileged;
    }
    for (const auto& metadata : special_register_encoding_metadata()) {
        auto& entry = entries[metadata.kind];
        entry.name = special_kind_name(metadata.kind);
        ++entry.rule_count;
        entry.privileged = entry.privileged || metadata.is_privileged;
    }

    IsaCoverageReport report;
    for (std::uint32_t opcode = 0; opcode <= 0xFFFFu; ++opcode) {
        const auto decoded = decode(static_cast<std::uint16_t>(opcode));
        if (!decoded.is_known()) {
            ++report.unknown_opcode_count;
            continue;
        }
        ++report.known_opcode_count;
        ++entries[decoded.kind].opcode_count;
    }

    report.instructions.reserve(entries.size());
    for (const auto& [kind, entry] : entries) {
        report.instructions.push_back({
            kind,
            entry.name,
            entry.rule_count,
            entry.opcode_count,
            entry.privileged
        });
    }
    return report;
}

std::string format_isa_coverage_report(const IsaCoverageReport& report) {
    std::ostringstream output;
    output << "KatanaRecomp SH-4 ISA-Abdeckung\n"
           << "Name                                           Regeln  Opcodes  Privilegiert\n";
    for (const auto& entry : report.instructions) {
        output << std::left << std::setw(46) << entry.name
               << std::right << std::setw(7) << entry.encoding_rule_count
               << std::setw(9) << entry.decoded_opcode_count
               << std::setw(14) << (entry.contains_privileged_encoding ? "ja" : "nein")
               << '\n';
    }
    output << "Bekannte Opcodes:   " << report.known_opcode_count << '\n'
           << "Unbekannte Opcodes: " << report.unknown_opcode_count << '\n'
           << "Instruktionsarten:  " << report.instructions.size() << '\n';
    return output.str();
}

}

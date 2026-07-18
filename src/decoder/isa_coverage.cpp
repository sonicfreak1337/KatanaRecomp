#include "katana/sh4/isa_coverage.hpp"

#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/instruction_metadata.hpp"

#include "katana/io/json_report.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace katana::sh4 {
namespace {

struct MutableCoverage {
    std::string name;
    std::size_t rule_count = 0;
    std::uint32_t opcode_count = 0;
    bool privileged = false;
    ControlFlowKind control_flow = ControlFlowKind::None;
};

AlphaIsaLayerSupport supported_layers() {
    return {AlphaIsaSupport::Supported,
            AlphaIsaSupport::Supported,
            AlphaIsaSupport::Supported,
            AlphaIsaSupport::Supported};
}

AlphaIsaLayerSupport restricted_runtime_layers() {
    return {AlphaIsaSupport::Supported,
            AlphaIsaSupport::Supported,
            AlphaIsaSupport::Supported,
            AlphaIsaSupport::Restricted};
}

std::vector<AlphaIsaFamilyEntry> alpha_families() {
    using Support = AlphaIsaSupport;
    return {
        {"integer-core",
         "Integer arithmetic and logic",
         Support::Supported,
         supported_layers(),
         "Bit-exact 32-bit integer, T-bit and accumulator semantics through generated code.",
         "",
         "Decode, lower, emit and execute success plus wraparound and flag boundaries."},
        {"memory-transfer",
         "Memory transfers and addressing",
         Support::Supported,
         supported_layers(),
         "Width, sign extension, addressing update and typed memory exceptions are explicit.",
         "",
         "Success, alignment, region-boundary and delay-slot exception vectors."},
        {"control-flow",
         "Direct and indirect control flow",
         Support::Supported,
         supported_layers(),
         "Delay slots, PR, resolved targets and controlled unknown-target failure are explicit.",
         "",
         "Taken/not-taken, call/return, delay-slot and unknown-target vectors."},
        {"system-control",
         "Status, exception and system control",
         Support::Restricted,
         restricted_runtime_layers(),
         "SR/FPSCR transfers, traps and exception return preserve structured CPU state.",
         "Complete SLEEP wakeup and unimplemented privileged MMU/cache-control operations "
         "remain restricted; user-mode privilege violations trap before side effects.",
         "User/privileged mode, bank switch, exception, delay-slot and rejection vectors."},
        {"cache-store-queue",
         "Cache, prefetch and store queues",
         Support::Restricted,
         restricted_runtime_layers(),
         "PREF and store-queue transfers use explicit platform-service boundaries.",
         "Operand-cache RAM and complete cache coherency behavior are not enabled.",
         "RAM/TA queue success, invalid queue, cache invalidation and denied-profile vectors."},
        {"fpu",
         "Floating point",
         Support::Restricted,
         restricted_runtime_layers(),
         "FPSCR mode, register banks, scalar/vector operations and disabled-FPU faults are "
         "explicit.",
         "Complete SH-4 floating-point exception cause, enable and sticky-flag behavior is "
         "incomplete.",
         "Bit-pattern, rounding, DN, bank/mode, disabled-FPU and invalid-combination vectors."},
        {"unknown-opcode",
         "Unknown or unimplemented encodings",
         Support::Rejected,
         {},
         "Unknown 16-bit encodings are never decoded or executed as successful no-ops.",
         "No semantics are claimed until decoder, IR, backend and runtime contracts exist.",
         "Stable unknown-opcode diagnostic and non-success execution vector."}};
}

bool fpu_kind(const InstructionKind kind) {
    return kind >= InstructionKind::FmovRegister && kind <= InstructionKind::Fschg;
}

bool system_kind(const InstructionKind kind) {
    switch (kind) {
    case InstructionKind::StoreSpecialRegister:
    case InstructionKind::StoreSpecialRegisterPreDecrement:
    case InstructionKind::LoadSpecialRegister:
    case InstructionKind::LoadSpecialRegisterPostIncrement:
    case InstructionKind::TrapAlways:
    case InstructionKind::ReturnFromException:
    case InstructionKind::Sleep:
        return true;
    default:
        return false;
    }
}

bool memory_kind(const InstructionKind kind) {
    return kind >= InstructionKind::MovByteStore && kind <= InstructionKind::MoveAddressPcRelative;
}

std::string
family_id(const InstructionKind kind, const bool privileged, const ControlFlowKind control_flow) {
    if (fpu_kind(kind)) return "fpu";
    if (kind == InstructionKind::Prefetch || kind == InstructionKind::Ocbp ||
        kind == InstructionKind::Ocbwb)
        return "cache-store-queue";
    if (privileged || system_kind(kind)) return "system-control";
    if (control_flow != ControlFlowKind::None || kind == InstructionKind::Rts)
        return "control-flow";
    if (memory_kind(kind)) return "memory-transfer";
    return "integer-core";
}

const AlphaIsaFamilyEntry& family(const std::vector<AlphaIsaFamilyEntry>& families,
                                  const std::string& id) {
    const auto found = std::find_if(
        families.begin(), families.end(), [&](const auto& entry) { return entry.id == id; });
    if (found == families.end()) throw std::logic_error("Alpha-ISA-Familie fehlt.");
    return *found;
}

std::string special_kind_name(const InstructionKind kind) {
    switch (kind) {
    case InstructionKind::StoreSpecialRegister:
        return "StoreSpecialRegister";
    case InstructionKind::StoreSpecialRegisterPreDecrement:
        return "StoreSpecialRegisterPreDecrement";
    case InstructionKind::LoadSpecialRegister:
        return "LoadSpecialRegister";
    case InstructionKind::LoadSpecialRegisterPostIncrement:
        return "LoadSpecialRegisterPostIncrement";
    default:
        return "Unknown";
    }
}

} // namespace

AlphaIsaSupport alpha_isa_intersection(const AlphaIsaLayerSupport layers) noexcept {
    const std::array values{layers.decoder, layers.ir, layers.backend, layers.runtime};
    if (std::find(values.begin(), values.end(), AlphaIsaSupport::Rejected) != values.end())
        return AlphaIsaSupport::Rejected;
    if (std::find(values.begin(), values.end(), AlphaIsaSupport::Restricted) != values.end())
        return AlphaIsaSupport::Restricted;
    return AlphaIsaSupport::Supported;
}

IsaCoverageReport build_isa_coverage_report() {
    std::map<InstructionKind, MutableCoverage> entries;

    for (const auto& metadata : instruction_metadata()) {
        auto& entry = entries[metadata.kind];
        entry.name = std::string(metadata.name);
        ++entry.rule_count;
        entry.privileged = entry.privileged || metadata.is_privileged;
        if (metadata.control_flow != ControlFlowKind::None)
            entry.control_flow = metadata.control_flow;
    }
    for (const auto& metadata : special_register_encoding_metadata()) {
        auto& entry = entries[metadata.kind];
        entry.name = special_kind_name(metadata.kind);
        ++entry.rule_count;
        entry.privileged = entry.privileged || metadata.is_privileged;
    }

    IsaCoverageReport report;
    report.families = alpha_families();
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
        const auto id = family_id(kind, entry.privileged, entry.control_flow);
        const auto& contract = family(report.families, id);
        const auto operation = katana::ir::lowering_operation_for_instruction(kind);
        const AlphaIsaLayerSupport layers{
            entry.rule_count != 0u && entry.opcode_count != 0u ? AlphaIsaSupport::Supported
                                                               : AlphaIsaSupport::Rejected,
            operation != katana::ir::Operation::Unknown ? AlphaIsaSupport::Supported
                                                        : AlphaIsaSupport::Rejected,
            katana::codegen::cpp_backend_supports_operation(operation) ? AlphaIsaSupport::Supported
                                                                       : AlphaIsaSupport::Rejected,
            contract.layers.runtime};
        report.instructions.push_back({kind,
                                       entry.name,
                                       entry.rule_count,
                                       entry.opcode_count,
                                       entry.privileged,
                                       id,
                                       layers,
                                       alpha_isa_intersection(layers),
                                       contract.limitation,
                                       contract.test_requirement});
    }
    return report;
}

std::string format_isa_coverage_report(const IsaCoverageReport& report) {
    std::ostringstream output;
    output
        << "KatanaRecomp SH-4 ISA-Abdeckung\n"
        << "Name                              Familie              Status       Regeln  Opcodes\n";
    for (const auto& entry : report.instructions) {
        output << std::left << std::setw(34) << entry.name << std::setw(21) << entry.family_id
               << std::setw(13) << alpha_isa_support_name(entry.support) << std::right
               << std::setw(7) << entry.encoding_rule_count << std::setw(9)
               << entry.decoded_opcode_count << '\n';
    }
    output << "Bekannte Opcodes:   " << report.known_opcode_count << '\n'
           << "Unbekannte Opcodes: " << report.unknown_opcode_count << '\n'
           << "Instruktionsarten:  " << report.instructions.size() << '\n'
           << "Alpha-Vertrag:      " << alpha_isa_contract_version << "\n\n"
           << "Familien und Grenzen:\n";
    for (const auto& entry : report.families) {
        output << "- " << entry.id << ": " << alpha_isa_support_name(entry.support) << " - "
               << entry.semantic_contract;
        if (!entry.limitation.empty()) output << " Grenze: " << entry.limitation;
        output << " Tests: " << entry.test_requirement << '\n';
    }
    return output.str();
}

const char* alpha_isa_support_name(const AlphaIsaSupport support) noexcept {
    switch (support) {
    case AlphaIsaSupport::Supported:
        return "supported";
    case AlphaIsaSupport::Restricted:
        return "restricted";
    case AlphaIsaSupport::Rejected:
        return "rejected";
    }
    return "rejected";
}

std::string format_alpha_isa_json(const IsaCoverageReport& report) {
    std::ostringstream output;
    katana::io::write_json_report_header(output, "katana-alpha-isa", "alpha-isa");
    output << ",\"contract_version\":" << alpha_isa_contract_version
           << ",\"known_opcode_count\":" << report.known_opcode_count
           << ",\"unknown_opcode_count\":" << report.unknown_opcode_count << ",\"families\":[";
    for (std::size_t index = 0u; index < report.families.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& entry = report.families[index];
        output << "{\"id\":" << katana::io::quote_json(entry.id)
               << ",\"name\":" << katana::io::quote_json(entry.name)
               << ",\"status\":" << katana::io::quote_json(alpha_isa_support_name(entry.support))
               << ",\"layers\":{\"decoder\":"
               << katana::io::quote_json(alpha_isa_support_name(entry.layers.decoder))
               << ",\"ir\":" << katana::io::quote_json(alpha_isa_support_name(entry.layers.ir))
               << ",\"backend\":"
               << katana::io::quote_json(alpha_isa_support_name(entry.layers.backend))
               << ",\"runtime\":"
               << katana::io::quote_json(alpha_isa_support_name(entry.layers.runtime)) << '}'
               << ",\"semantic_contract\":" << katana::io::quote_json(entry.semantic_contract)
               << ",\"limitation\":" << katana::io::quote_json(entry.limitation)
               << ",\"test_requirement\":" << katana::io::quote_json(entry.test_requirement) << '}';
    }
    output << "],\"instructions\":[";
    for (std::size_t index = 0u; index < report.instructions.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& entry = report.instructions[index];
        output << "{\"name\":" << katana::io::quote_json(entry.name)
               << ",\"family\":" << katana::io::quote_json(entry.family_id)
               << ",\"status\":" << katana::io::quote_json(alpha_isa_support_name(entry.support))
               << ",\"layers\":{\"decoder\":"
               << katana::io::quote_json(alpha_isa_support_name(entry.layers.decoder))
               << ",\"ir\":" << katana::io::quote_json(alpha_isa_support_name(entry.layers.ir))
               << ",\"backend\":"
               << katana::io::quote_json(alpha_isa_support_name(entry.layers.backend))
               << ",\"runtime\":"
               << katana::io::quote_json(alpha_isa_support_name(entry.layers.runtime)) << '}'
               << ",\"encoding_rules\":" << entry.encoding_rule_count
               << ",\"decoded_opcodes\":" << entry.decoded_opcode_count
               << ",\"privileged\":" << (entry.contains_privileged_encoding ? "true" : "false")
               << ",\"limitation\":" << katana::io::quote_json(entry.limitation)
               << ",\"test_requirement\":" << katana::io::quote_json(entry.test_requirement) << '}';
    }
    output << "]}";
    return output.str();
}

} // namespace katana::sh4

#include "katana/analysis/control_flow_report.hpp"
#include "katana/io/json_report.hpp"

#include <iomanip>
#include <algorithm>
#include <set>
#include <sstream>
#include <vector>

namespace katana::analysis {
namespace {

void address(std::ostringstream& output, const std::uint32_t value) {
    output << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << value << std::dec;
}

const char* kind_name(const IndirectControlFlowKind kind) {
    return kind == IndirectControlFlowKind::Call ? "call" : "jump";
}

struct ReportLine {
    std::uint32_t address = 0u;
    std::string kind;
    std::uint64_t order_target = 0u;
    std::string text;
};

void sort_lines(std::vector<ReportLine>& lines) {
    std::sort(lines.begin(), lines.end(), [](const auto& left, const auto& right) {
        if (left.address != right.address) {
            return left.address < right.address;
        }
        if (left.kind != right.kind) {
            return left.kind < right.kind;
        }
        if (left.order_target != right.order_target) {
            return left.order_target < right.order_target;
        }
        return left.text < right.text;
    });
}

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << value;
    return output.str();
}

std::string hex16(const std::uint16_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(4)
           << std::setfill('0') << value;
    return output.str();
}

}

std::string format_indirect_control_flow_report(
    const std::span<const IndirectControlFlowResolution> resolutions,
    const std::span<const JumpTableAnalysis> jump_tables
) {
    std::ostringstream output;
    output << "\nIndirekter Kontrollfluss\n";
    std::set<std::uint32_t> table_dispatches;
    for (const auto& table : jump_tables) {
        table_dispatches.insert(table.dispatch_address);
    }
    std::vector<ReportLine> resolved_lines;
    std::vector<ReportLine> unresolved_lines;
    for (const auto& resolution : resolutions) {
        if (table_dispatches.contains(resolution.instruction_address)) {
            continue;
        }
        std::ostringstream line;
        if (resolution.status != ResolutionStatus::Resolved) {
            line << "  " << kind_name(resolution.kind) << ' ';
            address(line, resolution.instruction_address);
            line << " [" << resolution.reason << "] Hinweis: jump = ";
            address(line, resolution.instruction_address);
            line << " ZIEL\n";
            unresolved_lines.push_back({
                resolution.instruction_address, kind_name(resolution.kind), 0u, line.str()
            });
            continue;
        }
        line << "  " << kind_name(resolution.kind) << ' ';
        address(line, resolution.instruction_address);
        line << " -> ";
        address(line, *resolution.target);
        line << " [" << resolution.reason << "]\n";
        resolved_lines.push_back({
            resolution.instruction_address,
            kind_name(resolution.kind),
            *resolution.target,
            line.str()
        });
    }
    for (const auto& table : jump_tables) {
        const auto kind = table.dispatch_kind == JumpTableDispatchKind::Call
            ? "jump-table-call"
            : "jump-table-jump";
        if (table.resolved) {
            for (const auto& entry : table.entries) {
                std::ostringstream line;
                line << "  " << kind << ' ';
                address(line, table.dispatch_address);
                line << " -> ";
                address(line, entry.target);
                line << " [" << entry.reason << "]\n";
                resolved_lines.push_back({
                    table.dispatch_address, kind, entry.target, line.str()
                });
            }
            continue;
        }
        std::ostringstream line;
        line << "  " << kind << ' ';
        address(line, table.dispatch_address);
        line << " [" << table.reason << "] Hinweis: jump_table = ";
        address(line, table.dispatch_address);
        line << " TABELLE ANZAHL\n";
        unresolved_lines.push_back({table.dispatch_address, kind, 0u, line.str()});
    }
    sort_lines(resolved_lines);
    sort_lines(unresolved_lines);
    output << "Aufgeloest:\n";
    if (resolved_lines.empty()) {
        output << "  keine\n";
    } else {
        for (const auto& line : resolved_lines) {
            output << line.text;
        }
    }
    output << "Ungeloest:\n";
    if (unresolved_lines.empty()) {
        output << "  keine\n";
    } else {
        for (const auto& line : unresolved_lines) {
            output << line.text;
        }
    }
    return output.str();
}

std::string format_control_flow_analysis_json(
    const ControlFlowAnalysisResult& analysis
) {
    std::ostringstream output;
    katana::io::write_json_report_header(
        output, "katana-control-flow-v1", "control-flow"
    );
    output << ",\"summary\":{\"instructions\":"
           << analysis.recursive.instructions.size()
           << ",\"ranges\":" << analysis.recursive.ranges.size()
           << ",\"functions\":" << analysis.recursive.functions.size()
           << ",\"conflicts\":" << analysis.recursive.conflicts.size()
           << ",\"diagnostics\":" << analysis.recursive.diagnostics.size()
           << ",\"indirect_sites\":" << analysis.indirect_control_flow.size()
           << ",\"jump_tables\":" << analysis.jump_tables.size()
           << ",\"fixpoint_iterations\":" << analysis.fixpoint_iterations << '}';

    auto functions = analysis.recursive.functions;
    std::sort(functions.begin(), functions.end(), [](const auto& left, const auto& right) {
        return left.address < right.address;
    });
    output << ",\"functions\":[";
    for (std::size_t index = 0u; index < functions.size(); ++index) {
        if (index != 0u) output << ',';
        auto origins = functions[index].origins;
        std::sort(origins.begin(), origins.end());
        output << "{\"address\":" << katana::io::quote_json(hex32(functions[index].address))
               << ",\"confidence\":"
               << katana::io::quote_json(analysis_confidence_name(functions[index].confidence))
               << ",\"origins\":[";
        for (std::size_t origin = 0u; origin < origins.size(); ++origin) {
            if (origin != 0u) output << ',';
            output << katana::io::quote_json(function_origin_name(origins[origin]));
        }
        output << "]}";
    }
    output << ']';

    auto indirect = analysis.indirect_control_flow;
    std::sort(indirect.begin(), indirect.end(), [](const auto& left, const auto& right) {
        if (left.instruction_address != right.instruction_address) {
            return left.instruction_address < right.instruction_address;
        }
        return left.kind < right.kind;
    });
    output << ",\"indirect_control_flow\":[";
    for (std::size_t index = 0u; index < indirect.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& value = indirect[index];
        output << "{\"instruction_address\":"
               << katana::io::quote_json(hex32(value.instruction_address))
               << ",\"kind\":" << katana::io::quote_json(kind_name(value.kind))
               << ",\"register\":" << static_cast<unsigned>(value.register_index)
               << ",\"status\":" << katana::io::quote_json(
                    value.status == ResolutionStatus::Resolved ? "resolved" : "unresolved"
                  )
               << ",\"target\":";
        if (value.target) output << katana::io::quote_json(hex32(*value.target));
        else output << "null";
        output << ",\"reason\":" << katana::io::quote_json(value.reason) << '}';
    }
    output << ']';

    auto jump_tables = analysis.jump_tables;
    std::sort(jump_tables.begin(), jump_tables.end(), [](const auto& left, const auto& right) {
        return left.dispatch_address < right.dispatch_address;
    });
    output << ",\"jump_tables\":[";
    for (std::size_t index = 0u; index < jump_tables.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& table = jump_tables[index];
        output << "{\"dispatch_address\":"
               << katana::io::quote_json(hex32(table.dispatch_address))
               << ",\"table_address\":" << katana::io::quote_json(hex32(table.table_address))
               << ",\"kind\":" << katana::io::quote_json(
                    table.dispatch_kind == JumpTableDispatchKind::Call ? "call" : "jump"
                  )
               << ",\"resolved\":" << (table.resolved ? "true" : "false")
               << ",\"requested_entries\":" << table.requested_entries
               << ",\"reason\":" << katana::io::quote_json(table.reason) << '}';
    }
    output << ']';

    auto diagnostics = analysis.recursive.diagnostics;
    std::sort(diagnostics.begin(), diagnostics.end(), [](const auto& left, const auto& right) {
        return left.address < right.address;
    });
    output << ",\"diagnostics\":[";
    for (std::size_t index = 0u; index < diagnostics.size(); ++index) {
        if (index != 0u) output << ',';
        output << "{\"address\":" << katana::io::quote_json(hex32(diagnostics[index].address))
               << ",\"opcode\":" << katana::io::quote_json(hex16(diagnostics[index].opcode))
               << ",\"reason\":" << katana::io::quote_json(diagnostics[index].reason) << '}';
    }
    output << "]}\n";
    return output.str();
}

}

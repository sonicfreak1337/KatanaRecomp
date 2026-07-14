#include "katana/analysis/control_flow_report.hpp"

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

}

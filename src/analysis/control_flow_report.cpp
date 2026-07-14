#include "katana/analysis/control_flow_report.hpp"

#include <iomanip>
#include <sstream>

namespace katana::analysis {
namespace {

void address(std::ostringstream& output, const std::uint32_t value) {
    output << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << value << std::dec;
}

const char* kind_name(const IndirectControlFlowKind kind) {
    return kind == IndirectControlFlowKind::Call ? "call" : "jump";
}

}

std::string format_indirect_control_flow_report(
    const std::span<const IndirectControlFlowResolution> resolutions,
    const std::span<const JumpTableAnalysis> jump_tables
) {
    std::ostringstream output;
    output << "\nIndirekter Kontrollfluss\n";
    output << "Aufgeloest:\n";
    bool any_resolved = false;
    for (const auto& resolution : resolutions) {
        if (resolution.status != ResolutionStatus::Resolved) {
            continue;
        }
        any_resolved = true;
        output << "  " << kind_name(resolution.kind) << ' ';
        address(output, resolution.instruction_address);
        output << " -> ";
        address(output, *resolution.target);
        output << " [" << resolution.reason << "]\n";
    }
    for (const auto& table : jump_tables) {
        if (!table.resolved) {
            continue;
        }
        any_resolved = true;
        output << "  jump-table ";
        address(output, table.dispatch_address);
        output << " entries=" << table.entries.size() << " [" << table.reason << "]\n";
    }
    if (!any_resolved) {
        output << "  keine\n";
    }

    output << "Ungeloest:\n";
    bool any_unresolved = false;
    for (const auto& resolution : resolutions) {
        if (resolution.status != ResolutionStatus::Unresolved) {
            continue;
        }
        any_unresolved = true;
        output << "  " << kind_name(resolution.kind) << ' ';
        address(output, resolution.instruction_address);
        output << " [" << resolution.reason << "]"
               << " Hinweis: jump = ";
        address(output, resolution.instruction_address);
        output << " ZIEL\n";
    }
    for (const auto& table : jump_tables) {
        if (table.resolved) {
            continue;
        }
        any_unresolved = true;
        output << "  jump-table ";
        address(output, table.dispatch_address);
        output << " [" << table.reason << "]"
               << " Hinweis: jump_table = ";
        address(output, table.dispatch_address);
        output << " TABELLE ANZAHL\n";
    }
    if (!any_unresolved) {
        output << "  keine\n";
    }
    return output.str();
}

}

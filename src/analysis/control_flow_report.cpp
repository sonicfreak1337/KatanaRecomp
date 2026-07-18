#include "katana/analysis/control_flow_report.hpp"
#include "katana/io/json_report.hpp"

#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>
#include <vector>

namespace katana::analysis {
namespace {

void address(std::ostringstream& output, const std::uint32_t value) {
    output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value
           << std::dec;
}

void address_with_symbol(std::ostringstream& output,
                         const std::uint32_t value,
                         const std::span<const SymbolicAddress> symbols) {
    address(output, value);
    if (const auto* symbol = find_symbolic_address(symbols, value)) {
        output << " (" << format_symbolic_address(*symbol) << ')';
    }
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
    output << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return output.str();
}

std::string hex16(const std::uint16_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << value;
    return output.str();
}

const char* binding_name(const katana::io::SymbolBinding binding) noexcept {
    switch (binding) {
    case katana::io::SymbolBinding::Local:
        return "local";
    case katana::io::SymbolBinding::Global:
        return "global";
    case katana::io::SymbolBinding::Weak:
        return "weak";
    case katana::io::SymbolBinding::Unknown:
        return "unknown";
    }
    return "unknown";
}

void append_symbol_json(std::ostringstream& output,
                        const std::string_view key,
                        const std::span<const SymbolicAddress> symbols,
                        const std::uint32_t value) {
    output << ",\"" << key << "\":";
    const auto* symbol = find_symbolic_address(symbols, value);
    if (symbol == nullptr) {
        output << "null";
        return;
    }
    output << "{\"name\":" << katana::io::quote_json(symbol->name)
           << ",\"symbol_address\":" << katana::io::quote_json(hex32(symbol->symbol_address))
           << ",\"offset\":" << symbol->offset
           << ",\"exact\":" << (symbol->exact ? "true" : "false")
           << ",\"kind\":" << katana::io::quote_json(katana::io::symbol_kind_name(symbol->kind))
           << ",\"binding\":" << katana::io::quote_json(binding_name(symbol->binding)) << '}';
}

IndirectControlFlowOriginClass
effective_origin_class(const IndirectControlFlowResolution& resolution) noexcept {
    return resolution.origin_class == IndirectControlFlowOriginClass::NotApplicable
               ? IndirectControlFlowOriginClass::RuntimePointer
               : resolution.origin_class;
}

void count_frontier_class(ControlFlowFrontierClassSummary& summary,
                          const IndirectControlFlowResolution& resolution) noexcept {
    switch (effective_origin_class(resolution)) {
    case IndirectControlFlowOriginClass::Callback:
        ++summary.callback;
        break;
    case IndirectControlFlowOriginClass::Parameter:
        ++summary.parameter;
        break;
    case IndirectControlFlowOriginClass::Stack:
        ++summary.stack;
        break;
    case IndirectControlFlowOriginClass::ObjectVTable:
        ++summary.object_vtable;
        break;
    case IndirectControlFlowOriginClass::Table:
        ++summary.table;
        break;
    case IndirectControlFlowOriginClass::UnboundedMemory:
        ++summary.unbounded_memory;
        break;
    case IndirectControlFlowOriginClass::NotApplicable:
    case IndirectControlFlowOriginClass::RuntimePointer:
        ++summary.runtime_pointer;
        break;
    }
}

void append_frontier_summary_json(std::ostringstream& output,
                                  const ControlFlowReportSummary& summary) {
    output << "\"indirect_total\":" << summary.indirect_total
           << ",\"resolved\":" << summary.resolved
           << ",\"guarded_complete\":" << summary.guarded_complete
           << ",\"guarded_partial\":" << summary.guarded_partial
           << ",\"runtime_only\":" << summary.runtime_only
           << ",\"unresolved\":" << summary.unresolved
           << ",\"frontier_classes\":{\"callback\":" << summary.frontier_classes.callback
           << ",\"parameter\":" << summary.frontier_classes.parameter
           << ",\"stack\":" << summary.frontier_classes.stack
           << ",\"object_vtable\":" << summary.frontier_classes.object_vtable
           << ",\"table\":" << summary.frontier_classes.table
           << ",\"unbounded_memory\":" << summary.frontier_classes.unbounded_memory
           << ",\"runtime_pointer\":" << summary.frontier_classes.runtime_pointer << '}';
}

ControlFlowReportSummary
summarize_resolutions(const std::span<const IndirectControlFlowResolution> resolutions) noexcept {
    ControlFlowReportSummary summary;
    summary.indirect_total = resolutions.size();
    for (const auto& resolution : resolutions) {
        const auto status = control_flow_report_status(resolution);
        switch (status) {
        case ControlFlowReportStatus::Resolved:
            ++summary.resolved;
            break;
        case ControlFlowReportStatus::GuardedComplete:
            ++summary.guarded_complete;
            ++summary.guarded;
            break;
        case ControlFlowReportStatus::GuardedPartial:
            ++summary.guarded_partial;
            ++summary.guarded;
            count_frontier_class(summary.frontier_classes, resolution);
            break;
        case ControlFlowReportStatus::RuntimeOnly:
            ++summary.runtime_only;
            count_frontier_class(summary.frontier_classes, resolution);
            break;
        case ControlFlowReportStatus::Unresolved:
            ++summary.unresolved;
            count_frontier_class(summary.frontier_classes, resolution);
            break;
        }
    }
    summary.unresolved_frontier =
        summary.guarded_partial + summary.runtime_only + summary.unresolved;
    return summary;
}

} // namespace

ControlFlowReportStatus
control_flow_report_status(const IndirectControlFlowResolution& resolution) noexcept {
    if (resolution.evidence == ControlFlowEvidence::RuntimeOnly)
        return ControlFlowReportStatus::RuntimeOnly;
    if (resolution.evidence == ControlFlowEvidence::GuardedComplete)
        return ControlFlowReportStatus::GuardedComplete;
    if (resolution.status == ResolutionStatus::Resolved) return ControlFlowReportStatus::Resolved;
    if (resolution.status == ResolutionStatus::Guarded)
        return ControlFlowReportStatus::GuardedPartial;
    return ControlFlowReportStatus::Unresolved;
}

const char* control_flow_report_status_name(const ControlFlowReportStatus status) noexcept {
    switch (status) {
    case ControlFlowReportStatus::Resolved:
        return "resolved";
    case ControlFlowReportStatus::GuardedComplete:
        return "guarded_complete";
    case ControlFlowReportStatus::GuardedPartial:
        return "guarded_partial";
    case ControlFlowReportStatus::RuntimeOnly:
        return "runtime_only";
    case ControlFlowReportStatus::Unresolved:
        return "unresolved";
    }
    return "unresolved";
}

ControlFlowReportSummary
summarize_control_flow_analysis(const ControlFlowAnalysisResult& analysis) noexcept {
    auto summary = summarize_resolutions(analysis.indirect_control_flow);
    summary.proven_instructions = analysis.recursive.proven_instruction_addresses.size();
    summary.guarded_candidate_instructions =
        analysis.recursive.guarded_candidate_instruction_addresses.size();
    return summary;
}

std::string format_indirect_control_flow_report(
    const std::span<const IndirectControlFlowResolution> resolutions,
    const std::span<const JumpTableAnalysis> jump_tables,
    const std::span<const SymbolicAddress> symbols) {
    std::ostringstream output;
    output << "\nIndirekter Kontrollfluss\n";
    const auto summary = summarize_resolutions(resolutions);
    output << "Status: total=" << summary.indirect_total << "; resolved=" << summary.resolved
           << "; guarded_complete=" << summary.guarded_complete
           << "; guarded_partial=" << summary.guarded_partial
           << "; runtime_only=" << summary.runtime_only << "; unresolved=" << summary.unresolved
           << '\n'
           << "Frontklassen: callback=" << summary.frontier_classes.callback
           << "; parameter=" << summary.frontier_classes.parameter
           << "; stack=" << summary.frontier_classes.stack
           << "; object_vtable=" << summary.frontier_classes.object_vtable
           << "; table=" << summary.frontier_classes.table
           << "; unbounded_memory=" << summary.frontier_classes.unbounded_memory
           << "; runtime_pointer=" << summary.frontier_classes.runtime_pointer << '\n';
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
            address_with_symbol(line, resolution.instruction_address, symbols);
            line << " [" << resolution.reason;
            line << "; evidence=" << control_flow_evidence_name(resolution.evidence);
            line << "; status="
                 << control_flow_report_status_name(control_flow_report_status(resolution));
            line << "; class="
                 << indirect_control_flow_origin_class_name(effective_origin_class(resolution));
            if (resolution.target.has_value()) {
                line << "; candidate=";
                address(line, *resolution.target);
            }
            line << "] Hinweis: jump = ";
            address(line, resolution.instruction_address);
            line << " ZIEL\n";
            unresolved_lines.push_back(
                {resolution.instruction_address, kind_name(resolution.kind), 0u, line.str()});
            continue;
        }
        auto targets = resolution.targets;
        if (resolution.target.has_value()) targets.push_back(*resolution.target);
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (const auto target : targets) {
            std::ostringstream resolved;
            resolved << "  " << kind_name(resolution.kind) << ' ';
            address_with_symbol(resolved, resolution.instruction_address, symbols);
            resolved << " -> ";
            address_with_symbol(resolved, target, symbols);
            resolved << " [" << resolution.reason
                     << "; evidence=" << control_flow_evidence_name(resolution.evidence);
            if (!resolution.evidence_callees.empty()) {
                resolved << "; r" << static_cast<unsigned>(resolution.register_index)
                         << "; callees=";
                for (std::size_t index = 0u; index < resolution.evidence_callees.size(); ++index) {
                    if (index != 0u) resolved << ',';
                    address(resolved, resolution.evidence_callees[index]);
                }
            }
            resolved << "]\n";
            resolved_lines.push_back({resolution.instruction_address,
                                      kind_name(resolution.kind),
                                      target,
                                      resolved.str()});
        }
    }
    for (const auto& table : jump_tables) {
        const auto kind = table.dispatch_kind == JumpTableDispatchKind::Call ? "jump-table-call"
                                                                             : "jump-table-jump";
        if (table.resolved && control_flow_evidence_complete(table.evidence)) {
            for (const auto& entry : table.entries) {
                std::ostringstream line;
                line << "  " << kind << ' ';
                address_with_symbol(line, table.dispatch_address, symbols);
                line << " -> ";
                address_with_symbol(line, entry.target, symbols);
                line << " [" << entry.reason
                     << "; evidence=" << control_flow_evidence_name(table.evidence) << "]\n";
                resolved_lines.push_back({table.dispatch_address, kind, entry.target, line.str()});
            }
            continue;
        }
        std::ostringstream line;
        line << "  " << kind << ' ';
        address_with_symbol(line, table.dispatch_address, symbols);
        line << " [" << table.reason << "; evidence=" << control_flow_evidence_name(table.evidence)
             << "] Hinweis: jump_table = ";
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

std::string format_control_flow_analysis_json(const ControlFlowAnalysisResult& analysis) {
    std::ostringstream output;
    const auto summary = summarize_control_flow_analysis(analysis);
    katana::io::write_json_report_header(output, "katana-control-flow-v3", "control-flow");
    output << ",\"privacy\":\"local-detailed\""
           << ",\"summary\":{\"instructions\":" << analysis.recursive.instructions.size()
           << ",\"instruction_contexts\":" << analysis.recursive.contextual_instructions.size()
           << ",\"ranges\":" << analysis.recursive.ranges.size()
           << ",\"functions\":" << analysis.recursive.functions.size()
           << ",\"conflicts\":" << analysis.recursive.conflicts.size()
           << ",\"diagnostics\":" << analysis.recursive.diagnostics.size()
           << ",\"indirect_sites\":" << summary.indirect_total << ',';
    append_frontier_summary_json(output, summary);
    output << ",\"guarded\":" << summary.guarded
           << ",\"proven_instructions\":" << summary.proven_instructions
           << ",\"guarded_candidate_instructions\":" << summary.guarded_candidate_instructions
           << ",\"unresolved_frontier\":" << summary.unresolved_frontier
           << ",\"jump_tables\":" << analysis.jump_tables.size()
           << ",\"function_value_summaries\":" << analysis.function_value_summaries.size()
           << ",\"directive_diagnostics\":" << analysis.directive_diagnostics.size()
           << ",\"fixpoint_iterations\":" << analysis.fixpoint_iterations
           << ",\"recursive_processed_work_items\":" << analysis.recursive.processed_work_items
           << ",\"recursive_reused_contexts\":" << analysis.recursive.reused_contexts
           << ",\"function_summary_iterations\":" << analysis.function_summary_iterations
           << ",\"function_scc_count\":" << analysis.function_scc_count
           << ",\"unchanged_ingress_skips\":" << analysis.unchanged_ingress_skips
           << ",\"function_iteration_budget\":" << analysis.function_iteration_budget
           << ",\"function_budget_exhausted\":"
           << (analysis.function_budget_exhausted ? "true" : "false")
           << ",\"jump_table_cache_hits\":" << analysis.jump_table_cache.hits
           << ",\"jump_table_cache_misses\":" << analysis.jump_table_cache.misses
           << ",\"interned_evidence\":" << analysis.evidence_ids.size() << '}';

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
               << ",\"evidence\":"
               << katana::io::quote_json(control_flow_evidence_name(functions[index].evidence));
        append_symbol_json(output, "symbol", analysis.symbolic_addresses, functions[index].address);
        output << ",\"origins\":[";
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
               << katana::io::quote_json(hex32(value.instruction_address));
        append_symbol_json(
            output, "instruction_symbol", analysis.symbolic_addresses, value.instruction_address);
        output << ",\"kind\":" << katana::io::quote_json(kind_name(value.kind))
               << ",\"register\":" << static_cast<unsigned>(value.register_index) << ",\"status\":"
               << katana::io::quote_json(
                      control_flow_report_status_name(control_flow_report_status(value)))
               << ",\"origin_class\":";
        if (value.origin_class == IndirectControlFlowOriginClass::NotApplicable &&
            (control_flow_report_status(value) == ControlFlowReportStatus::Resolved ||
             control_flow_report_status(value) == ControlFlowReportStatus::GuardedComplete)) {
            output << "null";
        } else {
            output << katana::io::quote_json(
                indirect_control_flow_origin_class_name(effective_origin_class(value)));
        }
        output << ",\"evidence\":"
               << katana::io::quote_json(control_flow_evidence_name(value.evidence))
               << ",\"evidence_origins\":[";
        auto evidence_origins = value.evidence_origins;
        std::sort(evidence_origins.begin(), evidence_origins.end());
        evidence_origins.erase(std::unique(evidence_origins.begin(), evidence_origins.end()),
                               evidence_origins.end());
        for (std::size_t origin = 0u; origin < evidence_origins.size(); ++origin) {
            if (origin != 0u) output << ',';
            output << katana::io::quote_json(
                analysis_evidence_origin_name(evidence_origins[origin]));
        }
        output << "],\"target\":";
        if (value.target)
            output << katana::io::quote_json(hex32(*value.target));
        else
            output << "null";
        if (value.target) {
            append_symbol_json(output, "target_symbol", analysis.symbolic_addresses, *value.target);
        } else {
            output << ",\"target_symbol\":null";
        }
        output << ",\"targets\":[";
        for (std::size_t target = 0u; target < value.targets.size(); ++target) {
            if (target != 0u) output << ',';
            output << katana::io::quote_json(hex32(value.targets[target]));
        }
        output << "],\"evidence_call_sites\":[";
        for (std::size_t evidence = 0u; evidence < value.evidence_call_sites.size(); ++evidence) {
            if (evidence != 0u) output << ',';
            output << katana::io::quote_json(hex32(value.evidence_call_sites[evidence]));
        }
        output << "],\"evidence_callees\":[";
        for (std::size_t evidence = 0u; evidence < value.evidence_callees.size(); ++evidence) {
            if (evidence != 0u) output << ',';
            output << katana::io::quote_json(hex32(value.evidence_callees[evidence]));
        }
        output << "],\"reason\":" << katana::io::quote_json(value.reason) << '}';
    }
    output << ']';

    output << ",\"function_value_summaries\":[";
    for (std::size_t index = 0u; index < analysis.function_value_summaries.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& function_summary = analysis.function_value_summaries[index];
        output << "{\"function_address\":"
               << katana::io::quote_json(hex32(function_summary.function_address))
               << ",\"registers\":[";
        for (std::size_t reg = 0u; reg < function_summary.registers.size(); ++reg) {
            if (reg != 0u) output << ',';
            const auto& value = function_summary.registers[reg];
            output << "{\"register\":" << static_cast<unsigned>(value.register_index)
                   << ",\"complete\":" << (value.complete ? "true" : "false")
                   << ",\"guarded\":" << (value.guarded ? "true" : "false")
                   << ",\"abi_preserved\":" << (value.abi_preserved ? "true" : "false")
                   << ",\"reason\":" << katana::io::quote_json(value.reason) << ",\"values\":[";
            for (std::size_t item = 0u; item < value.values.size(); ++item) {
                if (item != 0u) output << ',';
                output << katana::io::quote_json(hex32(value.values[item]));
            }
            output << "],\"return_sites\":[";
            for (std::size_t item = 0u; item < value.return_sites.size(); ++item) {
                if (item != 0u) output << ',';
                output << katana::io::quote_json(hex32(value.return_sites[item]));
            }
            output << "],\"evidence_callees\":[";
            for (std::size_t item = 0u; item < value.evidence_callees.size(); ++item) {
                if (item != 0u) output << ',';
                output << katana::io::quote_json(hex32(value.evidence_callees[item]));
            }
            output << "]}";
        }
        output << "]}";
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
        output << "{\"dispatch_address\":" << katana::io::quote_json(hex32(table.dispatch_address))
               << ",\"table_address\":" << katana::io::quote_json(hex32(table.table_address))
               << ",\"encoding\":"
               << katana::io::quote_json(jump_table_encoding_name(table.encoding))
               << ",\"target_base\":";
        if (table.encoding == JumpTableEncoding::SignedRelative16)
            output << katana::io::quote_json(hex32(table.target_base));
        else
            output << "null";
        output << ",\"kind\":"
               << katana::io::quote_json(
                      table.dispatch_kind == JumpTableDispatchKind::Call ? "call" : "jump")
               << ",\"resolved\":" << (table.resolved ? "true" : "false") << ",\"evidence\":"
               << katana::io::quote_json(control_flow_evidence_name(table.evidence))
               << ",\"requested_entries\":" << table.requested_entries
               << ",\"reason\":" << katana::io::quote_json(table.reason) << ",\"entries\":[";
        for (std::size_t entry_index = 0u; entry_index < table.entries.size(); ++entry_index) {
            if (entry_index != 0u) output << ',';
            const auto& entry = table.entries[entry_index];
            output << "{\"index\":" << entry.index
                   << ",\"entry_address\":" << katana::io::quote_json(hex32(entry.entry_address))
                   << ",\"target\":" << katana::io::quote_json(hex32(entry.target))
                   << ",\"accepted\":" << (entry.accepted ? "true" : "false")
                   << ",\"reason\":" << katana::io::quote_json(entry.reason) << '}';
        }
        output << "]}";
    }
    output << ']';

    output << ",\"directive_diagnostics\":[";
    for (std::size_t index = 0u; index < analysis.directive_diagnostics.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& diagnostic = analysis.directive_diagnostics[index];
        output << "{\"line\":" << diagnostic.line
               << ",\"address\":" << katana::io::quote_json(hex32(diagnostic.address))
               << ",\"status\":"
               << katana::io::quote_json(
                      analysis_directive_diagnostic_status_name(diagnostic.status))
               << ",\"reason\":" << katana::io::quote_json(diagnostic.reason) << '}';
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
               << ",\"reason\":" << katana::io::quote_json(diagnostics[index].reason);
        append_symbol_json(
            output, "symbol", analysis.symbolic_addresses, diagnostics[index].address);
        output << '}';
    }
    output << "]}\n";
    return output.str();
}

std::string format_control_flow_frontier_json(const ControlFlowAnalysisResult& analysis) {
    std::ostringstream output;
    const auto summary = summarize_control_flow_analysis(analysis);
    katana::io::write_json_report_header(
        output, "katana-control-flow-frontier-v1", "control-flow-frontier");
    output << ",\"privacy\":\"aggregate\",\"summary\":{\"instructions\":"
           << analysis.recursive.instructions.size()
           << ",\"functions\":" << analysis.recursive.functions.size() << ',';
    append_frontier_summary_json(output, summary);
    output << "}}\n";
    return output.str();
}

} // namespace katana::analysis

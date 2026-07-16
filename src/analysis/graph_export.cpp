#include "katana/analysis/graph_export.hpp"

#include "katana/analysis/function_analysis.hpp"
#include "katana/io/json_report.hpp"

#include <algorithm>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace katana::analysis {
namespace {

std::string hex32(const std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << value;
    return output.str();
}

const katana::sh4::DisassemblyLine& control_line(const BasicBlock& block) {
    const auto last = block.lines.size() - 1u;
    return block.lines[last].is_delay_slot && last != 0u
        ? block.lines[last - 1u]
        : block.lines[last];
}

std::string symbol_for(
    const ControlFlowAnalysisResult& analysis,
    const std::uint32_t address
) {
    const auto* symbol = find_symbolic_address(analysis.symbolic_addresses, address);
    return symbol == nullptr ? std::string{} : format_symbolic_address(*symbol);
}

void sort_graph(AnalysisGraph& graph) {
    std::sort(graph.nodes.begin(), graph.nodes.end(), [](const auto& left, const auto& right) {
        if (left.address != right.address) return left.address < right.address;
        if (left.end_address != right.end_address) return left.end_address < right.end_address;
        return left.symbol < right.symbol;
    });
    for (std::size_t index = 1u; index < graph.nodes.size(); ++index) {
        if (graph.nodes[index - 1u].address == graph.nodes[index].address) {
            throw std::invalid_argument("Analysegraph enthaelt eine doppelte Knotenadresse.");
        }
    }
    std::sort(graph.edges.begin(), graph.edges.end(), [](const auto& left, const auto& right) {
        if (left.source != right.source) return left.source < right.source;
        if (left.callsite != right.callsite) return left.callsite < right.callsite;
        if (left.target != right.target) return left.target < right.target;
        return left.kind < right.kind;
    });
    graph.edges.erase(std::unique(graph.edges.begin(), graph.edges.end(), [](const auto& left, const auto& right) {
        return left.source == right.source && left.target == right.target
            && left.callsite == right.callsite && left.kind == right.kind;
    }), graph.edges.end());
}

std::string dot_escape(const std::string_view value) {
    std::string result;
    for (const auto character : value) {
        if (character == '"' || character == '\\') result.push_back('\\');
        if (character == '\n' || character == '\r') result += ' ';
        else result.push_back(character);
    }
    return result;
}

}

AnalysisGraph build_control_flow_graph(const ControlFlowAnalysisResult& analysis) {
    std::vector<std::uint32_t> leaders;
    leaders.reserve(analysis.recursive.functions.size());
    for (const auto& function : analysis.recursive.functions) leaders.push_back(function.address);
    const auto blocks = build_basic_blocks(
        analysis.recursive.instructions, analysis.resolved_edges, leaders
    );
    AnalysisGraph graph{AnalysisGraphKind::ControlFlow};
    graph.nodes.reserve(blocks.size());
    for (const auto& block : blocks) {
        graph.nodes.push_back({
            block.start_address, block.end_address, symbol_for(analysis, block.start_address)
        });
        if (block.lines.empty()) continue;
        const auto& control = control_line(block);
        for (const auto successor : block.successors) {
            auto kind = AnalysisGraphEdgeKind::Fallthrough;
            if (control.instruction.control_flow == katana::sh4::ControlFlowKind::ConditionalBranch) {
                kind = AnalysisGraphEdgeKind::Conditional;
            } else if (control.instruction.control_flow
                    == katana::sh4::ControlFlowKind::UnconditionalBranch) {
                kind = AnalysisGraphEdgeKind::Branch;
            }
            const auto resolved = std::find_if(
                analysis.resolved_edges.begin(), analysis.resolved_edges.end(),
                [&control, successor](const auto& edge) {
                    return edge.instruction_address == control.address
                        && edge.target_address == successor
                        && edge.kind == ResolvedControlFlowKind::Jump;
                }
            );
            if (resolved != analysis.resolved_edges.end()) {
                kind = AnalysisGraphEdgeKind::ResolvedIndirect;
            }
            graph.edges.push_back({block.start_address, successor, control.address, kind});
        }
        if (block.has_indirect_successor) {
            const auto kind = control.instruction.control_flow
                    == katana::sh4::ControlFlowKind::IndirectCall
                ? AnalysisGraphEdgeKind::UnresolvedCall
                : AnalysisGraphEdgeKind::UnresolvedJump;
            graph.edges.push_back({block.start_address, std::nullopt, control.address, kind});
        }
    }
    sort_graph(graph);
    return graph;
}

AnalysisGraph build_call_graph(const ControlFlowAnalysisResult& analysis) {
    std::vector<std::uint32_t> entries;
    entries.reserve(analysis.recursive.functions.size());
    for (const auto& function : analysis.recursive.functions) entries.push_back(function.address);
    const auto functions = discover_functions(
        analysis.recursive.instructions, entries, analysis.resolved_edges
    );
    AnalysisGraph graph{AnalysisGraphKind::CallGraph};
    graph.nodes.reserve(functions.size());
    std::set<std::uint32_t> known_entries;
    for (const auto& function : functions) {
        known_entries.insert(function.entry_address);
        graph.nodes.push_back({
            function.entry_address, function.entry_address,
            symbol_for(analysis, function.entry_address)
        });
    }
    for (const auto& function : functions) {
        for (const auto target : function.direct_callees) {
            const auto resolved_call = std::find_if(
                analysis.resolved_edges.begin(), analysis.resolved_edges.end(),
                [&function, target](const auto& edge) {
                    return edge.kind == ResolvedControlFlowKind::Call
                        && edge.target_address == target
                        && std::binary_search(
                            function.indirect_call_sites.begin(),
                            function.indirect_call_sites.end(),
                            edge.instruction_address
                        );
                }
            );
            graph.edges.push_back({
                function.entry_address,
                target,
                resolved_call == analysis.resolved_edges.end()
                    ? 0u
                    : resolved_call->instruction_address,
                resolved_call != analysis.resolved_edges.end()
                    ? AnalysisGraphEdgeKind::ResolvedIndirectCall
                    : AnalysisGraphEdgeKind::DirectCall
            });
            if (!known_entries.contains(target)) {
                graph.nodes.push_back({target, target, symbol_for(analysis, target)});
                known_entries.insert(target);
            }
        }
        for (const auto callsite : function.indirect_call_sites) {
            const bool resolved = std::any_of(
                analysis.resolved_edges.begin(), analysis.resolved_edges.end(),
                [callsite](const auto& edge) {
                    return edge.kind == ResolvedControlFlowKind::Call
                        && edge.instruction_address == callsite;
                }
            );
            if (!resolved) {
                graph.edges.push_back({
                    function.entry_address, std::nullopt, callsite,
                    AnalysisGraphEdgeKind::UnresolvedCall
                });
            }
        }
    }
    sort_graph(graph);
    return graph;
}

std::string serialize_analysis_graph_json(const AnalysisGraph& graph) {
    auto ordered = graph;
    sort_graph(ordered);
    std::ostringstream output;
    katana::io::write_json_report_header(output, "katana-analysis-graph", "analysis-graph");
    output << ",\"graph_version\":" << analysis_graph_schema_version
           << ",\"kind\":" << katana::io::quote_json(analysis_graph_kind_name(ordered.kind))
           << ",\"nodes\":[";
    for (std::size_t index = 0u; index < ordered.nodes.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& node = ordered.nodes[index];
        output << "{\"address\":" << katana::io::quote_json(hex32(node.address))
               << ",\"end_address\":" << katana::io::quote_json(hex32(node.end_address))
               << ",\"symbol\":";
        node.symbol.empty() ? output << "null" : output << katana::io::quote_json(node.symbol);
        output << '}';
    }
    output << "],\"edges\":[";
    for (std::size_t index = 0u; index < ordered.edges.size(); ++index) {
        if (index != 0u) output << ',';
        const auto& edge = ordered.edges[index];
        output << "{\"source\":" << katana::io::quote_json(hex32(edge.source))
               << ",\"target\":";
        edge.target.has_value()
            ? output << katana::io::quote_json(hex32(*edge.target))
            : output << "null";
        output << ",\"callsite\":" << katana::io::quote_json(hex32(edge.callsite))
               << ",\"kind\":"
               << katana::io::quote_json(analysis_graph_edge_kind_name(edge.kind)) << '}';
    }
    output << "]}";
    return output.str();
}

std::string serialize_analysis_graph_dot(const AnalysisGraph& graph) {
    auto ordered = graph;
    sort_graph(ordered);
    std::ostringstream output;
    output << "digraph " << analysis_graph_kind_name(ordered.kind) << " {\n";
    for (const auto& node : ordered.nodes) {
        output << "  n" << std::hex << std::uppercase << node.address << std::dec
               << " [label=\"" << hex32(node.address);
        if (!node.symbol.empty()) output << "\\n" << dot_escape(node.symbol);
        output << "\"];\n";
    }
    std::size_t unresolved = 0u;
    for (const auto& edge : ordered.edges) {
        output << "  n" << std::hex << std::uppercase << edge.source << std::dec << " -> ";
        std::optional<std::size_t> unresolved_index;
        if (edge.target.has_value()) {
            output << 'n' << std::hex << std::uppercase << *edge.target << std::dec;
        } else {
            unresolved_index = unresolved;
            output << "u" << unresolved;
            ++unresolved;
        }
        output << " [label=\"" << analysis_graph_edge_kind_name(edge.kind) << "\"];\n";
        if (unresolved_index.has_value()) {
            output << "  u" << *unresolved_index
                   << " [shape=diamond,label=\"unresolved\"];\n";
        }
    }
    output << "}\n";
    return output.str();
}

const char* analysis_graph_kind_name(const AnalysisGraphKind kind) noexcept {
    return kind == AnalysisGraphKind::ControlFlow ? "control_flow" : "call_graph";
}

const char* analysis_graph_edge_kind_name(const AnalysisGraphEdgeKind kind) noexcept {
    switch (kind) {
        case AnalysisGraphEdgeKind::Fallthrough: return "fallthrough";
        case AnalysisGraphEdgeKind::Branch: return "branch";
        case AnalysisGraphEdgeKind::Conditional: return "conditional";
        case AnalysisGraphEdgeKind::ResolvedIndirect: return "resolved-indirect";
        case AnalysisGraphEdgeKind::DirectCall: return "direct-call";
        case AnalysisGraphEdgeKind::ResolvedIndirectCall: return "resolved-indirect-call";
        case AnalysisGraphEdgeKind::UnresolvedJump: return "unresolved-jump";
        case AnalysisGraphEdgeKind::UnresolvedCall: return "unresolved-call";
    }
    return "unknown";
}

}

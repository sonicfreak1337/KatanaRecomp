#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/graph_export.hpp"
#include "katana/io/executable_image.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    using namespace katana::analysis;

    AnalysisGraph graph;
    graph.kind = AnalysisGraphKind::ControlFlow;
    graph.nodes = {
        {0x3000u, 0x3002u, "callee\"name"}, {0x1000u, 0x1004u, "entry"}, {0x2000u, 0x2002u, {}}};
    graph.edges = {{0x1000u, 0x2000u, 0x1002u, AnalysisGraphEdgeKind::Conditional},
                   {0x1000u, 0x2000u, 0x1002u, AnalysisGraphEdgeKind::Conditional},
                   {0x2000u, std::nullopt, 0x2000u, AnalysisGraphEdgeKind::UnresolvedJump},
                   {0x1000u, 0x3000u, 0x1000u, AnalysisGraphEdgeKind::DirectCall},
                   {0x1000u, 0x3000u, 0x1000u, AnalysisGraphEdgeKind::ResolvedIndirectCall},
                   {0x1000u, 0x3000u, 0x1002u, AnalysisGraphEdgeKind::GuardedIndirectCall},
                   {0x2000u, 0x3000u, 0x2000u, AnalysisGraphEdgeKind::ResolvedIndirect},
                   {0x2000u, 0x3000u, 0x2000u, AnalysisGraphEdgeKind::GuardedIndirect},
                   {0x2000u, 0x3000u, 0x2002u, AnalysisGraphEdgeKind::Branch},
                   {0x2000u, 0x2002u, 0x2000u, AnalysisGraphEdgeKind::Fallthrough},
                   {0x3000u, std::nullopt, 0x3000u, AnalysisGraphEdgeKind::UnresolvedCall}};
    const auto json = serialize_analysis_graph_json(graph);
    const auto dot = serialize_analysis_graph_dot(graph);
    require(json == serialize_analysis_graph_json(graph) &&
                dot == serialize_analysis_graph_dot(graph),
            "Gleicher Analysegraph ist nicht bytegleich reproduzierbar.");
    require(json.find("\"graph_version\":1") != std::string::npos &&
                json.find("\"symbol\":\"callee\\\"name\"") != std::string::npos &&
                json.find("\"target\":null") != std::string::npos &&
                json.find("resolved-indirect-call") != std::string::npos &&
                json.find("guarded-indirect-call") != std::string::npos &&
                json.find("guarded-indirect") != std::string::npos &&
                json.find("unresolved-jump") != std::string::npos &&
                json.find("unresolved-call") != std::string::npos,
            "Graph-JSON verliert Schema, Symbole, indirekte oder unaufgeloeste Kanten.");
    const auto first_conditional = json.find("\"kind\":\"conditional\"");
    require(first_conditional != std::string::npos &&
                json.find("\"kind\":\"conditional\"", first_conditional + 1u) == std::string::npos,
            "Doppelte Graphkante wurde nicht dedupliziert.");
    require(json.find("0x00001000") < json.find("0x00002000") &&
                json.find("0x00002000") < json.find("0x00003000") &&
                dot.find("shape=diamond,label=\"unresolved\"") != std::string::npos,
            "Graphknoten sind nicht sortiert oder DOT verdeckt unaufgeloeste Ziele.");

    katana::io::ExecutableImage image;
    image.add_segment({"text",
                       0x1000u,
                       0u,
                       0x14u,
                       katana::io::SegmentKind::Code,
                       {true, false, true},
                       {
                           0x02u, 0xB0u, // bsr 0x1008
                           0x09u, 0x00u, // delay-slot nop
                           0x0Bu, 0x00u, // rts
                           0x09u, 0x00u, // delay-slot nop
                           0x0Bu, 0x00u, // callee rts
                           0x09u, 0x00u, // delay-slot nop
                           0x0Bu, 0x40u, // jsr @r0
                           0x09u, 0x00u, // delay-slot nop
                           0x0Bu, 0x00u, // rts
                           0x09u, 0x00u  // delay-slot nop
                       }});
    image.add_entry_point(0x1000u);
    image.add_entry_point(0x100Cu);
    image.add_symbol({"entry",
                      0x1000u,
                      8u,
                      katana::io::SymbolKind::Function,
                      katana::io::SymbolBinding::Global});
    const auto analysis = analyze_control_flow(image);
    const auto cfg = build_control_flow_graph(analysis);
    const auto calls = build_call_graph(analysis);
    require(!cfg.nodes.empty() && !calls.nodes.empty() &&
                std::any_of(calls.edges.begin(),
                            calls.edges.end(),
                            [](const auto& edge) {
                                return edge.kind == AnalysisGraphEdgeKind::DirectCall;
                            }) &&
                std::any_of(calls.edges.begin(),
                            calls.edges.end(),
                            [](const auto& edge) {
                                return edge.kind == AnalysisGraphEdgeKind::UnresolvedCall;
                            }),
            "CFG/Callgraph werden nicht aus derselben Analyse samt direktem und offenem Call "
            "abgeleitet.");

    std::cout << "KR-3605 CFG- und Callgraph-Export erfolgreich.\n";
    return EXIT_SUCCESS;
}

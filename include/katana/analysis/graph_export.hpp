#pragma once

#include "katana/analysis/control_flow_analysis.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace katana::analysis {

inline constexpr std::uint32_t analysis_graph_schema_version = 1u;

enum class AnalysisGraphKind : std::uint8_t { ControlFlow, CallGraph };
enum class AnalysisGraphEdgeKind : std::uint8_t {
    Fallthrough,
    Branch,
    Conditional,
    ResolvedIndirect,
    DirectCall,
    ResolvedIndirectCall,
    UnresolvedJump,
    UnresolvedCall
};

struct AnalysisGraphNode {
    std::uint32_t address = 0u;
    std::uint32_t end_address = 0u;
    std::string symbol;
};

struct AnalysisGraphEdge {
    std::uint32_t source = 0u;
    std::optional<std::uint32_t> target;
    std::uint32_t callsite = 0u;
    AnalysisGraphEdgeKind kind = AnalysisGraphEdgeKind::Fallthrough;
};

struct AnalysisGraph {
    AnalysisGraphKind kind = AnalysisGraphKind::ControlFlow;
    std::vector<AnalysisGraphNode> nodes;
    std::vector<AnalysisGraphEdge> edges;
};

[[nodiscard]] AnalysisGraph build_control_flow_graph(const ControlFlowAnalysisResult& analysis);
[[nodiscard]] AnalysisGraph build_call_graph(const ControlFlowAnalysisResult& analysis);
[[nodiscard]] std::string serialize_analysis_graph_json(const AnalysisGraph& graph);
[[nodiscard]] std::string serialize_analysis_graph_dot(const AnalysisGraph& graph);
[[nodiscard]] const char* analysis_graph_kind_name(AnalysisGraphKind kind) noexcept;
[[nodiscard]] const char* analysis_graph_edge_kind_name(AnalysisGraphEdgeKind kind) noexcept;

} // namespace katana::analysis

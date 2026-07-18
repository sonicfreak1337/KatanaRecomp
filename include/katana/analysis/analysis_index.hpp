#pragma once

#include "katana/analysis/basic_blocks.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/io/executable_image.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace katana::analysis {

using EvidenceId = std::uint32_t;
inline constexpr EvidenceId invalid_evidence_id = 0u;

class EvidenceInterner final {
  public:
    [[nodiscard]] EvidenceId intern(std::string_view text);
    [[nodiscard]] std::string_view resolve(EvidenceId id) const;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    std::unordered_map<std::string, EvidenceId> ids_;
    std::vector<std::string> strings_;
};

class InstructionArena final {
  public:
    explicit InstructionArena(std::vector<katana::sh4::DisassemblyLine> instructions);

    [[nodiscard]] std::span<const katana::sh4::DisassemblyLine> instructions() const noexcept;
    [[nodiscard]] const katana::sh4::DisassemblyLine* find(std::uint32_t address) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    std::shared_ptr<const std::vector<katana::sh4::DisassemblyLine>> instructions_;
    std::unordered_map<std::uint32_t, std::size_t> by_address_;
};

struct InstructionSpan {
    std::size_t first = 0u;
    std::size_t count = 0u;

    [[nodiscard]] std::span<const katana::sh4::DisassemblyLine>
    view(const InstructionArena& arena) const;
    [[nodiscard]] bool operator==(const InstructionSpan&) const = default;
};

[[nodiscard]] std::vector<InstructionSpan>
build_block_spans(const InstructionArena& arena, std::span<const BasicBlock> blocks);

class AnalysisIndex final {
  public:
    AnalysisIndex(const InstructionArena& arena,
                  std::span<const BasicBlock> blocks,
                  std::span<const ResolvedControlFlowEdge> edges,
                  std::span<const FunctionInfo> functions,
                  std::span<const katana::io::ImageSegment> segments);

    [[nodiscard]] std::optional<std::size_t> instruction(std::uint32_t address) const noexcept;
    [[nodiscard]] std::optional<std::size_t> block(std::uint32_t start) const noexcept;
    [[nodiscard]] std::span<const std::size_t> edges(std::uint32_t site) const noexcept;
    [[nodiscard]] std::optional<std::size_t> function(std::uint32_t entry) const noexcept;
    [[nodiscard]] std::optional<std::size_t> segment(std::uint32_t address) const noexcept;

  private:
    std::unordered_map<std::uint32_t, std::size_t> instructions_;
    std::unordered_map<std::uint32_t, std::size_t> blocks_;
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> edges_;
    std::unordered_map<std::uint32_t, std::size_t> functions_;
    std::vector<std::tuple<std::uint32_t, std::uint64_t, std::size_t>> segments_;
};

} // namespace katana::analysis

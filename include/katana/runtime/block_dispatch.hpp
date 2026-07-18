#pragma once

#include "katana/runtime/indirect_dispatch.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace katana::runtime {

struct BlockEndDefinition {
    BlockEndKind kind = BlockEndKind::Fallthrough;
    BlockAddress source;
    std::vector<BlockAddress> direct_successors;
    std::optional<BlockAddress> fallthrough;
    std::optional<std::uint32_t> callsite;
};

struct BlockDispatchOutcome {
    BlockExit exit;
    std::optional<RuntimeBlockHandle> target_block;
    bool direct_link = false;
};

class CanonicalBlockDispatcher {
  public:
    explicit CanonicalBlockDispatcher(const RuntimeBlockTable& table,
                                      DispatchDiagnosticRecorder* diagnostics = nullptr);
    [[nodiscard]] BlockDispatchOutcome
    dispatch(CpuState& cpu,
             BlockExecutionContext& context,
             const BlockVariantKey& variant,
             const BlockEndDefinition& end,
             bool condition = false,
             std::optional<std::uint32_t> dynamic_target = std::nullopt);
    void link(std::string source_identity, std::string target_identity);
    [[nodiscard]] std::vector<std::string> unlink_target(const std::string& target_identity);
    [[nodiscard]] std::size_t
    incoming_link_count(const std::string& target_identity) const noexcept;

  private:
    [[nodiscard]] RuntimeBlockHandle lookup(BlockAddress address,
                                            const BlockVariantKey& variant) const;
    const RuntimeBlockTable& table_;
    DispatchDiagnosticRecorder* diagnostics_ = nullptr;
    std::map<std::string, std::set<std::string>> incoming_;
};

} // namespace katana::runtime

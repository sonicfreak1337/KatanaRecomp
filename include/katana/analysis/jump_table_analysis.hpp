#pragma once

#include "katana/analysis/evidence.hpp"
#include "katana/io/executable_image.hpp"
#include "katana/sh4/disassembler.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace katana::analysis {

struct JumpTableEntry {
    std::size_t index = 0u;
    std::uint32_t entry_address = 0u;
    std::uint32_t target = 0u;
    bool accepted = false;
    std::string reason;
};

enum class JumpTableDispatchKind { Jump, Call };

enum class JumpTableEncoding { Absolute32, SignedRelative16 };

struct JumpTableAnalysis {
    std::uint32_t dispatch_address = 0u;
    std::uint32_t table_address = 0u;
    std::uint32_t target_base = 0u;
    std::size_t requested_entries = 0u;
    JumpTableDispatchKind dispatch_kind = JumpTableDispatchKind::Jump;
    JumpTableEncoding encoding = JumpTableEncoding::Absolute32;
    bool resolved = false;
    ControlFlowEvidence evidence = ControlFlowEvidence::Unresolved;
    std::vector<JumpTableEntry> entries;
    std::string reason;
};

struct JumpTableCacheCounters {
    std::uint64_t hits = 0u;
    std::uint64_t misses = 0u;
    std::uint64_t evictions = 0u;
};

class JumpTableSnapshotCache final {
  public:
    explicit JumpTableSnapshotCache(std::size_t capacity = 128u);
    [[nodiscard]] std::optional<JumpTableAnalysis> load(std::string_view key);
    void store(std::string key, JumpTableAnalysis analysis);
    [[nodiscard]] const JumpTableCacheCounters& counters() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    std::size_t capacity_ = 0u;
    std::vector<std::string> order_;
    std::unordered_map<std::string, JumpTableAnalysis> entries_;
    JumpTableCacheCounters counters_;
};

[[nodiscard]] JumpTableAnalysis analyze_jump_table(const katana::io::ExecutableImage& image,
                                                   std::uint32_t dispatch_address,
                                                   std::uint32_t table_address,
                                                   std::size_t entry_count,
                                                   JumpTableSnapshotCache* cache = nullptr);

[[nodiscard]] JumpTableAnalysis
analyze_relative_jump_table(const katana::io::ExecutableImage& image,
                            std::uint32_t dispatch_address,
                            std::uint32_t table_address,
                            std::uint32_t target_base,
                            std::size_t entry_count,
                            JumpTableSnapshotCache* cache = nullptr);

[[nodiscard]] std::optional<JumpTableAnalysis>
recognize_bounded_relative_jump_table(const katana::io::ExecutableImage& image,
                                      std::span<const katana::sh4::DisassemblyLine> lines,
                                      std::size_t dispatch_index,
                                      JumpTableSnapshotCache* cache = nullptr);

[[nodiscard]] const char* jump_table_encoding_name(JumpTableEncoding encoding) noexcept;

} // namespace katana::analysis

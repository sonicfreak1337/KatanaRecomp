#pragma once

#include "katana/io/executable_image.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
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

struct JumpTableAnalysis {
    std::uint32_t dispatch_address = 0u;
    std::uint32_t table_address = 0u;
    std::size_t requested_entries = 0u;
    JumpTableDispatchKind dispatch_kind = JumpTableDispatchKind::Jump;
    bool resolved = false;
    std::vector<JumpTableEntry> entries;
    std::string reason;
};

[[nodiscard]] JumpTableAnalysis analyze_jump_table(const katana::io::ExecutableImage& image,
                                                   std::uint32_t dispatch_address,
                                                   std::uint32_t table_address,
                                                   std::size_t entry_count);

} // namespace katana::analysis

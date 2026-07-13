#pragma once

#include "katana/sh4/disassembler.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace katana::analysis {

struct FunctionInfo {
    std::size_t id = 0;
    std::uint32_t entry_address = 0;

    std::vector<std::uint32_t> block_addresses;
    std::vector<std::uint32_t> direct_callees;
    std::vector<std::uint32_t> indirect_call_sites;
};

[[nodiscard]] std::vector<FunctionInfo> discover_functions(
    std::span<const katana::sh4::DisassemblyLine> lines,
    std::span<const std::uint32_t> seed_entries
);

}

#pragma once

#include "katana/codegen/project.hpp"
#include "katana/runtime/block_abi.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace katana::codegen {

inline constexpr std::uint32_t block_metadata_schema_version = 1u;

enum class BlockProvenance : std::uint8_t {
    ImageSegment,
    RomToRamCopy,
    FallbackDecode,
    RuntimeWrite
};

enum class BlockStateGuard : std::uint32_t {
    None = 0u,
    Mmu = 1u << 0u,
    Fpscr = 1u << 1u,
    AddressSpace = 1u << 2u,
    Watchpoint = 1u << 3u
};

using BlockStateGuards = std::uint32_t;

[[nodiscard]] constexpr BlockStateGuards block_state_guard(
    const BlockStateGuard guard
) noexcept {
    return static_cast<BlockStateGuards>(guard);
}

struct BlockMetadata {
    katana::runtime::BlockAddress address;
    std::string source_segment;
    std::uint64_t source_byte_offset = 0u;
    std::uint32_t byte_size = 0u;
    BlockProvenance provenance = BlockProvenance::ImageSegment;
    std::vector<std::uint16_t> guest_opcodes;
    std::uint64_t estimated_guest_cycles = 0u;
    katana::runtime::BlockEndKind end_kind = katana::runtime::BlockEndKind::Fallthrough;
    std::vector<katana::runtime::BlockAddress> direct_successors;
    BlockStateGuards state_guards = block_state_guard(BlockStateGuard::None);
};

[[nodiscard]] std::string serialize_block_metadata(
    std::span<const BlockMetadata> blocks,
    std::string_view backend_name,
    std::uint32_t backend_abi
);

[[nodiscard]] std::vector<ProjectArtifact> make_separated_codegen_artifacts(
    std::vector<ProjectArtifact> code_units,
    std::string constants,
    std::string symbols,
    std::string runtime_metadata
);

} // namespace katana::codegen

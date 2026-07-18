#pragma once

#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/io/executable_image.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace katana::analysis {

enum class ExecutableByteClass : std::uint8_t {
    ProvenReachableCode,
    RuntimeDiscoveredCode,
    UnreachableDecodableCode,
    EmbeddedData,
    LiteralPool,
    JumpTable,
    PointerTable,
    Padding,
    OverlayCandidate,
    ModuleCandidate,
    CompressedOrEncoded,
    UnknownExecutable,
    Count
};

enum class MixedRangeRole : std::uint8_t {
    ProvenCode,
    ReachableCode,
    RuntimeMaterializableCode,
    LiteralPool,
    JumpTable,
    PointerTable,
    ReadOnlyData,
    WritableData,
    Padding,
    CompressedOrEncoded,
    ModulePayload,
    Unknown,
    Count
};

enum class RangeProofClass : std::uint8_t { Proven, Guarded, Candidate, Unknown, Count };

enum class PrecompileClass : std::uint8_t {
    InitiallyReachable,
    StaticallyDiscoverable,
    LoadableModule,
    RuntimeMaterializable,
    NeverExecutedData,
    Unknown,
    Count
};

constexpr std::size_t executable_byte_class_count =
    static_cast<std::size_t>(ExecutableByteClass::Count);
constexpr std::size_t precompile_class_count = static_cast<std::size_t>(PrecompileClass::Count);
constexpr std::size_t mixed_range_role_count = static_cast<std::size_t>(MixedRangeRole::Count);
constexpr std::size_t range_proof_class_count = static_cast<std::size_t>(RangeProofClass::Count);

struct ExecutableInventoryRange {
    std::size_t segment_index = 0u;
    std::uint32_t address = 0u;
    std::uint64_t file_offset = 0u;
    std::uint64_t size = 0u;
    ExecutableByteClass byte_class = ExecutableByteClass::UnknownExecutable;
    PrecompileClass precompile_class = PrecompileClass::Unknown;
    MixedRangeRole role = MixedRangeRole::Unknown;
    RangeProofClass proof = RangeProofClass::Unknown;
    bool writable = false;
    std::uint64_t static_references = 0u;
    std::uint64_t runtime_writes = 0u;
    std::uint64_t relocations = 0u;
    std::uint64_t potential_control_flow_targets = 0u;
    std::string reason;
};

struct ExecutableInventoryPage {
    std::size_t segment_index = 0u;
    std::uint32_t address = 0u;
    std::uint64_t size = 0u;
    bool writable = false;
    MixedRangeRole dominant_role = MixedRangeRole::Unknown;
    RangeProofClass strongest_proof = RangeProofClass::Unknown;
    std::uint64_t dominant_role_bytes = 0u;
    std::uint64_t unknown_bytes = 0u;
    std::uint64_t static_references = 0u;
    std::uint64_t runtime_writes = 0u;
    std::uint64_t relocations = 0u;
    std::uint64_t potential_control_flow_targets = 0u;
    std::uint32_t decode_density_ppm = 0u;
    std::uint32_t entropy_millibits = 0u;
    std::uint64_t distance_to_proven_code = 0u;
};

struct ExecutableInventoryGroup {
    std::size_t segment_index = 0u;
    io::ImageSourceKind source_kind = io::ImageSourceKind::Unknown;
    io::ImageLoadPhase load_phase = io::ImageLoadPhase::Initial;
    bool writable = false;
    std::uint64_t committed_bytes = 0u;
    std::uint64_t zero_fill_bytes = 0u;
    std::array<std::uint64_t, executable_byte_class_count> byte_counts{};
};

struct ExecutableByteInventory {
    std::uint64_t committed_executable_bytes = 0u;
    std::array<std::uint64_t, executable_byte_class_count> byte_counts{};
    std::array<std::uint64_t, precompile_class_count> precompile_counts{};
    std::array<std::uint64_t, mixed_range_role_count> role_counts{};
    std::array<std::uint64_t, range_proof_class_count> proof_counts{};
    std::vector<ExecutableInventoryGroup> groups;
    std::vector<ExecutableInventoryRange> ranges;
    std::vector<ExecutableInventoryPage> pages;
};

[[nodiscard]] ExecutableByteInventory
build_executable_byte_inventory(const io::ExecutableImage& image,
                                const ControlFlowAnalysisResult& analysis);
[[nodiscard]] std::string format_executable_inventory_json(const io::ExecutableImage& image,
                                                           const ExecutableByteInventory& inventory,
                                                           bool include_local_details);
[[nodiscard]] const char* executable_byte_class_name(ExecutableByteClass value) noexcept;
[[nodiscard]] const char* precompile_class_name(PrecompileClass value) noexcept;
[[nodiscard]] const char* mixed_range_role_name(MixedRangeRole value) noexcept;
[[nodiscard]] const char* range_proof_class_name(RangeProofClass value) noexcept;

} // namespace katana::analysis

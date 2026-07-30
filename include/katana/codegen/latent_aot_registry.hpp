#pragma once

#include "katana/ir/ir.hpp"
#include "katana/runtime/disc.hpp"
#include "katana/runtime/native_aot_template.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace katana::codegen {

// A local, export-time-only request for a native entry in an exact disc module.
// The offset is meaningful only for the exact byte identity and logical disc
// location; it is never inferred from mutable runtime memory.
struct LatentAotEntryHint {
    std::string byte_identity;
    std::uint64_t disc_byte_offset = 0u;
    std::uint32_t byte_size = 0u;
    std::uint32_t module_relative_offset = 0u;

    [[nodiscard]] bool operator==(const LatentAotEntryHint&) const = default;
};

struct LatentAotDiscoveryOptions {
    std::size_t maximum_directory_entries = 4096u;
    std::size_t maximum_directory_bytes = 4u * 1024u * 1024u;
    std::size_t maximum_total_directory_bytes = 16u * 1024u * 1024u;
    std::size_t maximum_candidate_files = 128u;
    std::size_t maximum_file_bytes =
        katana::runtime::maximum_native_aot_template_extent;
    std::size_t maximum_total_file_bytes = 64u * 1024u * 1024u;
    std::size_t maximum_workers = 12u;
    std::size_t maximum_entry_scan_instructions = 1024u;
    std::size_t maximum_native_instructions_per_module = 32768u;
    std::size_t maximum_blocks_per_module = 8192u;
    std::size_t maximum_functions_per_module = 2048u;
    std::size_t maximum_analysis_iterations = 64u;
    std::size_t maximum_analysis_contexts = 65536u;
    std::uint32_t source_address_begin = 0x88000000u;
    std::uint32_t source_address_end = 0x8C000000u;
};

struct LatentAotOccupiedRange {
    std::uint32_t start = 0u;
    std::uint64_t size = 0u;

    [[nodiscard]] bool operator==(const LatentAotOccupiedRange&) const = default;
};

struct PreparedLatentAotBlockIdentity {
    std::uint32_t source_offset = 0u;
    std::uint32_t size = 0u;
    std::string sha256;

    [[nodiscard]] bool operator==(const PreparedLatentAotBlockIdentity&) const = default;
};

// Export-time description of a disc file whose finite native SH-4 graph was
// accepted. Paths, names and source bytes deliberately do not survive this
// boundary.
struct PreparedLatentAotModule {
    std::string id;
    std::string byte_identity;
    std::uint64_t disc_byte_offset = 0u;
    std::uint32_t byte_size = 0u;
    std::uint32_t source_address = 0u;
    // Includes the conventional module entry at offset zero and every accepted
    // hash-bound explicit entry. The exporter must require a native source
    // block for every listed offset before publishing a loaded-module template.
    std::vector<std::uint32_t> entry_offsets;
    // Sorted, non-overlapping identities for every uniquely emitted native IR
    // block. Only hashes survive export-time discovery; source bytes do not.
    std::vector<PreparedLatentAotBlockIdentity> block_identities;
    std::vector<katana::ir::Function> program;
};

struct LatentAotDiscovery {
    std::vector<PreparedLatentAotModule> modules;
    std::size_t examined_files = 0u;
    std::size_t rejected_files = 0u;
    std::size_t duplicate_files = 0u;
    std::uint64_t examined_bytes = 0u;
};

// Every address that the native backend relocates must stay inside the exact
// byte-identity extent. Otherwise a synthetic export-time base could leak into
// execution when the file is loaded at another guest address.
[[nodiscard]] bool latent_aot_program_is_relocation_closed(
    std::span<const katana::ir::Function> program,
    std::uint32_t source_start,
    std::uint32_t extent) noexcept;

[[nodiscard]] LatentAotDiscovery discover_latent_aot_modules(
    std::shared_ptr<const katana::runtime::DiscSource> source,
    std::uint32_t volume_start_lba,
    std::uint32_t extent_lba_bias,
    std::span<const std::string> excluded_byte_identities = {},
    const LatentAotDiscoveryOptions& options = {},
    std::span<const LatentAotOccupiedRange> occupied_source_ranges = {},
    std::span<const LatentAotEntryHint> entry_hints = {});

} // namespace katana::codegen

#pragma once

#include "katana/codegen/project.hpp"
#include "katana/io/executable_image.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace katana::codegen {

inline constexpr std::uint32_t source_map_schema_version = 1u;

struct AddressSourceLocation {
    std::uint32_t guest_address = 0u;
    std::string input_segment;
    std::uint64_t input_byte_offset = 0u;
    std::string generated_path;
    std::size_t generated_line = 0u;
};

[[nodiscard]] std::vector<AddressSourceLocation> build_address_source_map(
    const katana::io::ExecutableImage& image,
    std::span<const ProjectArtifact> generated_units
);

[[nodiscard]] std::span<const AddressSourceLocation> find_source_locations(
    std::span<const AddressSourceLocation> locations,
    std::uint32_t guest_address
) noexcept;

[[nodiscard]] std::string serialize_address_source_map(
    std::span<const AddressSourceLocation> locations
);

}

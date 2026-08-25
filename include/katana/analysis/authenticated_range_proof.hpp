#pragma once

#include "katana/io/executable_image.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace katana::analysis {

enum class AuthenticatedRangeAddressPolicy : std::uint8_t {
    // The supplied address itself must be backed by committed image bytes.
    Exact,
    // The validated ExecutableImage alias/address model may resolve the
    // supplied address to its authenticated source bytes.
    ResolveAlias,
};

enum class AuthenticatedRangePermission : std::uint8_t {
    Readable,
    Executable,
};

struct AuthenticatedRangeProof final {
    std::uint32_t requested_address = 0u;
    std::uint32_t authenticated_address = 0u;
    std::size_t size = 0u;
    std::uint64_t image_instance_identity = 0u;
    std::uint64_t immutable_generation = 0u;
};

// Authentication never invents an alias: callers must opt into the image's
// already validated alias map. Runtime-memory segments remain inadmissible,
// and every returned proof is bound to the current immutable generation.
[[nodiscard]] inline std::optional<AuthenticatedRangeProof>
authenticate_image_range(
    const katana::io::ExecutableImage& image,
    const std::uint32_t address,
    const std::size_t size,
    const AuthenticatedRangeAddressPolicy address_policy,
    const AuthenticatedRangePermission permission) noexcept {
    if (size == 0u) return std::nullopt;
    std::optional<std::uint32_t> authenticated_address;
    if (address_policy == AuthenticatedRangeAddressPolicy::Exact) {
        if (image.find_segment(address, size) != nullptr)
            authenticated_address = address;
    } else {
        authenticated_address = image.resolve_segment_address(address, size);
    }
    if (!authenticated_address.has_value()) return std::nullopt;

    const auto* const segment =
        image.find_segment(*authenticated_address, size);
    if (segment == nullptr || !segment->permissions.readable ||
        segment->source_kind == katana::io::ImageSourceKind::RuntimeMemory ||
        (permission == AuthenticatedRangePermission::Executable &&
         !segment->permissions.executable))
        return std::nullopt;
    const auto* const immutable =
        image.find_immutable_range(*authenticated_address, size);
    if (immutable == nullptr) return std::nullopt;

    return AuthenticatedRangeProof{
        address,
        *authenticated_address,
        size,
        image.analysis_instance_identity(),
        immutable->generation};
}

[[nodiscard]] inline bool same_authenticated_range_generation(
    const AuthenticatedRangeProof& left,
    const AuthenticatedRangeProof& right) noexcept {
    return left.image_instance_identity != 0u &&
           left.image_instance_identity == right.image_instance_identity &&
           left.immutable_generation != 0u &&
           left.immutable_generation == right.immutable_generation;
}

} // namespace katana::analysis

#pragma once

#include "katana/runtime/block_abi.hpp"

namespace katana::runtime {

namespace code_address_detail {

// Shared by the ABI helpers and generated native code. A scope change refreshes
// both intervals before the next guest instruction can use them. No mapping
// state is retained in an AOT frame across nested calls or provider boundaries.
struct LookupInterval final {
    // Nonwrapping [begin, begin+extent); 64 bits include full-space identity.
    std::uint64_t extent = 0u;
    std::uint32_t begin = 0u;
    std::uint32_t delta = 0u;
};

// Constant initialization is part of the fast-path contract: reading these
// trivial TLS objects must not invoke a per-translation-unit initializer.
extern constinit thread_local LookupInterval relocated;
extern constinit thread_local LookupInterval unrelocated;

} // namespace code_address_detail

// Keep the out-of-line ABI symbols for existing consumers. The native emitter
// uses these explicit inline forms to fold constant PCs and avoid one host call
// per lookup. Cache misses retain the exact innermost-mapping implementation.
[[nodiscard]] inline std::uint32_t relocate_code_address_inline(
    const std::uint32_t source_address) noexcept {
    const auto& cached = code_address_detail::relocated;
    if (static_cast<std::uint64_t>(source_address - cached.begin) < cached.extent)
        return source_address + cached.delta;
    return relocate_code_address(source_address);
}

[[nodiscard]] inline std::uint32_t unrelocate_code_address_inline(
    const std::uint32_t runtime_address) noexcept {
    const auto& cached = code_address_detail::unrelocated;
    if (static_cast<std::uint64_t>(runtime_address - cached.begin) < cached.extent)
        return runtime_address + cached.delta;
    return unrelocate_code_address(runtime_address);
}

} // namespace katana::runtime

#pragma once

#include "katana/abi_contract.hpp"

#include <cstdint>

#ifndef KATANA_ANALYZER_ABI_VERSION
#error "Katana analyzer headers require the KatanaRecomp::analyzer target contract"
#endif

namespace katana::analysis {

inline constexpr std::uint32_t abi_version = abi_contract::analyzer_abi_version;

static_assert(KATANA_ANALYZER_ABI_VERSION == abi_version,
              "Incompatible Katana analyzer ABI");

namespace detail {

template <std::uint32_t Version>
void require_analyzer_abi() noexcept;

template <>
void require_analyzer_abi<abi_version>() noexcept;

class AnalyzerAbiLinkGuard final {
  public:
    AnalyzerAbiLinkGuard() noexcept {
        require_analyzer_abi<abi_version>();
    }
};

// Give every translation unit that consumes a public analyzer layout an
// ABI-specific undefined reference.  The analyzer archive provides exactly
// the current specialization, so stale objects fail at link time.
[[maybe_unused]] static const AnalyzerAbiLinkGuard analyzer_abi_link_guard;

} // namespace detail

} // namespace katana::analysis

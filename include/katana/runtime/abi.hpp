#pragma once

#include "katana/build_contract.hpp"

#include <cstdint>

namespace katana::runtime {

inline constexpr std::uint32_t abi_version = build_contract::runtime_abi_version;

} // namespace katana::runtime

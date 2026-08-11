#pragma once

#include "katana/build_contract.hpp"

#include <cstdint>

namespace katana::runtime {

inline constexpr std::uint32_t native_port_profile_contract_version =
    build_contract::native_port_profile_contract_version;

struct NativePortLinkContract final {
    std::uint32_t version = native_port_profile_contract_version;
    bool allows_guest_cpu_interpreter = false;
    bool allows_legacy_device_runtime = false;
    bool allows_software_pvr = false;
};

[[nodiscard]] const NativePortLinkContract&
native_port_link_contract() noexcept;

} // namespace katana::runtime

#include "katana/runtime/abi.hpp"
#include "katana/runtime/native_port.hpp"
#include "katana/runtime/system_replay.hpp"

static_assert(katana::runtime::abi_version == KATANA_EXPECTED_RUNTIME_ABI);
static_assert(katana::build_contract::port_project_contract_version ==
              KATANA_EXPECTED_PORT_PROJECT_CONTRACT);
static_assert(katana::runtime::native_port_profile_contract_version ==
              KATANA_EXPECTED_NATIVE_PORT_PROFILE_CONTRACT);

int main() {
    const auto& native_port = katana::runtime::native_port_link_contract();
    const katana::runtime::SystemReplayLog replay(
        katana::runtime::SystemReplayConfig{1u, false});
    return replay.config().capacity == 1u &&
                   !native_port.allows_guest_cpu_interpreter &&
                   !native_port.allows_legacy_device_runtime &&
                   !native_port.allows_software_pvr
               ? 0
               : 1;
}

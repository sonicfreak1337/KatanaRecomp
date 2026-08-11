#include "katana/runtime/abi.hpp"
#include "katana/runtime/native_port.hpp"
#include "katana/runtime/runtime.hpp"

static_assert(katana::runtime::abi_version == KATANA_EXPECTED_RUNTIME_ABI);
static_assert(katana::build_contract::port_project_contract_version ==
              KATANA_EXPECTED_PORT_PROJECT_CONTRACT);
static_assert(katana::runtime::native_port_profile_contract_version ==
              KATANA_EXPECTED_NATIVE_PORT_PROFILE_CONTRACT);

int main() {
    const auto& native_port = katana::runtime::native_port_link_contract();
    katana::runtime::CpuState cpu;
    katana::runtime::reset_cpu(
        cpu, katana::runtime::ResetState{0x1000u, 0x2000u});
    return cpu.pc == 0x1000u && cpu.r[15] == 0x2000u &&
                   !native_port.allows_guest_cpu_interpreter &&
                   !native_port.allows_legacy_device_runtime &&
                   !native_port.allows_software_pvr &&
                   !native_port.allows_ta_packet_renderer &&
                   !native_port.allows_aica_command_translation
               ? 0
               : 1;
}

#include "katana/runtime/abi.hpp"
#include "katana/runtime/system_replay.hpp"

static_assert(katana::runtime::abi_version == KATANA_EXPECTED_RUNTIME_ABI);
static_assert(katana::build_contract::port_project_contract_version ==
              KATANA_EXPECTED_PORT_PROJECT_CONTRACT);

int main() {
    const katana::runtime::SystemReplayLog replay(
        katana::runtime::SystemReplayConfig{1u, false});
    return replay.config().capacity == 1u ? 0 : 1;
}

#include "katana/runtime/abi.hpp"

static_assert(katana::runtime::abi_version == KATANA_EXPECTED_RUNTIME_ABI);

int main() {
    return 0;
}

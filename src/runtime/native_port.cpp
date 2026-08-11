#include "katana/runtime/native_port.hpp"

namespace katana::runtime {

const NativePortLinkContract& native_port_link_contract() noexcept {
    static constexpr NativePortLinkContract contract;
    return contract;
}

} // namespace katana::runtime

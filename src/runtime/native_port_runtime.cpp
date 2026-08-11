#include "katana/runtime/native_port.hpp"

namespace katana::runtime {

// This symbol is the positive link anchor for the native product archive.
// Generated bootstrap code must consume it, so merely naming the archive as a
// transitive dependency cannot masquerade as a successfully linked native
// runtime in the post-link map audit.
const NativePortLinkContract& native_port_link_contract() noexcept {
    static constexpr NativePortLinkContract contract;
    return contract;
}

} // namespace katana::runtime

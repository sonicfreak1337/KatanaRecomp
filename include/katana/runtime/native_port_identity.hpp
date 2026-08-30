#pragma once

#include "katana/runtime/native_port.hpp"

#include <string>

namespace katana::runtime {

// Canonical identity of a validated NativePortDefinition as consumed by
// code-generation/runtime-frontier bindings. This is deliberately separate
// from the product AOT runtime contract: provider-refresh tooling needs the
// semantic definition identity, while generated execution units do not.
[[nodiscard]] std::string native_port_definition_export_identity(
    const NativePortDefinition& definition);

} // namespace katana::runtime

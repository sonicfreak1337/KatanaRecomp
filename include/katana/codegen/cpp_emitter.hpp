#pragma once

#include "katana/codegen/backend.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace katana::codegen {

[[nodiscard]] bool cpp_backend_supports_operation(katana::ir::Operation operation) noexcept;

// These names are part of the generated C++ translation-unit contract. External
// wrappers must use the same spelling (including hexadecimal letter case) as the
// backend declarations and definitions.
[[nodiscard]] std::string cpp_function_name(std::uint32_t address);
[[nodiscard]] std::string cpp_service_function_name(std::uint32_t address);

class CppBackend final : public Backend {
  public:
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] std::uint32_t interface_abi_version() const noexcept override;
    [[nodiscard]] std::uint32_t runtime_abi_version() const noexcept override;
    [[nodiscard]] BackendCapabilities capabilities() const noexcept override;
    [[nodiscard]] BackendEmission emit(const BackendRequest& request) const override;
};

// Product ports own the runtime-dispatch TLS metadata consumed after a generated
// block returns.  Emitting the statically known block-exit metadata directly
// avoids routing every local basic-block entry through another translation unit.
[[nodiscard]] BackendEmission
emit_cpp_port_translation_unit(const BackendRequest& request);

[[nodiscard]] std::string emit_cpp_program(std::span<const katana::ir::Function> functions,
                                           std::uint32_t entry_address);

} // namespace katana::codegen

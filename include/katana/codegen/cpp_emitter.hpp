#pragma once

#include "katana/codegen/backend.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace katana::codegen {

class CppBackend final : public Backend {
public:
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] BackendEmission emit(const BackendRequest& request) const override;
};

[[nodiscard]] std::string emit_cpp_program(
    std::span<const katana::ir::Function> functions,
    std::uint32_t entry_address
);

}

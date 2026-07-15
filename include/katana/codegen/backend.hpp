#pragma once

#include "katana/ir/ir.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace katana::codegen {

struct BackendRequest {
    std::span<const katana::ir::Function> functions;
    std::uint32_t entry_address = 0u;
};

struct BackendEmission {
    std::string declarations;
    std::string functions;
    std::string metadata;

    [[nodiscard]] std::string joined_text() const;
};

class Backend {
public:
    virtual ~Backend() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual BackendEmission emit(const BackendRequest& request) const = 0;
};

[[nodiscard]] BackendEmission generate_program(
    const Backend& backend,
    const BackendRequest& request
);

} // namespace katana::codegen

#pragma once

#include "katana/ir/ir.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace katana::ir {

struct VerificationIssue {
    std::uint32_t address = 0u;
    std::string message;
};

[[nodiscard]] std::vector<VerificationIssue> verify_function(const Function& function);

void require_valid_function(const Function& function);

[[nodiscard]] std::vector<VerificationIssue> verify_program(std::span<const Function> functions);

[[nodiscard]] std::vector<VerificationIssue>
verify_program(std::span<const Function> functions,
               std::span<const std::uint32_t> external_function_entries);

void require_valid_program(std::span<const Function> functions);

void require_valid_program(std::span<const Function> functions,
                           std::span<const std::uint32_t> external_function_entries);

} // namespace katana::ir

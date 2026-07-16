#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace katana::cli {

enum class ExitCode : int {
    Success = 0,
    Usage = 2,
    InvalidInput = 3,
    InputOutput = 4,
    ProcessingFailure = 5,
    CodeGenerationFailure = 6,
    BuildFailure = 7,
    InternalError = 70
};

[[nodiscard]] constexpr int exit_status(const ExitCode code) noexcept {
    return static_cast<int>(code);
}

[[nodiscard]] constexpr std::string_view exit_code_name(const ExitCode code) noexcept {
    switch (code) {
    case ExitCode::Success:
        return "success";
    case ExitCode::Usage:
        return "usage";
    case ExitCode::InvalidInput:
        return "invalid-input";
    case ExitCode::InputOutput:
        return "input-output";
    case ExitCode::ProcessingFailure:
        return "processing-failure";
    case ExitCode::CodeGenerationFailure:
        return "codegen-failure";
    case ExitCode::BuildFailure:
        return "build-failure";
    case ExitCode::InternalError:
        return "internal-error";
    }
    return "internal-error";
}

class Error final : public std::runtime_error {
  public:
    Error(const ExitCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {
        if (code == ExitCode::Success) {
            throw std::invalid_argument("CLI-Fehler darf keinen Erfolgsstatus tragen.");
        }
    }

    [[nodiscard]] ExitCode code() const noexcept {
        return code_;
    }

  private:
    ExitCode code_;
};

} // namespace katana::cli

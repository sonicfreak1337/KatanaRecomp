#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace katana::io {

class InputOutputError final : public std::runtime_error {
  public:
    explicit InputOutputError(std::string message) : std::runtime_error(std::move(message)) {}
};

} // namespace katana::io

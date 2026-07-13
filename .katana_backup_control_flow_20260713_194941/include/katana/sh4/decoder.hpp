#pragma once

#include "katana/sh4/instruction.hpp"

#include <cstdint>

namespace katana::sh4 {

[[nodiscard]] DecodedInstruction decode(std::uint16_t opcode);

}

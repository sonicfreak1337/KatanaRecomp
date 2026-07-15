#include "katana/codegen/probe.hpp"

#include <algorithm>

namespace katana::codegen {

bool block_requires_call_dispatch(const katana::ir::BasicBlock& block) noexcept {
    return std::any_of(
        block.instructions.begin(),
        block.instructions.end(),
        [](const katana::ir::Instruction& instruction) {
            return instruction.operation == katana::ir::Operation::Call ||
                instruction.operation == katana::ir::Operation::CallRegister;
        }
    );
}

} // namespace katana::codegen

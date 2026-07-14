#include "katana/ir/optimize.hpp"

#include "katana/ir/verifier.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

namespace katana::ir {
namespace {

using Constants = std::array<std::optional<std::uint32_t>, 16>;
using Aliases = std::array<std::optional<std::uint8_t>, 16>;

void canonicalize(Instruction& instruction) {
    instruction.widths = operation_operand_widths(instruction.operation);
    instruction.status_effects = instruction_status_effects(
        instruction.operation,
        instruction.special_register
    );
    instruction.memory_effects = instruction_memory_effects(
        instruction.operation,
        instruction.destination_register,
        instruction.source_register
    );
    instruction.accumulator_effects = operation_accumulator_effects(
        instruction.operation
    );
}

void replace_with_constant(
    Instruction& instruction,
    const std::uint8_t destination,
    const std::uint32_t value
) {
    instruction.operation = Operation::MovImmediate;
    instruction.destination_register = destination;
    instruction.source_register = 0u;
    instruction.branch_register = 0u;
    instruction.immediate = static_cast<std::int32_t>(value);
    instruction.displacement = 0;
    instruction.special_register = SpecialRegister::None;
    instruction.effective_address.reset();
    instruction.target_address.reset();
    instruction.forwarded_value_register.reset();
    canonicalize(instruction);
}

std::optional<std::uint32_t> binary_constant(
    const Constants& constants,
    const Instruction& instruction
) {
    const auto left = constants[instruction.destination_register];
    const auto right = constants[instruction.source_register];
    if (!left || !right) return std::nullopt;

    switch (instruction.operation) {
        case Operation::AddRegister: return *left + *right;
        case Operation::SubRegister: return *left - *right;
        case Operation::AndRegister: return *left & *right;
        case Operation::OrRegister: return *left | *right;
        case Operation::XorRegister: return *left ^ *right;
        default: return std::nullopt;
    }
}

OptimizationResult fold_block(BasicBlock& block) {
    Constants constants{};
    OptimizationResult result;

    for (auto& instruction : block.instructions) {
        const auto destination = instruction.destination_register;
        switch (instruction.operation) {
            case Operation::MovImmediate:
                constants[destination] = static_cast<std::uint32_t>(instruction.immediate);
                break;

            case Operation::MovRegister:
                if (constants[instruction.source_register]) {
                    const auto value = *constants[instruction.source_register];
                    replace_with_constant(instruction, destination, value);
                    constants[destination] = value;
                    ++result.changes;
                } else {
                    constants[destination].reset();
                }
                break;

            case Operation::AddImmediate:
                if (constants[destination]) {
                    const auto value = *constants[destination] +
                        static_cast<std::uint32_t>(instruction.immediate);
                    replace_with_constant(instruction, destination, value);
                    constants[destination] = value;
                    ++result.changes;
                } else {
                    constants[destination].reset();
                }
                break;

            case Operation::AddRegister:
            case Operation::SubRegister:
            case Operation::AndRegister:
            case Operation::OrRegister:
            case Operation::XorRegister: {
                const auto value = binary_constant(constants, instruction);
                if (value) {
                    replace_with_constant(instruction, destination, *value);
                    constants[destination] = *value;
                    ++result.changes;
                } else {
                    constants[destination].reset();
                }
                break;
            }

            case Operation::NegateRegister:
            case Operation::NotRegister:
                if (constants[instruction.source_register]) {
                    const auto source = *constants[instruction.source_register];
                    const auto value = instruction.operation == Operation::NegateRegister
                        ? 0u - source
                        : ~source;
                    replace_with_constant(instruction, destination, value);
                    constants[destination] = value;
                    ++result.changes;
                } else {
                    constants[destination].reset();
                }
                break;

            case Operation::Nop:
            case Operation::ClearS:
            case Operation::SetS:
            case Operation::ClearT:
            case Operation::SetT:
            case Operation::CompareEqualImmediate:
            case Operation::CompareEqualRegister:
            case Operation::CompareHigherOrSame:
            case Operation::CompareGreaterOrEqual:
            case Operation::CompareHigher:
            case Operation::CompareGreaterThan:
            case Operation::ComparePositiveOrZero:
            case Operation::ComparePositive:
            case Operation::CompareString:
            case Operation::TestImmediate:
            case Operation::TestRegister:
            case Operation::Branch:
            case Operation::Call:
            case Operation::BranchIfTrue:
            case Operation::BranchIfFalse:
            case Operation::JumpRegister:
            case Operation::CallRegister:
            case Operation::Return:
            case Operation::ReturnFromException:
            case Operation::TrapAlways:
            case Operation::Sleep:
                break;

            default:
                constants.fill(std::nullopt);
                break;
        }
    }
    return result;
}

std::uint8_t resolve_alias(const Aliases& aliases, std::uint8_t value) {
    for (std::size_t depth = 0u; depth < aliases.size(); ++depth) {
        if (!aliases[value] || *aliases[value] == value) break;
        value = *aliases[value];
    }
    return value;
}

void invalidate_aliases(Aliases& aliases, const std::uint8_t written) {
    std::array<bool, 16> invalid{};
    for (std::size_t index = 0u; index < aliases.size(); ++index) {
        invalid[index] =
            index == written ||
            (aliases[index] &&
                resolve_alias(aliases, static_cast<std::uint8_t>(index)) == written);
    }
    for (std::size_t index = 0u; index < aliases.size(); ++index) {
        if (invalid[index]) aliases[index].reset();
    }
}

bool has_propagatable_source(const Operation operation) noexcept {
    switch (operation) {
        case Operation::MovRegister:
        case Operation::AddRegister:
        case Operation::SubRegister:
        case Operation::NegateRegister:
        case Operation::NotRegister:
        case Operation::AndRegister:
        case Operation::OrRegister:
        case Operation::XorRegister:
        case Operation::CompareEqualRegister:
        case Operation::CompareHigherOrSame:
        case Operation::CompareGreaterOrEqual:
        case Operation::CompareHigher:
        case Operation::CompareGreaterThan:
        case Operation::CompareString:
        case Operation::TestRegister:
            return true;
        default:
            return false;
    }
}

bool writes_destination(const Operation operation) noexcept {
    switch (operation) {
        case Operation::MovImmediate:
        case Operation::MovRegister:
        case Operation::AddImmediate:
        case Operation::AddRegister:
        case Operation::SubRegister:
        case Operation::NegateRegister:
        case Operation::NotRegister:
        case Operation::AndRegister:
        case Operation::OrRegister:
        case Operation::XorRegister:
            return true;
        default:
            return false;
    }
}

OptimizationResult propagate_block_copies(BasicBlock& block) {
    Aliases aliases{};
    OptimizationResult result;

    for (auto& instruction : block.instructions) {
        if (has_propagatable_source(instruction.operation)) {
            const auto resolved = resolve_alias(aliases, instruction.source_register);
            if (resolved != instruction.source_register) {
                instruction.source_register = resolved;
                canonicalize(instruction);
                ++result.changes;
            }
        }

        if (writes_destination(instruction.operation)) {
            const auto destination = instruction.destination_register;
            invalidate_aliases(aliases, destination);
            if (
                instruction.operation == Operation::MovRegister &&
                destination != instruction.source_register
            ) {
                aliases[destination] = instruction.source_register;
            }
            continue;
        }

        switch (instruction.operation) {
            case Operation::Nop:
            case Operation::ClearS:
            case Operation::SetS:
            case Operation::ClearT:
            case Operation::SetT:
            case Operation::CompareEqualImmediate:
            case Operation::CompareEqualRegister:
            case Operation::CompareHigherOrSame:
            case Operation::CompareGreaterOrEqual:
            case Operation::CompareHigher:
            case Operation::CompareGreaterThan:
            case Operation::ComparePositiveOrZero:
            case Operation::ComparePositive:
            case Operation::CompareString:
            case Operation::TestImmediate:
            case Operation::TestRegister:
            case Operation::Branch:
            case Operation::Call:
            case Operation::BranchIfTrue:
            case Operation::BranchIfFalse:
            case Operation::JumpRegister:
            case Operation::CallRegister:
            case Operation::Return:
            case Operation::ReturnFromException:
            case Operation::TrapAlways:
            case Operation::Sleep:
                break;
            default:
                aliases.fill(std::nullopt);
                break;
        }
    }
    return result;
}

bool is_pure_register_write(const Instruction& instruction) noexcept {
    if (
        instruction.delay_slot.role != DelaySlotRole::None ||
        instruction.is_privileged ||
        instruction.status_effects != StatusRegisterEffects{} ||
        instruction.memory_effects != MemoryEffects{} ||
        instruction.accumulator_effects != AccumulatorEffects{}
    ) {
        return false;
    }
    return writes_destination(instruction.operation);
}

bool reads_register(
    const Instruction& instruction,
    const std::uint8_t register_index
) noexcept {
    switch (instruction.operation) {
        case Operation::MovImmediate:
            return false;
        case Operation::MovRegister:
        case Operation::NegateRegister:
        case Operation::NotRegister:
            return instruction.source_register == register_index;
        case Operation::AddImmediate:
            return instruction.destination_register == register_index;
        case Operation::AddRegister:
        case Operation::SubRegister:
        case Operation::AndRegister:
        case Operation::OrRegister:
        case Operation::XorRegister:
            return instruction.destination_register == register_index ||
                instruction.source_register == register_index;
        default:
            return true;
    }
}

OptimizationResult eliminate_dead_block_code(BasicBlock& block) {
    OptimizationResult result;
    std::size_t index = 1u;
    while (index < block.instructions.size()) {
        const auto& candidate = block.instructions[index];
        if (!is_pure_register_write(candidate)) {
            ++index;
            continue;
        }

        const auto destination = candidate.destination_register;
        bool overwritten = false;
        for (std::size_t next = index + 1u; next < block.instructions.size(); ++next) {
            const auto& instruction = block.instructions[next];
            if (reads_register(instruction, destination)) break;
            if (
                is_pure_register_write(instruction) &&
                instruction.destination_register == destination
            ) {
                overwritten = true;
                break;
            }
            if (!is_pure_register_write(instruction) &&
                instruction.operation != Operation::Nop) {
                break;
            }
        }

        if (overwritten) {
            block.instructions.erase(block.instructions.begin() +
                static_cast<std::ptrdiff_t>(index));
            ++result.changes;
        } else {
            ++index;
        }
    }
    return result;
}

OptimizationResult simplify_function_cfg(Function& function) {
    std::unordered_set<std::uint32_t> reachable;
    std::vector<std::uint32_t> worklist = {function.entry_address};
    while (!worklist.empty()) {
        const auto address = worklist.back();
        worklist.pop_back();
        if (!reachable.insert(address).second) continue;
        for (const auto& block : function.blocks) {
            if (block.start_address != address) continue;
            worklist.insert(
                worklist.end(),
                block.successors.begin(),
                block.successors.end()
            );
            break;
        }
    }

    OptimizationResult result;
    const auto original_size = function.blocks.size();
    function.blocks.erase(
        std::remove_if(
            function.blocks.begin(),
            function.blocks.end(),
            [&reachable](const BasicBlock& block) {
                return !reachable.contains(block.start_address);
            }
        ),
        function.blocks.end()
    );
    result.changes += original_size - function.blocks.size();

    for (auto& block : function.blocks) {
        const auto successor_size = block.successors.size();
        std::sort(block.successors.begin(), block.successors.end());
        block.successors.erase(
            std::unique(block.successors.begin(), block.successors.end()),
            block.successors.end()
        );
        result.changes += successor_size - block.successors.size();
    }
    std::sort(function.blocks.begin(), function.blocks.end(),
        [](const BasicBlock& left, const BasicBlock& right) {
            return left.start_address < right.start_address;
        });
    return result;
}

OptimizationResult simplify_block_load_store(BasicBlock& block) {
    OptimizationResult result;
    for (std::size_t index = 1u; index < block.instructions.size(); ++index) {
        const auto& store = block.instructions[index - 1u];
        auto& load = block.instructions[index];
        if (
            store.operation == Operation::StoreLong &&
            load.operation == Operation::LoadLong &&
            store.destination_register == load.source_register &&
            !load.forwarded_value_register.has_value()
        ) {
            load.forwarded_value_register = store.source_register;
            ++result.changes;
        }
    }
    return result;
}

}

OptimizationResult fold_constants(Function& function) {
    require_valid_function(function);
    OptimizationResult result;
    for (auto& block : function.blocks) {
        result.changes += fold_block(block).changes;
    }
    require_valid_function(function);
    return result;
}

OptimizationResult propagate_copies(Function& function) {
    require_valid_function(function);
    OptimizationResult result;
    for (auto& block : function.blocks) {
        result.changes += propagate_block_copies(block).changes;
    }
    require_valid_function(function);
    return result;
}

OptimizationResult eliminate_dead_code(Function& function) {
    require_valid_function(function);
    OptimizationResult result;
    for (auto& block : function.blocks) {
        result.changes += eliminate_dead_block_code(block).changes;
    }
    require_valid_function(function);
    return result;
}

OptimizationResult simplify_cfg(Function& function) {
    require_valid_function(function);
    const auto result = simplify_function_cfg(function);
    require_valid_function(function);
    return result;
}

OptimizationResult simplify_load_store(Function& function) {
    require_valid_function(function);
    OptimizationResult result;
    for (auto& block : function.blocks) {
        result.changes += simplify_block_load_store(block).changes;
    }
    require_valid_function(function);
    return result;
}

}

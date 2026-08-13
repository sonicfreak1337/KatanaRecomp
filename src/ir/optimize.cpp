#include "katana/ir/optimize.hpp"

#include "katana/ir/serialize.hpp"
#include "katana/ir/verifier.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace katana::ir {
namespace {

using Constants = std::array<std::optional<std::uint32_t>, 16>;
using Aliases = std::array<std::optional<std::uint8_t>, 16>;

struct DispatchabilityIndex {
    std::unordered_set<std::uint32_t> block_entries;
    std::unordered_set<std::uint32_t> instruction_continuations;
};

DispatchabilityIndex make_dispatchability_index(
    const OptimizationDispatchabilityContract& contract) {
    DispatchabilityIndex result;
    result.block_entries.reserve(contract.entries.size());
    result.instruction_continuations.reserve(contract.entries.size());
    std::unordered_set<std::uint32_t> addresses;
    addresses.reserve(contract.entries.size());
    for (const auto& entry : contract.entries) {
        if (entry.address == 0u || (entry.address & 1u) != 0u ||
            !addresses.insert(entry.address).second)
            throw std::invalid_argument(
                "Externer IR-Dispatch-Eintritt ist ungueltig oder "
                "doppelt deklariert.");
        switch (entry.kind) {
        case ExternalDispatchEntryKind::BlockEntry:
            result.block_entries.insert(entry.address);
            break;
        case ExternalDispatchEntryKind::InstructionContinuation:
            result.instruction_continuations.insert(entry.address);
            break;
        default:
            throw std::invalid_argument(
                "Externer IR-Dispatch-Eintritt besitzt eine ungueltige "
                "Art.");
        }
    }
    return result;
}

void require_dispatchability_contract(
    const std::span<const Function> program,
    const DispatchabilityIndex& contract) {
    if (contract.block_entries.empty() &&
        contract.instruction_continuations.empty())
        return;

    std::unordered_set<std::uint32_t> found_blocks;
    std::unordered_set<std::uint32_t> found_continuations;
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            if (contract.block_entries.contains(block.start_address))
                found_blocks.insert(block.start_address);
            for (const auto& instruction : block.instructions) {
                if (!contract.instruction_continuations.contains(
                        instruction.source_address))
                    continue;
                if (instruction.delay_slot.role == DelaySlotRole::Slot)
                    throw std::invalid_argument(
                        "Externe IR-Instruktionsfortsetzung liegt in einem "
                        "Delay Slot.");
                found_continuations.insert(instruction.source_address);
            }
        }
    }
    if (found_blocks.size() != contract.block_entries.size()) {
        const auto missing = std::find_if(
            contract.block_entries.begin(), contract.block_entries.end(),
            [&](const auto address) { return !found_blocks.contains(address); });
        if (missing == contract.block_entries.end())
            throw std::invalid_argument(
                "Extern erreichbare IR-Blockeintritte sind nicht "
                "kanonisch eindeutig.");
        throw std::invalid_argument(
            "Extern erreichbarer IR-Blockeintritt fehlt: " +
            std::to_string(*missing));
    }
    if (found_continuations.size() !=
        contract.instruction_continuations.size()) {
        const auto missing = std::find_if(
            contract.instruction_continuations.begin(),
            contract.instruction_continuations.end(),
            [&](const auto address) {
                return !found_continuations.contains(address);
            });
        if (missing == contract.instruction_continuations.end())
            throw std::invalid_argument(
                "Extern erreichbare IR-Instruktionsfortsetzungen sind "
                "nicht kanonisch eindeutig.");
        throw std::invalid_argument(
            "Extern erreichbare IR-Instruktionsfortsetzung fehlt: " +
            std::to_string(*missing));
    }
}

void canonicalize(Instruction& instruction) {
    instruction.widths = operation_operand_widths(instruction.operation);
    instruction.status_effects =
        instruction_status_effects(instruction.operation, instruction.special_register);
    instruction.memory_effects = instruction_memory_effects(
        instruction.operation, instruction.destination_register, instruction.source_register);
    instruction.accumulator_effects =
        operation_accumulator_effects(instruction.operation, instruction.special_register);
}

void replace_with_nop(Instruction& instruction) {
    instruction.operation = Operation::Nop;
    instruction.destination_register = 0u;
    instruction.source_register = 0u;
    instruction.branch_register = 0u;
    instruction.immediate = 0;
    instruction.displacement = 0;
    instruction.special_register = SpecialRegister::None;
    instruction.effective_address.reset();
    instruction.target_address.reset();
    instruction.resolved_targets.clear();
    instruction.forwarded_value_register.reset();
    instruction.dynamic_target_class = DynamicTargetClass::NotApplicable;
    instruction.is_privileged = false;
    instruction.branch_register_relative = false;
    canonicalize(instruction);
}

void replace_with_constant(Instruction& instruction,
                           const std::uint8_t destination,
                           const std::uint32_t value) {
    instruction.operation = Operation::Constant32;
    instruction.destination_register = destination;
    instruction.source_register = 0u;
    instruction.branch_register = 0u;
    instruction.immediate = static_cast<std::int32_t>(value);
    instruction.displacement = 0;
    instruction.special_register = SpecialRegister::None;
    instruction.effective_address.reset();
    instruction.target_address.reset();
    instruction.resolved_targets.clear();
    instruction.forwarded_value_register.reset();
    instruction.dynamic_target_class = DynamicTargetClass::NotApplicable;
    canonicalize(instruction);
}

std::optional<std::uint32_t> binary_constant(const Constants& constants,
                                             const Instruction& instruction) {
    const auto left = constants[instruction.destination_register];
    const auto right = constants[instruction.source_register];
    if (!left || !right) return std::nullopt;

    switch (instruction.operation) {
    case Operation::AddRegister:
        return *left + *right;
    case Operation::SubRegister:
        return *left - *right;
    case Operation::AndRegister:
        return *left & *right;
    case Operation::OrRegister:
        return *left | *right;
    case Operation::XorRegister:
        return *left ^ *right;
    default:
        return std::nullopt;
    }
}

OptimizationResult fold_block(BasicBlock& block) {
    Constants constants{};
    OptimizationResult result;

    for (auto& instruction : block.instructions) {
        const auto destination = instruction.destination_register;
        switch (instruction.operation) {
        case Operation::MovImmediate:
        case Operation::Constant32:
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
                const auto value =
                    *constants[destination] + static_cast<std::uint32_t>(instruction.immediate);
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
                const auto value =
                    instruction.operation == Operation::NegateRegister ? 0u - source : ~source;
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
            (aliases[index] && resolve_alias(aliases, static_cast<std::uint8_t>(index)) == written);
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
    case Operation::Constant32:
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
            if (instruction.operation == Operation::MovRegister &&
                destination != instruction.source_register) {
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
    if (instruction.delay_slot.role != DelaySlotRole::None || instruction.is_privileged ||
        instruction.status_effects != StatusRegisterEffects{} ||
        instruction.memory_effects != MemoryEffects{} ||
        instruction.accumulator_effects != AccumulatorEffects{}) {
        return false;
    }
    return writes_destination(instruction.operation);
}

bool reads_register(const Instruction& instruction, const std::uint8_t register_index) noexcept {
    switch (instruction.operation) {
    case Operation::MovImmediate:
    case Operation::Constant32:
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
            if (is_pure_register_write(instruction) &&
                instruction.destination_register == destination) {
                overwritten = true;
                break;
            }
            if (!is_pure_register_write(instruction) && instruction.operation != Operation::Nop) {
                break;
            }
        }

        if (overwritten) {
            // A dead architectural register write may lose its host-side effect,
            // but its guest instruction boundary, timing, and source PC remain
            // observable. Keep a semantic NOP instead of
            // shortening the native instruction stream.
            replace_with_nop(block.instructions[index]);
            ++result.changes;
        }
        ++index;
    }
    return result;
}

OptimizationResult simplify_function_cfg(
    Function& function,
    const DispatchabilityIndex& dispatchability) {
    std::unordered_map<std::uint32_t, const BasicBlock*> block_by_address;
    block_by_address.reserve(function.blocks.size());
    for (const auto& block : function.blocks)
        block_by_address.emplace(block.start_address, &block);
    std::unordered_set<std::uint32_t> reachable;
    std::vector<std::uint32_t> worklist = {function.entry_address};
    for (const auto& block : function.blocks) {
        if (dispatchability.block_entries.contains(
                block.start_address))
            worklist.push_back(block.start_address);
        if (std::any_of(
                block.instructions.begin(), block.instructions.end(),
                [&](const auto& instruction) {
                    return dispatchability.instruction_continuations
                        .contains(instruction.source_address);
                }))
            worklist.push_back(block.start_address);
    }
    while (!worklist.empty()) {
        const auto address = worklist.back();
        worklist.pop_back();
        if (!reachable.insert(address).second) continue;
        const auto block = block_by_address.find(address);
        if (block != block_by_address.end()) {
            worklist.insert(worklist.end(),
                            block->second->successors.begin(),
                            block->second->successors.end());
            // Ownership-only case edges retain the bounded switch body but
            // never become executable successors or emitter dispatch edges.
            worklist.insert(
                worklist.end(),
                block->second->guarded_case_ownership_targets.begin(),
                block->second->guarded_case_ownership_targets.end());
        }
    }

    OptimizationResult result;
    const auto original_size = function.blocks.size();
    function.blocks.erase(std::remove_if(function.blocks.begin(),
                                         function.blocks.end(),
                                         [&reachable](const BasicBlock& block) {
                                             return !reachable.contains(block.start_address);
                                         }),
                          function.blocks.end());
    result.changes += original_size - function.blocks.size();

    for (auto& block : function.blocks) {
        const auto successor_size = block.successors.size();
        std::sort(block.successors.begin(), block.successors.end());
        block.successors.erase(std::unique(block.successors.begin(), block.successors.end()),
                               block.successors.end());
        result.changes += successor_size - block.successors.size();
    }
    std::sort(function.blocks.begin(),
              function.blocks.end(),
              [](const BasicBlock& left, const BasicBlock& right) {
                  return left.start_address < right.start_address;
              });

    std::vector<std::uint32_t> direct_callees;
    std::vector<std::uint32_t> indirect_call_sites;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.operation == Operation::Call && instruction.target_address) {
                direct_callees.push_back(*instruction.target_address);
            }
            if (instruction.operation == Operation::CallRegister) {
                indirect_call_sites.push_back(instruction.source_address);
                direct_callees.insert(direct_callees.end(),
                                      instruction.resolved_targets.begin(),
                                      instruction.resolved_targets.end());
            }
        }
    }
    const auto canonicalize_addresses = [](std::vector<std::uint32_t>& values) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    };
    canonicalize_addresses(direct_callees);
    canonicalize_addresses(indirect_call_sites);
    if (function.direct_callees != direct_callees) {
        function.direct_callees = std::move(direct_callees);
        ++result.changes;
    }
    if (function.indirect_call_sites != indirect_call_sites) {
        function.indirect_call_sites = std::move(indirect_call_sites);
        ++result.changes;
    }
    return result;
}

OptimizationResult simplify_block_load_store(BasicBlock& block) {
    OptimizationResult result;
    for (std::size_t index = 1u; index < block.instructions.size(); ++index) {
        const auto& store = block.instructions[index - 1u];
        auto& load = block.instructions[index];
        if (store.operation == Operation::StoreLong && load.operation == Operation::LoadLong &&
            store.destination_register == load.source_register &&
            store.memory_effects.region == MemoryRegionKind::NormalRam &&
            load.memory_effects.region == MemoryRegionKind::NormalRam &&
            !load.forwarded_value_register.has_value()) {
            load.forwarded_value_register = store.source_register;
            ++result.changes;
        }
    }
    return result;
}

} // namespace

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
    return simplify_cfg(function, OptimizationDispatchabilityContract{});
}

OptimizationResult simplify_cfg(
    Function& function,
    const OptimizationDispatchabilityContract& dispatchability) {
    require_valid_function(function);
    const auto index = make_dispatchability_index(dispatchability);
    const std::span<const Function> single_function(&function, 1u);
    require_dispatchability_contract(single_function, index);
    const auto result = simplify_function_cfg(
        function, index);
    require_valid_function(function);
    require_dispatchability_contract(single_function, index);
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

OptimizationPipelineReport optimize_program(std::vector<Function>& program,
                                            const OptimizationOptions& options,
                                            const katana::ProgressReporter& progress) {
    return optimize_program(
        program, options, progress,
        OptimizationDispatchabilityContract{});
}

OptimizationPipelineReport optimize_program(
    std::vector<Function>& program,
    const OptimizationOptions& options,
    const katana::ProgressReporter& progress,
    const OptimizationDispatchabilityContract& dispatchability) {
    const auto dispatchability_index =
        make_dispatchability_index(dispatchability);
    require_dispatchability_contract(program, dispatchability_index);
    OptimizationPipelineReport report;
    const auto enabled_passes =
        static_cast<std::size_t>(options.constant_folding) +
        static_cast<std::size_t>(options.copy_propagation) +
        static_cast<std::size_t>(options.dead_code_elimination) +
        static_cast<std::size_t>(options.cfg_simplification) +
        static_cast<std::size_t>(
            options.load_store_simplification);
    auto optimization_progress = progress.begin(
        katana::ProgressOperation::IrOptimization,
        katana::ProgressUnit::Steps,
        options.enabled
            ? std::optional<std::uint64_t>(
                  enabled_passes * program.size())
            : std::nullopt,
        "ir-optimization");
    if (!options.enabled) {
        optimization_progress.skipped();
        return report;
    }

    const auto run_pass =
        [&program,
         &options,
         &report,
         &optimization_progress,
         &dispatchability_index](
            const char* name,
            const bool enabled,
            const auto& pass) {
            if (!enabled) return;
            require_dispatchability_contract(
                program, dispatchability_index);
            OptimizationPassReport pass_report;
            pass_report.name = name;
            if (options.capture_dumps) {
                pass_report.before = emit_ir_text(program);
            }
            for (auto& function : program) {
                pass_report.changes += pass(function).changes;
                optimization_progress.advance(1u);
            }
            require_dispatchability_contract(
                program, dispatchability_index);
            if (options.capture_dumps) {
                pass_report.after = emit_ir_text(program);
            }
            report.total_changes += pass_report.changes;
            report.passes.push_back(std::move(pass_report));
        };

    run_pass("constant-folding", options.constant_folding, fold_constants);
    run_pass("copy-propagation", options.copy_propagation, propagate_copies);
    run_pass("dead-code-elimination", options.dead_code_elimination, eliminate_dead_code);
    run_pass(
        "cfg-simplification",
        options.cfg_simplification,
        [&](Function& function) {
            require_valid_function(function);
            const auto result = simplify_function_cfg(
                function, dispatchability_index);
            require_valid_function(function);
            return result;
        });
    run_pass("load-store-simplification", options.load_store_simplification, simplify_load_store);
    optimization_progress.complete();
    return report;
}

} // namespace katana::ir

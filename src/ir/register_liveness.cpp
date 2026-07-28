#include "katana/ir/register_liveness.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace katana::ir {
namespace {

void use_gpr(InstructionRegisterUseDef& result, const std::uint8_t index) noexcept {
    result.uses |= gpr_register_bit(index);
}

void def_gpr(InstructionRegisterUseDef& result, const std::uint8_t index) noexcept {
    result.defs |= gpr_register_bit(index);
}

void use_destination(InstructionRegisterUseDef& result,
                     const Instruction& instruction) noexcept {
    use_gpr(result, instruction.destination_register);
}

void def_destination(InstructionRegisterUseDef& result,
                     const Instruction& instruction) noexcept {
    def_gpr(result, instruction.destination_register);
}

void use_source(InstructionRegisterUseDef& result, const Instruction& instruction) noexcept {
    use_gpr(result, instruction.source_register);
}

void use_binary_and_def_destination(InstructionRegisterUseDef& result,
                                    const Instruction& instruction) noexcept {
    use_destination(result, instruction);
    use_source(result, instruction);
    def_destination(result, instruction);
}

void use_unary_and_def_destination(InstructionRegisterUseDef& result,
                                   const Instruction& instruction) noexcept {
    use_source(result, instruction);
    def_destination(result, instruction);
}

RegisterMask special_register_bit(const SpecialRegister special_register) noexcept {
    switch (special_register) {
    case SpecialRegister::Pr:
        return register_bit(TrackedRegister::Pr);
    case SpecialRegister::Gbr:
        return register_bit(TrackedRegister::Gbr);
    case SpecialRegister::Mach:
        return register_bit(TrackedRegister::Mach);
    case SpecialRegister::Macl:
        return register_bit(TrackedRegister::Macl);
    case SpecialRegister::Fpul:
        return register_bit(TrackedRegister::Fpul);
    case SpecialRegister::None:
    case SpecialRegister::Fpscr:
    case SpecialRegister::Sr:
    case SpecialRegister::Vbr:
    case SpecialRegister::Ssr:
    case SpecialRegister::Spc:
    case SpecialRegister::Sgr:
    case SpecialRegister::Dbr:
    case SpecialRegister::Bank0:
    case SpecialRegister::Bank1:
    case SpecialRegister::Bank2:
    case SpecialRegister::Bank3:
    case SpecialRegister::Bank4:
    case SpecialRegister::Bank5:
    case SpecialRegister::Bank6:
    case SpecialRegister::Bank7:
        return 0u;
    }
    return 0u;
}

void add_status_effects(InstructionRegisterUseDef& result,
                        const StatusRegisterEffects effects) noexcept {
    if (contains_status_bit(effects.reads, StatusRegisterBit::T))
        result.uses |= register_bit(TrackedRegister::T);
    if (contains_status_bit(effects.writes, StatusRegisterBit::T))
        result.defs |= register_bit(TrackedRegister::T);
}

void add_accumulator_effects(InstructionRegisterUseDef& result,
                             const AccumulatorEffects effects) noexcept {
    const auto reads = static_cast<AccumulatorRegister>(
        static_cast<std::uint8_t>(effects.reads_if_s_clear) |
        static_cast<std::uint8_t>(effects.reads_if_s_set));
    const auto writes = static_cast<AccumulatorRegister>(
        static_cast<std::uint8_t>(effects.writes_if_s_clear) |
        static_cast<std::uint8_t>(effects.writes_if_s_set));

    if (contains_accumulator_register(reads, AccumulatorRegister::Mach))
        result.uses |= register_bit(TrackedRegister::Mach);
    if (contains_accumulator_register(reads, AccumulatorRegister::Macl))
        result.uses |= register_bit(TrackedRegister::Macl);
    if (contains_accumulator_register(writes, AccumulatorRegister::Mach))
        result.defs |= register_bit(TrackedRegister::Mach);
    if (contains_accumulator_register(writes, AccumulatorRegister::Macl))
        result.defs |= register_bit(TrackedRegister::Macl);
}

void add_canonical_scalar_effects(InstructionRegisterUseDef& result,
                                  const Instruction& instruction) noexcept {
    add_status_effects(result, instruction.status_effects);
    add_status_effects(
        result, instruction_status_effects(instruction.operation, instruction.special_register));
    add_accumulator_effects(result, instruction.accumulator_effects);
    add_accumulator_effects(
        result, operation_accumulator_effects(instruction.operation, instruction.special_register));
}

} // namespace

InstructionRegisterUseDef
instruction_register_use_def(const Instruction& instruction) noexcept {
    InstructionRegisterUseDef result;

    switch (instruction.operation) {
    case Operation::Unknown:
        // The generated illegal-instruction path is an architectural boundary.
        // Preserve every scalar value if malformed IR reaches localization.
        result.uses = tracked_register_mask;
        result.defs = tracked_register_mask;
        break;

    case Operation::Nop:
    case Operation::ClearS:
    case Operation::SetS:
    case Operation::ClearT:
    case Operation::SetT:
    case Operation::ClearMac:
    case Operation::DivideInitializeUnsigned:
    case Operation::LoadTlb:
    case Operation::Sleep:
    case Operation::ReturnFromException:
    case Operation::TrapAlways:
    case Operation::Branch:
    case Operation::FmovRegister:
    case Operation::Fldi0:
    case Operation::Fldi1:
    case Operation::Fabs:
    case Operation::Fadd:
    case Operation::FcmpEqual:
    case Operation::FcmpGreater:
    case Operation::Fdiv:
    case Operation::Fmac:
    case Operation::Fmul:
    case Operation::Fneg:
    case Operation::Fsqrt:
    case Operation::Fsrra:
    case Operation::Fipr:
    case Operation::Ftrv:
    case Operation::Fsub:
    case Operation::Frchg:
    case Operation::Fschg:
        break;

    case Operation::MovImmediate:
    case Operation::Constant32:
    case Operation::LoadWordSignedPcRelative:
    case Operation::LoadLongPcRelative:
        def_destination(result, instruction);
        break;

    case Operation::MoveAddressPcRelative:
        def_gpr(result, 0u);
        break;

    case Operation::AddImmediate:
    case Operation::DecrementAndTest:
    case Operation::ShiftLogicalLeftOne:
    case Operation::ShiftLogicalRightOne:
    case Operation::ShiftArithmeticLeftOne:
    case Operation::ShiftArithmeticRightOne:
    case Operation::ShiftLogicalLeftTwo:
    case Operation::ShiftLogicalLeftEight:
    case Operation::ShiftLogicalLeftSixteen:
    case Operation::ShiftLogicalRightTwo:
    case Operation::ShiftLogicalRightEight:
    case Operation::ShiftLogicalRightSixteen:
    case Operation::RotateLeft:
    case Operation::RotateRight:
    case Operation::RotateLeftThroughT:
    case Operation::RotateRightThroughT:
        use_destination(result, instruction);
        def_destination(result, instruction);
        break;

    case Operation::MovRegister:
    case Operation::NegateRegister:
    case Operation::NotRegister:
    case Operation::NegateWithCarry:
    case Operation::ExtendUnsignedByte:
    case Operation::ExtendUnsignedWord:
    case Operation::ExtendSignedByte:
    case Operation::ExtendSignedWord:
    case Operation::SwapBytes:
    case Operation::SwapWords:
        use_unary_and_def_destination(result, instruction);
        break;

    case Operation::AddRegister:
    case Operation::SubRegister:
    case Operation::AddWithCarry:
    case Operation::AddWithOverflow:
    case Operation::SubWithCarry:
    case Operation::SubWithOverflow:
    case Operation::ExtractMiddle:
    case Operation::ShiftArithmeticDynamic:
    case Operation::ShiftLogicalDynamic:
    case Operation::DivideStep:
    case Operation::AndRegister:
    case Operation::OrRegister:
    case Operation::XorRegister:
        use_binary_and_def_destination(result, instruction);
        break;

    case Operation::MultiplyLong:
    case Operation::MultiplySignedWord:
    case Operation::MultiplyUnsignedWord:
    case Operation::DoubleMultiplySignedLong:
    case Operation::DoubleMultiplyUnsignedLong:
    case Operation::DivideInitializeSigned:
    case Operation::CompareEqualRegister:
    case Operation::CompareHigherOrSame:
    case Operation::CompareGreaterOrEqual:
    case Operation::CompareHigher:
    case Operation::CompareGreaterThan:
    case Operation::CompareString:
    case Operation::TestRegister:
        use_destination(result, instruction);
        use_source(result, instruction);
        break;

    case Operation::MultiplyAccumulateWord:
    case Operation::MultiplyAccumulateLong:
        use_binary_and_def_destination(result, instruction);
        def_gpr(result, instruction.source_register);
        break;

    case Operation::AndImmediate:
    case Operation::OrImmediate:
    case Operation::XorImmediate:
        use_gpr(result, 0u);
        def_gpr(result, 0u);
        break;

    case Operation::CompareEqualImmediate:
    case Operation::TestImmediate:
        use_gpr(result, 0u);
        break;

    case Operation::ComparePositiveOrZero:
    case Operation::ComparePositive:
        use_destination(result, instruction);
        break;

    case Operation::MoveT:
        def_destination(result, instruction);
        break;

    case Operation::TestByteImmediate:
    case Operation::AndByteImmediate:
    case Operation::XorByteImmediate:
    case Operation::OrByteImmediate:
        use_gpr(result, 0u);
        result.uses |= register_bit(TrackedRegister::Gbr);
        break;

    case Operation::TestAndSetByte:
    case Operation::Prefetch:
    case Operation::Ocbi:
    case Operation::Ocbp:
    case Operation::Ocbwb:
        use_source(result, instruction);
        break;

    case Operation::LoadByteSigned:
    case Operation::LoadWordSigned:
    case Operation::LoadLong:
    case Operation::LoadByteSignedDisplacement:
    case Operation::LoadWordSignedDisplacement:
    case Operation::LoadLongDisplacement:
        use_source(result, instruction);
        if (instruction.forwarded_value_register)
            use_gpr(result, *instruction.forwarded_value_register);
        def_destination(result, instruction);
        break;

    case Operation::StoreByte:
    case Operation::StoreWord:
    case Operation::StoreLong:
    case Operation::StoreByteDisplacement:
    case Operation::StoreWordDisplacement:
    case Operation::StoreLongDisplacement:
        use_destination(result, instruction);
        use_source(result, instruction);
        break;

    case Operation::StoreBytePreDecrement:
    case Operation::StoreWordPreDecrement:
    case Operation::StoreLongPreDecrement:
        use_destination(result, instruction);
        use_source(result, instruction);
        def_destination(result, instruction);
        break;

    case Operation::LoadByteSignedPostIncrement:
    case Operation::LoadWordSignedPostIncrement:
    case Operation::LoadLongPostIncrement:
        use_source(result, instruction);
        def_gpr(result, instruction.source_register);
        def_destination(result, instruction);
        break;

    case Operation::StoreByteR0Indexed:
    case Operation::StoreWordR0Indexed:
    case Operation::StoreLongR0Indexed:
        use_gpr(result, 0u);
        use_destination(result, instruction);
        use_source(result, instruction);
        break;

    case Operation::LoadByteSignedR0Indexed:
    case Operation::LoadWordSignedR0Indexed:
    case Operation::LoadLongR0Indexed:
        use_gpr(result, 0u);
        use_source(result, instruction);
        def_destination(result, instruction);
        break;

    case Operation::StoreByteGbrDisplacement:
    case Operation::StoreWordGbrDisplacement:
    case Operation::StoreLongGbrDisplacement:
        use_gpr(result, 0u);
        result.uses |= register_bit(TrackedRegister::Gbr);
        break;

    case Operation::LoadByteSignedGbrDisplacement:
    case Operation::LoadWordSignedGbrDisplacement:
    case Operation::LoadLongGbrDisplacement:
        result.uses |= register_bit(TrackedRegister::Gbr);
        def_gpr(result, 0u);
        break;

    case Operation::StoreSpecialRegister:
        result.uses |= special_register_bit(instruction.special_register);
        def_destination(result, instruction);
        break;

    case Operation::StoreSpecialRegisterPreDecrement:
        result.uses |= special_register_bit(instruction.special_register);
        use_destination(result, instruction);
        def_destination(result, instruction);
        break;

    case Operation::LoadSpecialRegister:
        use_source(result, instruction);
        result.defs |= special_register_bit(instruction.special_register);
        break;

    case Operation::LoadSpecialRegisterPostIncrement:
        use_source(result, instruction);
        def_gpr(result, instruction.source_register);
        result.defs |= special_register_bit(instruction.special_register);
        break;

    case Operation::MovcaLong:
        use_gpr(result, 0u);
        use_destination(result, instruction);
        break;

    case Operation::FmovLoad:
        use_source(result, instruction);
        break;
    case Operation::FmovLoadPostIncrement:
        use_source(result, instruction);
        def_gpr(result, instruction.source_register);
        break;
    case Operation::FmovLoadR0Indexed:
        use_gpr(result, 0u);
        use_source(result, instruction);
        break;
    case Operation::FmovStore:
        use_destination(result, instruction);
        break;
    case Operation::FmovStorePreDecrement:
        use_destination(result, instruction);
        def_destination(result, instruction);
        break;
    case Operation::FmovStoreR0Indexed:
        use_gpr(result, 0u);
        use_destination(result, instruction);
        break;

    case Operation::Flds:
    case Operation::Ftrc:
    case Operation::FcnvDoubleToSingle:
        result.defs |= register_bit(TrackedRegister::Fpul);
        break;

    case Operation::Fsts:
    case Operation::FloatFromFpul:
    case Operation::Fsca:
    case Operation::FcnvSingleToDouble:
        result.uses |= register_bit(TrackedRegister::Fpul);
        break;

    case Operation::Call:
        result.defs |= register_bit(TrackedRegister::Pr);
        break;
    case Operation::BranchIfTrue:
    case Operation::BranchIfFalse:
        break;
    case Operation::JumpRegister:
        use_gpr(result, instruction.branch_register);
        break;
    case Operation::CallRegister:
        use_gpr(result, instruction.branch_register);
        result.defs |= register_bit(TrackedRegister::Pr);
        break;
    case Operation::Return:
        result.uses |= register_bit(TrackedRegister::Pr);
        break;
    }

    add_canonical_scalar_effects(result, instruction);
    result.uses &= tracked_register_mask;
    result.defs &= tracked_register_mask;
    return result;
}

const BlockRegisterLiveness*
RegisterLocalizationPlan::find_block(const std::uint32_t start_address) const noexcept {
    const auto found = std::find_if(blocks.begin(), blocks.end(), [&](const auto& block) {
        return block.start_address == start_address;
    });
    return found == blocks.end() ? nullptr : &*found;
}

RegisterLocalizationPlan make_register_localization_plan(const Function& function) {
    RegisterLocalizationPlan result;
    result.blocks.reserve(function.blocks.size());

    std::unordered_map<std::uint32_t, std::size_t> block_indexes;
    block_indexes.reserve(function.blocks.size());
    for (std::size_t index = 0u; index < function.blocks.size(); ++index) {
        const auto [ignored, inserted] =
            block_indexes.emplace(function.blocks[index].start_address, index);
        static_cast<void>(ignored);
        if (!inserted)
            throw std::invalid_argument(
                "Register-Liveness erhielt doppelte Basic-Block-Startadresse.");

        BlockRegisterLiveness block_result;
        block_result.start_address = function.blocks[index].start_address;
        for (const auto& instruction : function.blocks[index].instructions) {
            const auto use_def = instruction_register_use_def(instruction);
            block_result.uses_before_def |= use_def.uses & ~block_result.defs;
            block_result.defs |= use_def.defs;
            result.referenced_registers |= use_def.uses | use_def.defs;
        }
        result.blocks.push_back(block_result);
    }

    std::vector<std::vector<std::size_t>> internal_successors(function.blocks.size());
    for (std::size_t index = 0u; index < function.blocks.size(); ++index) {
        const auto& block = function.blocks[index];
        auto& liveness = result.blocks[index];
        liveness.has_open_successor = block.has_indirect_successor;
        for (const auto successor : block.successors) {
            const auto found = block_indexes.find(successor);
            if (found == block_indexes.end()) {
                liveness.has_open_successor = true;
            } else {
                internal_successors[index].push_back(found->second);
            }
        }
        if (liveness.has_open_successor) result.closed_control_flow = false;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t reverse = result.blocks.size(); reverse != 0u; --reverse) {
            const auto index = reverse - 1u;
            auto live_out =
                result.blocks[index].has_open_successor ? tracked_register_mask : RegisterMask{0u};
            for (const auto successor : internal_successors[index])
                live_out |= result.blocks[successor].live_in;

            const auto live_in =
                result.blocks[index].uses_before_def | (live_out & ~result.blocks[index].defs);
            if (live_out != result.blocks[index].live_out ||
                live_in != result.blocks[index].live_in) {
                result.blocks[index].live_out = live_out;
                result.blocks[index].live_in = live_in;
                changed = true;
            }
        }
    }

    for (std::size_t block_index = 0u; block_index < function.blocks.size(); ++block_index) {
        auto live_after = result.blocks[block_index].live_out;
        const auto& instructions = function.blocks[block_index].instructions;
        for (auto instruction = instructions.rbegin(); instruction != instructions.rend();
             ++instruction) {
            const auto use_def = instruction_register_use_def(*instruction);
            // A produced value needed later, or an incoming value consumed more
            // than once, benefits from surviving this native instruction.
            result.candidate_registers |=
                (use_def.defs & live_after) | (use_def.uses & live_after);
            live_after = use_def.uses | (live_after & ~use_def.defs);
        }
    }
    result.candidate_registers &= tracked_register_mask;
    return result;
}

} // namespace katana::ir

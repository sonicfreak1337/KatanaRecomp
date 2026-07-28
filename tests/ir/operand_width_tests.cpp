#include "katana/ir/ir.hpp"
#include "katana/ir/register_liveness.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    using katana::ir::OperandWidth;
    using katana::ir::Operation;
    using katana::ir::operation_operand_widths;

    const auto immediate = operation_operand_widths(Operation::MovImmediate);
    require(immediate.result == OperandWidth::Bits32 && immediate.input == OperandWidth::None &&
                immediate.immediate == OperandWidth::Bits8,
            "MOV Immediate besitzt keine explizite Register- und Immediate-Breite.");

    const auto extension = operation_operand_widths(Operation::ExtendSignedByte);
    require(extension.result == OperandWidth::Bits32 && extension.input == OperandWidth::Bits8,
            "Byte-Erweiterung unterscheidet Quell- und Zielbreite nicht.");

    const auto load = operation_operand_widths(Operation::LoadByteSigned);
    require(load.result == OperandWidth::Bits32 && load.input == OperandWidth::None &&
                load.memory == OperandWidth::Bits8 && load.address == OperandWidth::Bits32,
            "Byte-Load besitzt keine getrennte Speicher- und Registerbreite.");

    const auto displacement = operation_operand_widths(Operation::StoreLongDisplacement);
    require(displacement.result == OperandWidth::None &&
                displacement.input == OperandWidth::Bits32 &&
                displacement.displacement == OperandWidth::Bits4 &&
                displacement.memory == OperandWidth::Bits32 &&
                displacement.address == OperandWidth::Bits32,
            "Register-Displacement-Store besitzt falsche Breiten.");

    const auto comparison = operation_operand_widths(Operation::CompareGreaterThan);
    require(comparison.result == OperandWidth::Bit1 && comparison.input == OperandWidth::Bits32,
            "Vergleich trennt boolesche Ergebnis- und Eingabebreite nicht.");

    const auto branch = operation_operand_widths(Operation::Branch);
    require(branch.displacement == OperandWidth::Bits12 && branch.address == OperandWidth::Bits32,
            "Direkter Branch besitzt keine explizite Displacement- oder Adressbreite.");

    const auto conditional = operation_operand_widths(Operation::BranchIfTrue);
    require(conditional.input == OperandWidth::Bit1 &&
                conditional.displacement == OperandWidth::Bits8,
            "Bedingter Branch besitzt falsche T- oder Displacement-Breite.");

    const auto multiply = operation_operand_widths(Operation::DoubleMultiplySignedLong);
    require(multiply.result == OperandWidth::Bits64 && multiply.input == OperandWidth::Bits32,
            "Doppelte Multiplikation bildet ihre Ergebnisbreite nicht ab.");

    const auto div0s = operation_operand_widths(Operation::DivideInitializeSigned);
    require(div0s.result == OperandWidth::None && div0s.input == OperandWidth::Bits32,
            "DIV0S behauptet eine Ausgabe oder verliert seine 32-Bit-Eingaben.");

    const auto rte = operation_operand_widths(Operation::ReturnFromException);
    require(rte.result == OperandWidth::None && rte.input == OperandWidth::Bits32 &&
                rte.address == OperandWidth::Bits32,
            "RTE besitzt falsche Eingabe- oder Adressbreiten.");

    const auto mac_word = operation_operand_widths(Operation::MultiplyAccumulateWord);
    const auto mac_long = operation_operand_widths(Operation::MultiplyAccumulateLong);
    require(mac_word.result == OperandWidth::None && mac_word.input == OperandWidth::Bits16 &&
                mac_long.result == OperandWidth::None && mac_long.input == OperandWidth::Bits32,
            "MAC.W oder MAC.L behauptet ein pauschales 64-Bit-Ergebnis.");

    const auto mac_word_effects =
        katana::ir::operation_accumulator_effects(Operation::MultiplyAccumulateWord);
    const auto mac_long_effects =
        katana::ir::operation_accumulator_effects(Operation::MultiplyAccumulateLong);
    require(!katana::ir::contains_accumulator_register(mac_word_effects.reads_if_s_set,
                                                       katana::ir::AccumulatorRegister::Mach) &&
                katana::ir::contains_accumulator_register(mac_word_effects.reads_if_s_set,
                                                          katana::ir::AccumulatorRegister::Macl) &&
                katana::ir::contains_accumulator_register(mac_word_effects.writes_if_s_clear,
                                                          katana::ir::AccumulatorRegister::Mach) &&
                !katana::ir::contains_accumulator_register(mac_word_effects.writes_if_s_set,
                                                           katana::ir::AccumulatorRegister::Mach) &&
                katana::ir::contains_accumulator_register(mac_word_effects.writes_if_s_set,
                                                          katana::ir::AccumulatorRegister::Macl) &&
                katana::ir::contains_accumulator_register(mac_long_effects.writes_if_s_set,
                                                          katana::ir::AccumulatorRegister::Mach) &&
                katana::ir::contains_accumulator_register(mac_long_effects.writes_if_s_set,
                                                          katana::ir::AccumulatorRegister::Macl),
            "S-abhaengige MACH/MACL-Schreibwirkungen sind unvollstaendig.");

    const auto mul_l = katana::ir::operation_accumulator_effects(Operation::MultiplyLong);
    const auto muls_w = katana::ir::operation_accumulator_effects(Operation::MultiplySignedWord);
    const auto mulu_w = katana::ir::operation_accumulator_effects(Operation::MultiplyUnsignedWord);
    const auto dmuls_l =
        katana::ir::operation_accumulator_effects(Operation::DoubleMultiplySignedLong);
    const auto dmulu_l =
        katana::ir::operation_accumulator_effects(Operation::DoubleMultiplyUnsignedLong);
    require(katana::ir::contains_accumulator_register(mul_l.writes_if_s_clear,
                                                      katana::ir::AccumulatorRegister::Macl) &&
                katana::ir::contains_accumulator_register(muls_w.writes_if_s_set,
                                                          katana::ir::AccumulatorRegister::Macl) &&
                katana::ir::contains_accumulator_register(mulu_w.writes_if_s_clear,
                                                          katana::ir::AccumulatorRegister::Macl) &&
                katana::ir::contains_accumulator_register(dmuls_l.writes_if_s_clear,
                                                          katana::ir::AccumulatorRegister::Mach) &&
                katana::ir::contains_accumulator_register(dmuls_l.writes_if_s_clear,
                                                          katana::ir::AccumulatorRegister::Macl) &&
                katana::ir::contains_accumulator_register(dmulu_l.writes_if_s_set,
                                                          katana::ir::AccumulatorRegister::Mach) &&
                katana::ir::contains_accumulator_register(dmulu_l.writes_if_s_set,
                                                          katana::ir::AccumulatorRegister::Macl),
            "MUL-/DMUL-Akkumulatorschreibwirkungen sind unvollstaendig.");

    const auto sts_mach = katana::ir::operation_accumulator_effects(
        Operation::StoreSpecialRegister, katana::ir::SpecialRegister::Mach);
    const auto lds_macl = katana::ir::operation_accumulator_effects(
        Operation::LoadSpecialRegister, katana::ir::SpecialRegister::Macl);
    require(katana::ir::contains_accumulator_register(sts_mach.reads_if_s_clear,
                                                      katana::ir::AccumulatorRegister::Mach) &&
                katana::ir::contains_accumulator_register(lds_macl.writes_if_s_set,
                                                          katana::ir::AccumulatorRegister::Macl),
            "Spezialregistertransfers modellieren MACH/MACL nicht.");

    const auto nop = operation_operand_widths(Operation::Nop);
    require(nop.result == OperandWidth::None && nop.input == OperandWidth::None &&
                nop.memory == OperandWidth::None,
            "Operandlose Operation besitzt erfundene Breiten.");

    require(katana::ir::operand_width_name(OperandWidth::Bits32) == "i32" &&
                katana::ir::operand_width_name(OperandWidth::None) == "none",
            "Textnamen der Operandbreiten sind instabil.");

    using katana::ir::TrackedRegister;
    using katana::ir::gpr_register_bit;
    using katana::ir::register_bit;
    using katana::ir::register_mask_contains;

    katana::ir::Instruction addc;
    addc.operation = Operation::AddWithCarry;
    addc.destination_register = 2u;
    addc.source_register = 3u;
    const auto addc_use_def = katana::ir::instruction_register_use_def(addc);
    require((addc_use_def.uses &
             (gpr_register_bit(2u) | gpr_register_bit(3u) |
              register_bit(TrackedRegister::T))) ==
                (gpr_register_bit(2u) | gpr_register_bit(3u) |
                 register_bit(TrackedRegister::T)) &&
                (addc_use_def.defs &
                 (gpr_register_bit(2u) | register_bit(TrackedRegister::T))) ==
                    (gpr_register_bit(2u) | register_bit(TrackedRegister::T)),
            "Register-Use/Def verliert ADDC-Eingaben oder T-/GPR-Ausgaben.");

    katana::ir::Instruction sts_pr;
    sts_pr.operation = Operation::StoreSpecialRegisterPreDecrement;
    sts_pr.destination_register = 5u;
    sts_pr.special_register = katana::ir::SpecialRegister::Pr;
    const auto sts_pr_use_def = katana::ir::instruction_register_use_def(sts_pr);
    require(register_mask_contains(sts_pr_use_def.uses, TrackedRegister::Pr) &&
                (sts_pr_use_def.uses & gpr_register_bit(5u)) != 0u &&
                (sts_pr_use_def.defs & gpr_register_bit(5u)) != 0u,
            "STS.L modelliert PR oder den Predecrement-GPR nicht.");

    katana::ir::Instruction fcnvds;
    fcnvds.operation = Operation::FcnvDoubleToSingle;
    const auto fcnvds_use_def = katana::ir::instruction_register_use_def(fcnvds);
    katana::ir::Instruction fcnvsd;
    fcnvsd.operation = Operation::FcnvSingleToDouble;
    const auto fcnvsd_use_def = katana::ir::instruction_register_use_def(fcnvsd);
    require(register_mask_contains(fcnvds_use_def.defs, TrackedRegister::Fpul) &&
                register_mask_contains(fcnvsd_use_def.uses, TrackedRegister::Fpul),
            "FPUL-Provenienz von FCNVDS/FCNVSD ist vertauscht oder fehlt.");

    katana::ir::Function function;
    function.entry_address = 0x100u;
    katana::ir::BasicBlock entry;
    entry.start_address = 0x100u;
    katana::ir::Instruction move;
    move.operation = Operation::MovRegister;
    move.destination_register = 2u;
    move.source_register = 1u;
    entry.instructions.push_back(move);
    katana::ir::Instruction dead_constant;
    dead_constant.operation = Operation::MovImmediate;
    dead_constant.destination_register = 6u;
    entry.instructions.push_back(dead_constant);
    entry.successors = {0x110u, 0x120u};

    katana::ir::BasicBlock left;
    left.start_address = 0x110u;
    katana::ir::Instruction add;
    add.operation = Operation::AddRegister;
    add.destination_register = 3u;
    add.source_register = 2u;
    left.instructions.push_back(add);
    left.successors = {0x130u};

    katana::ir::BasicBlock right;
    right.start_address = 0x120u;
    katana::ir::Instruction constant;
    constant.operation = Operation::MovImmediate;
    constant.destination_register = 3u;
    right.instructions.push_back(constant);
    right.successors = {0x130u};

    katana::ir::BasicBlock join;
    join.start_address = 0x130u;
    katana::ir::Instruction consume;
    consume.operation = Operation::AddRegister;
    consume.destination_register = 4u;
    consume.source_register = 3u;
    join.instructions.push_back(consume);
    katana::ir::Instruction function_return;
    function_return.operation = Operation::Return;
    join.instructions.push_back(function_return);

    function.blocks = {entry, left, right, join};
    const auto localization = katana::ir::make_register_localization_plan(function);
    const auto* entry_liveness = localization.find_block(0x100u);
    const auto* right_liveness = localization.find_block(0x120u);
    require(localization.closed_control_flow && entry_liveness != nullptr &&
                right_liveness != nullptr &&
                (entry_liveness->live_out & gpr_register_bit(2u)) != 0u &&
                (entry_liveness->live_in & gpr_register_bit(1u)) != 0u &&
                (entry_liveness->live_in & gpr_register_bit(2u)) == 0u &&
                (right_liveness->live_in & gpr_register_bit(3u)) == 0u &&
                register_mask_contains(entry_liveness->live_in, TrackedRegister::Pr) &&
                (localization.general_register_candidates() & (1u << 2u)) != 0u &&
                (localization.general_register_candidates() & (1u << 3u)) != 0u &&
                (localization.referenced_registers & gpr_register_bit(6u)) != 0u &&
                (localization.candidate_registers & gpr_register_bit(6u)) == 0u,
            "CFG-Fixpunkt berechnet Live-In/Live-Out ueber Join oder Definition falsch.");

    katana::ir::Function open_function;
    katana::ir::BasicBlock open_block;
    open_block.start_address = 0x200u;
    open_block.successors = {0xDEADBEEFu};
    open_function.blocks.push_back(open_block);
    const auto open_localization =
        katana::ir::make_register_localization_plan(open_function);
    require(!open_localization.closed_control_flow &&
                open_localization.blocks.front().has_open_successor &&
                open_localization.blocks.front().live_out ==
                    katana::ir::tracked_register_mask,
            "Offene CFG-Kante wird nicht konservativ als vollstaendig live behandelt.");

    std::cout << "KR-1901 Explizite Operandbreiten erfolgreich.\n";
    return EXIT_SUCCESS;
}

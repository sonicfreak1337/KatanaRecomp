#include "../../src/analysis/guarded_native_entry_shape.hpp"
#include "../../src/analysis/return_object_callback_inventory.hpp"

#include "katana/io/executable_image.hpp"
#include "katana/ir/ir.hpp"
#include "katana/sh4/disassembler.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture final {
    katana::io::ExecutableImage image;
    std::vector<katana::sh4::DisassemblyLine> decoded_lines;
    std::vector<katana::ir::Function> program;
};

katana::sh4::DisassemblyLine decoded_line(
    const std::uint32_t address,
    const std::uint16_t opcode,
    const katana::sh4::InstructionKind kind,
    const std::uint8_t destination = 0u,
    const std::uint8_t source = 0u,
    const katana::sh4::ControlFlowKind control_flow =
        katana::sh4::ControlFlowKind::None,
    const bool has_delay_slot = false,
    const bool is_delay_slot = false,
    const std::uint8_t branch_register = 0u) {
    katana::sh4::DisassemblyLine line;
    line.address = address;
    line.opcode = opcode;
    line.instruction.opcode = opcode;
    line.instruction.kind = kind;
    line.instruction.destination_register = destination;
    line.instruction.source_register = source;
    line.instruction.branch_register = branch_register;
    line.instruction.control_flow = control_flow;
    line.instruction.has_delay_slot = has_delay_slot;
    line.is_delay_slot = is_delay_slot;
    return line;
}

katana::ir::Instruction ir_instruction(
    const std::uint32_t address,
    const katana::ir::Operation operation,
    const std::uint8_t destination = 0u,
    const std::uint8_t source = 0u,
    const std::int32_t displacement = 0,
    const std::uint8_t branch_register = 0u) {
    katana::ir::Instruction instruction;
    instruction.source_address = address;
    instruction.operation = operation;
    instruction.original_operation = operation;
    instruction.destination_register = destination;
    instruction.source_register = source;
    instruction.displacement = displacement;
    instruction.branch_register = branch_register;
    return instruction;
}

enum class DelayBehavior {
    Nop,
    KillR0,
    MoveR0ToR4,
    LoadCallbackLiteralIntoR3,
};

Fixture make_literal_jsr_fixture(const bool gpr_clobber_before_call,
                                  const DelayBehavior delay_behavior) {
    Fixture fixture;
    std::vector<std::uint8_t> bytes(0x80u, 0u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            bytes[offset + byte] =
                static_cast<std::uint8_t>(value >> (byte * 8u));
    };
    put_u16(0x60u, 0x000Bu); // target rts
    put_u16(0x62u, 0x0009u); // target delay nop
    put_u32(0x58u, 0x8C09846Eu); // immutable JSR callee literal
    put_u32(0x5Cu, 0x1060u); // immutable callback target literal
    fixture.image.add_segment({".literal-jsr-fixture",
                               0x1000u,
                               0u,
                               bytes.size(),
                               katana::io::SegmentKind::Mixed,
                               {true, true, true},
                               std::move(bytes)});
    fixture.image.add_immutable_range(
        {0x1000u, 0x80u, "literal-jsr-fixture-v1", 0u});

    fixture.decoded_lines.push_back(decoded_line(
        0x1000u, 0xD32Au,
        katana::sh4::InstructionKind::MovLongLoadPcRelative, 3u));
    if (gpr_clobber_before_call) {
        fixture.decoded_lines.push_back(decoded_line(
            0x1002u, 0x7301u,
            katana::sh4::InstructionKind::AddImmediate, 3u));
    } else {
        fixture.decoded_lines.push_back(decoded_line(
            0x1002u, 0x0009u, katana::sh4::InstructionKind::Nop));
    }
    fixture.decoded_lines.push_back(decoded_line(
        0x1004u, 0x430Bu, katana::sh4::InstructionKind::Jsr, 0u, 0u,
        katana::sh4::ControlFlowKind::IndirectCall, true, false, 3u));
    if (delay_behavior == DelayBehavior::KillR0) {
        fixture.decoded_lines.push_back(decoded_line(
            0x1006u, 0xE001u,
            katana::sh4::InstructionKind::MovImmediate, 0u, 0u,
            katana::sh4::ControlFlowKind::None, false, true));
    } else if (delay_behavior == DelayBehavior::MoveR0ToR4) {
        fixture.decoded_lines.push_back(decoded_line(
            0x1006u, 0x6403u,
            katana::sh4::InstructionKind::MovRegister, 4u, 0u,
            katana::sh4::ControlFlowKind::None, false, true));
    } else if (delay_behavior == DelayBehavior::LoadCallbackLiteralIntoR3) {
        fixture.decoded_lines.push_back(decoded_line(
            0x1006u, 0xD315u,
            katana::sh4::InstructionKind::MovLongLoadPcRelative, 3u, 0u,
            katana::sh4::ControlFlowKind::None, false, true));
    } else {
        fixture.decoded_lines.push_back(decoded_line(
            0x1006u, 0x0009u, katana::sh4::InstructionKind::Nop, 0u, 0u,
            katana::sh4::ControlFlowKind::None, false, true));
    }
    const bool delay_supplies_receiver =
        delay_behavior == DelayBehavior::MoveR0ToR4;
    if (!delay_supplies_receiver)
        fixture.decoded_lines.push_back(decoded_line(
            0x1008u, 0x6403u, katana::sh4::InstructionKind::MovRegister, 4u,
            0u));
    const bool delay_supplies_callback_literal =
        delay_behavior == DelayBehavior::LoadCallbackLiteralIntoR3;
    if (!delay_supplies_callback_literal)
        fixture.decoded_lines.push_back(decoded_line(
            delay_supplies_receiver ? 0x1008u : 0x100Au, 0xD329u,
            katana::sh4::InstructionKind::MovLongLoadPcRelative, 3u));
    fixture.decoded_lines.push_back(decoded_line(
        delay_supplies_callback_literal
            ? 0x100Au
            : (delay_supplies_receiver ? 0x100Au : 0x100Cu),
        0x1436u, katana::sh4::InstructionKind::MovLongStoreDisplacement, 4u,
        3u));

    katana::ir::BasicBlock block;
    block.start_address = 0x1000u;
    auto pc_callee = ir_instruction(
        0x1000u, katana::ir::Operation::LoadLongPcRelative, 3u);
    pc_callee.effective_address = 0x1058u;
    block.instructions.push_back(pc_callee);
    if (gpr_clobber_before_call)
        block.instructions.push_back(ir_instruction(
            0x1002u, katana::ir::Operation::AddImmediate, 3u));
    else
        block.instructions.push_back(
            ir_instruction(0x1002u, katana::ir::Operation::Nop));
    auto call = ir_instruction(
        0x1004u, katana::ir::Operation::CallRegister, 0u, 0u, 0, 3u);
    call.delay_slot = {katana::ir::DelaySlotRole::Owner, 0x1006u};
    block.instructions.push_back(call);
    auto delay = ir_instruction(
        0x1006u,
        delay_behavior == DelayBehavior::KillR0
            ? katana::ir::Operation::MovImmediate
            : (delay_behavior == DelayBehavior::MoveR0ToR4
                   ? katana::ir::Operation::MovRegister
                   : (delay_behavior == DelayBehavior::LoadCallbackLiteralIntoR3
                          ? katana::ir::Operation::LoadLongPcRelative
                          : katana::ir::Operation::Nop)),
        delay_behavior == DelayBehavior::MoveR0ToR4 ? 4u
            : (delay_behavior == DelayBehavior::LoadCallbackLiteralIntoR3
                   ? 3u
                   : 0u),
        delay_behavior == DelayBehavior::MoveR0ToR4 ? 0u : 0u);
    if (delay_behavior == DelayBehavior::LoadCallbackLiteralIntoR3)
        delay.effective_address = 0x105Cu;
    delay.delay_slot = {katana::ir::DelaySlotRole::Slot, 0x1004u};
    block.instructions.push_back(delay);
    if (!delay_supplies_receiver)
        block.instructions.push_back(ir_instruction(
            0x1008u, katana::ir::Operation::MovRegister, 4u, 0u));
    if (!delay_supplies_callback_literal) {
        auto pc_callback = ir_instruction(
            delay_supplies_receiver ? 0x1008u : 0x100Au,
            katana::ir::Operation::LoadLongPcRelative, 3u);
        pc_callback.effective_address = 0x105Cu;
        block.instructions.push_back(pc_callback);
    }
    block.instructions.push_back(ir_instruction(
        delay_supplies_callback_literal
            ? 0x100Au
            : (delay_supplies_receiver ? 0x100Au : 0x100Cu),
        katana::ir::Operation::StoreLongDisplacement, 4u, 3u, 24));
    fixture.program.push_back({});
    fixture.program.front().entry_address = 0x1000u;
    fixture.program.front().blocks.push_back(std::move(block));
    return fixture;
}

Fixture make_fixture(const bool fpu_write,
                     const bool gpr_clobber,
                     const bool stack_receiver,
                     const bool indirect_call) {
    Fixture fixture;
    std::vector<std::uint8_t> bytes(0x80u, 0u);
    const auto put_u16 = [&bytes](const std::size_t offset,
                                  const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1u] = static_cast<std::uint8_t>(value >> 8u);
    };
    const auto put_u32 = [&bytes](const std::size_t offset,
                                  const std::uint32_t value) {
        for (std::size_t byte = 0u; byte < 4u; ++byte)
            bytes[offset + byte] =
                static_cast<std::uint8_t>(value >> (byte * 8u));
    };
    // The bytes are only needed for the immutable PC literal and the guarded
    // target-shape check; the producer sequence itself is supplied below as
    // CFA's already decoded lines/IR.
    put_u16(0x20u, 0x000Bu); // rts
    put_u16(0x22u, 0x0009u); // delay nop
    put_u32(0x1Cu, 0x1020u); // PC-relative callback target literal
    fixture.image.add_segment({".return-object-fixture",
                               0x1000u,
                               0u,
                               bytes.size(),
                               katana::io::SegmentKind::Mixed,
                               {true, true, true},
                               std::move(bytes)});
    fixture.image.add_immutable_range(
        {0x1000u, 0x80u, "return-object-fixture-v1", 0u});

    const auto receiver = static_cast<std::uint8_t>(
        stack_receiver ? 15u : 4u);
    fixture.decoded_lines.push_back(decoded_line(
        0x1000u, indirect_call ? 0x400Bu : 0xB00Eu,
        indirect_call ? katana::sh4::InstructionKind::Jsr
                      : katana::sh4::InstructionKind::Bsr,
        0u, 0u,
        indirect_call ? katana::sh4::ControlFlowKind::IndirectCall
                      : katana::sh4::ControlFlowKind::Call,
        true));
    fixture.decoded_lines.push_back(decoded_line(
        0x1002u, 0x0009u, katana::sh4::InstructionKind::Nop, 0u, 0u,
        katana::sh4::ControlFlowKind::None, false, true));
    fixture.decoded_lines.push_back(decoded_line(
        0x1004u, 0x6403u, katana::sh4::InstructionKind::MovRegister,
        receiver, 0u));
    if (fpu_write) {
        fixture.decoded_lines.push_back(decoded_line(
            0x1006u, 0xF00Cu, katana::sh4::InstructionKind::FmovRegister,
            receiver, receiver));
    } else if (gpr_clobber) {
        fixture.decoded_lines.push_back(decoded_line(
            0x1006u, 0x7401u, katana::sh4::InstructionKind::AddImmediate,
            receiver, 0u));
    }
    const auto pc_load_address = fpu_write || gpr_clobber ? 0x1008u : 0x1006u;
    const auto store_address = pc_load_address + 2u;
    fixture.decoded_lines.push_back(decoded_line(
        pc_load_address, 0xD305u,
        katana::sh4::InstructionKind::MovLongLoadPcRelative, 3u, 0u));
    fixture.decoded_lines.push_back(decoded_line(
        store_address, 0x1436u,
        katana::sh4::InstructionKind::MovLongStoreDisplacement,
        receiver, 3u));

    auto call = ir_instruction(
        0x1000u,
        indirect_call ? katana::ir::Operation::CallRegister
                      : katana::ir::Operation::Call);
    if (!indirect_call) call.target_address = 0x1020u;
    call.delay_slot = {katana::ir::DelaySlotRole::Owner, 0x1002u};
    auto delay = ir_instruction(0x1002u, katana::ir::Operation::Nop);
    delay.delay_slot = {katana::ir::DelaySlotRole::Slot, 0x1000u};
    auto move = ir_instruction(0x1004u, katana::ir::Operation::MovRegister,
                               receiver, 0u);
    fixture.program.push_back({});
    fixture.program.front().entry_address = 0x1000u;
    katana::ir::BasicBlock block;
    block.start_address = 0x1000u;
    block.instructions = {call, delay, move};
    if (fpu_write)
        block.instructions.push_back(ir_instruction(
            0x1006u, katana::ir::Operation::FmovRegister, receiver,
            receiver));
    else if (gpr_clobber)
        block.instructions.push_back(ir_instruction(
            0x1006u, katana::ir::Operation::AddImmediate, receiver, 0));
    auto pc_load = ir_instruction(
        pc_load_address, katana::ir::Operation::LoadLongPcRelative, 3u);
    pc_load.effective_address = 0x101Cu;
    block.instructions.push_back(pc_load);
    block.instructions.push_back(ir_instruction(
        store_address, katana::ir::Operation::StoreLongDisplacement,
        receiver, 3u, 24));
    fixture.program.front().blocks.push_back(std::move(block));
    return fixture;
}

std::vector<katana::codegen::LatentAotReturnObjectCallbackCandidate> scan(
    const Fixture& fixture) {
    katana::analysis::detail::GuardedNativeEntryShapeCache shapes{
        fixture.image};
    return katana::analysis::detail::discover_return_object_callback_candidates(
        fixture.image, fixture.decoded_lines, fixture.program, shapes);
}

void test_positive_and_fpu_preservation() {
    auto fixture = make_fixture(true, false, false, false);
    const auto candidates = scan(fixture);
    require(candidates.size() == 1u,
            "Die direkte Return/Object-Producersequenz wurde nicht erkannt.");
    const auto& candidate = candidates.front();
    require(candidate.function_address == 0x1000u &&
                candidate.block_address == 0x1000u &&
                candidate.call_instruction_address == 0x1000u &&
                candidate.callee_address == 0x1020u &&
                candidate.return_register == 0u &&
                candidate.object_register == 4u &&
                candidate.literal_address == 0x101Cu &&
                candidate.literal_value == 0x1020u &&
                candidate.store_instruction_address == 0x100Au &&
                candidate.field_displacement == 24 &&
                candidate.target_address == 0x1020u &&
                candidate.target_shape_valid && !candidate.complete &&
                !candidate.strict_eligible && !candidate.execution_eligible &&
                candidate.reason == "return-object-alias-unproven",
            "Die Return/Object-Diagnoseevidence besitzt falsche Felder.");
}

void test_fail_closed_variants() {
    require(scan(make_fixture(false, true, false, false)).empty(),
            "Ein GPR-Clobber liess unberechtigte Return-Provenienz stehen.");
    require(scan(make_fixture(false, false, true, false)).empty(),
            "Ein Stack-Receiver wurde als persistentes Objekt inventarisiert.");
    require(scan(make_fixture(false, false, false, true)).empty(),
            "Ein indirekter Call wurde als direkter Return-Producer inventarisiert.");
}

void test_literal_jsr_resolution_and_delay_order() {
    const auto candidates =
        scan(make_literal_jsr_fixture(false, DelayBehavior::Nop));
    require(candidates.size() == 1u,
            "Der PC-Literal-JSR-Producer wurde nicht erkannt.");
    const auto& candidate = candidates.front();
    require(candidate.function_address == 0x1000u &&
                candidate.call_instruction_address == 0x1004u &&
                candidate.callee_address == 0x8C09846Eu &&
                candidate.object_register == 4u &&
                candidate.literal_address == 0x105Cu &&
                candidate.literal_value == 0x1060u &&
                candidate.store_instruction_address == 0x100Cu &&
                candidate.field_displacement == 24 && !candidate.complete &&
                !candidate.strict_eligible && !candidate.execution_eligible,
            "Die aufgeloeste JSR-Evidence besitzt falsche Felder.");
    require(scan(make_literal_jsr_fixture(true, DelayBehavior::Nop)).empty(),
            "Ein GPR-Clobber vor dem JSR wurde nicht verworfen.");
    require(scan(make_literal_jsr_fixture(false, DelayBehavior::KillR0)).size() ==
                1u,
            "Ein r0-Clobber im JSR-Delay-Slot loeschte den spaeteren Return.");
    require(scan(make_literal_jsr_fixture(false, DelayBehavior::MoveR0ToR4))
                .empty(),
            "Ein Delay-Slot-Kopiealias wurde nach dem unbekannten Callee "
            "weitergetragen.");
    require(scan(make_literal_jsr_fixture(
                      false, DelayBehavior::LoadCallbackLiteralIntoR3))
                .empty(),
            "Ein Delay-Slot-Literal wurde nach dem unbekannten Callee "
            "weitergetragen.");
}

} // namespace

int main() {
    try {
        test_positive_and_fpu_preservation();
        test_fail_closed_variants();
        test_literal_jsr_resolution_and_delay_order();
        std::cout << "Return/Object-Callback-Diagnose erfolgreich.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << error.what() << '\n';
        return 1;
    }
}

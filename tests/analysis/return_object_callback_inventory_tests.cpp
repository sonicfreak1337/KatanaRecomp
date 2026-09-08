#include "../../src/analysis/guarded_native_entry_shape.hpp"
#include "../../src/analysis/return_object_callback_inventory.hpp"
#include "../../src/analysis/returned_receiver_diagnostic.hpp"

#include "katana/io/executable_image.hpp"
#include "katana/ir/ir.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
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

struct ReturnedReceiverFixture final {
    katana::io::ExecutableImage image;
    std::vector<katana::ir::Function> program;
    std::vector<katana::sh4::DisassemblyLine> decoded_lines;
};

void append_returned_receiver_decoded(
    std::vector<katana::sh4::DisassemblyLine>& lines,
    const std::uint32_t address,
    const katana::sh4::InstructionKind kind,
    const katana::sh4::ControlFlowKind control_flow =
        katana::sh4::ControlFlowKind::None,
    const bool has_delay_slot = false,
    const bool is_delay_slot = false,
    const std::uint8_t destination = 0u,
    const std::uint8_t source = 0u,
    const std::uint8_t branch_register = 0u) {
    lines.push_back(decoded_line(
        address, 0u, kind, destination, source, control_flow,
        has_delay_slot, is_delay_slot, branch_register));
}

katana::ir::Instruction returned_receiver_call(
    const std::uint32_t address,
    const std::uint32_t target,
    const katana::ir::Operation operation = katana::ir::Operation::Call) {
    auto instruction = ir_instruction(address, operation);
    instruction.target_address = target;
    instruction.delay_slot = {katana::ir::DelaySlotRole::Owner, address + 2u};
    return instruction;
}

katana::ir::Instruction returned_receiver_delay(
    const std::uint32_t address,
    const std::uint32_t owner_address) {
    auto instruction = ir_instruction(address, katana::ir::Operation::Nop);
    instruction.delay_slot = {katana::ir::DelaySlotRole::Slot, owner_address};
    return instruction;
}

katana::ir::Instruction returned_receiver_return(
    const std::uint32_t address) {
    auto instruction = ir_instruction(address, katana::ir::Operation::Return);
    instruction.delay_slot = {katana::ir::DelaySlotRole::Owner, address + 2u};
    return instruction;
}

ReturnedReceiverFixture make_returned_receiver_fixture() {
    ReturnedReceiverFixture fixture;
    fixture.image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);

    katana::ir::Function producer;
    producer.entry_address = 0x1000u;
    katana::ir::BasicBlock producer_entry;
    producer_entry.start_address = 0x1000u;
    producer_entry.instructions.push_back(
        returned_receiver_call(0x1000u, 0x2000u));
    producer_entry.instructions.push_back(
        returned_receiver_delay(0x1002u, 0x1000u));
    producer_entry.instructions.push_back(ir_instruction(
        0x1004u, katana::ir::Operation::MovRegister, 14u, 0u));
    auto producer_branch = ir_instruction(
        0x1006u, katana::ir::Operation::BranchIfTrue);
    producer_branch.target_address = 0x1010u;
    producer_branch.delay_slot = {katana::ir::DelaySlotRole::Owner, 0x1008u};
    producer_entry.instructions.push_back(producer_branch);
    producer_entry.instructions.push_back(
        returned_receiver_delay(0x1008u, 0x1006u));
    producer_entry.successors = {0x1010u, 0x1020u};
    producer.blocks.push_back(std::move(producer_entry));

    katana::ir::BasicBlock producer_return;
    producer_return.start_address = 0x1010u;
    producer_return.instructions.push_back(ir_instruction(
        0x1010u, katana::ir::Operation::MovRegister, 0u, 14u));
    producer_return.instructions.push_back(returned_receiver_return(0x1012u));
    producer_return.instructions.push_back(
        returned_receiver_delay(0x1014u, 0x1012u));
    producer.blocks.push_back(std::move(producer_return));

    katana::ir::BasicBlock producer_null;
    producer_null.start_address = 0x1020u;
    auto null_value =
        ir_instruction(0x1020u, katana::ir::Operation::MovImmediate, 0u);
    null_value.immediate = 0;
    producer_null.instructions.push_back(null_value);
    producer_null.instructions.push_back(returned_receiver_return(0x1022u));
    producer_null.instructions.push_back(
        returned_receiver_delay(0x1024u, 0x1022u));
    producer.blocks.push_back(std::move(producer_null));
    fixture.program.push_back(std::move(producer));

    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1000u, katana::sh4::InstructionKind::Bsr,
        katana::sh4::ControlFlowKind::Call, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1002u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1004u,
        katana::sh4::InstructionKind::MovRegister);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1006u, katana::sh4::InstructionKind::Bt,
        katana::sh4::ControlFlowKind::ConditionalBranch, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1008u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1010u,
        katana::sh4::InstructionKind::MovRegister);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1012u, katana::sh4::InstructionKind::Rts,
        katana::sh4::ControlFlowKind::Return, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1014u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1020u,
        katana::sh4::InstructionKind::MovImmediate);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1022u, katana::sh4::InstructionKind::Rts,
        katana::sh4::ControlFlowKind::Return, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1024u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);

    katana::ir::Function consumer;
    consumer.entry_address = 0x3000u;
    katana::ir::BasicBlock consumer_block;
    consumer_block.start_address = 0x3000u;
    consumer_block.instructions.push_back(
        returned_receiver_call(0x3000u, 0x2000u));
    consumer_block.instructions.push_back(
        returned_receiver_delay(0x3002u, 0x3000u));
    consumer_block.instructions.push_back(ir_instruction(
        0x3004u, katana::ir::Operation::MovRegister, 4u, 0u));
    consumer_block.instructions.push_back(ir_instruction(
        0x3006u, katana::ir::Operation::LoadLongDisplacement, 3u, 4u, 24));
    auto consumer_callback = ir_instruction(
        0x3008u, katana::ir::Operation::CallRegister, 0u, 0u, 0, 3u);
    consumer_callback.delay_slot = {
        katana::ir::DelaySlotRole::Owner, 0x300Au};
    consumer_block.instructions.push_back(consumer_callback);
    consumer_block.instructions.push_back(
        returned_receiver_delay(0x300Au, 0x3008u));
    consumer.blocks.push_back(std::move(consumer_block));
    fixture.program.push_back(std::move(consumer));

    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x3000u, katana::sh4::InstructionKind::Bsr,
        katana::sh4::ControlFlowKind::Call, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x3002u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x3004u,
        katana::sh4::InstructionKind::MovRegister);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x3006u,
        katana::sh4::InstructionKind::MovLongLoadDisplacement);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x3008u, katana::sh4::InstructionKind::Jsr,
        katana::sh4::ControlFlowKind::IndirectCall, true, false, 0u, 0u,
        3u);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x300Au, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);

    katana::ir::Function mismatched;
    mismatched.entry_address = 0x4000u;
    katana::ir::BasicBlock mismatched_entry;
    mismatched_entry.start_address = 0x4000u;
    mismatched_entry.instructions.push_back(
        returned_receiver_call(0x4000u, 0x2000u));
    mismatched_entry.instructions.push_back(
        returned_receiver_delay(0x4002u, 0x4000u));
    mismatched_entry.instructions.push_back(ir_instruction(
        0x4004u, katana::ir::Operation::MovRegister, 14u, 0u));
    auto mismatched_branch = ir_instruction(
        0x4006u, katana::ir::Operation::BranchIfTrue);
    mismatched_branch.target_address = 0x4010u;
    mismatched_branch.delay_slot = {katana::ir::DelaySlotRole::Owner,
                                    0x4008u};
    mismatched_entry.instructions.push_back(mismatched_branch);
    mismatched_entry.instructions.push_back(
        returned_receiver_delay(0x4008u, 0x4006u));
    mismatched_entry.successors = {0x4010u, 0x4020u};
    mismatched.blocks.push_back(std::move(mismatched_entry));

    katana::ir::BasicBlock matching_return;
    matching_return.start_address = 0x4010u;
    matching_return.instructions.push_back(ir_instruction(
        0x4010u, katana::ir::Operation::MovRegister, 0u, 14u));
    matching_return.instructions.push_back(returned_receiver_return(0x4012u));
    matching_return.instructions.push_back(
        returned_receiver_delay(0x4014u, 0x4012u));
    mismatched.blocks.push_back(std::move(matching_return));

    katana::ir::BasicBlock alternate_return;
    alternate_return.start_address = 0x4020u;
    alternate_return.instructions.push_back(
        returned_receiver_call(0x4020u, 0x2100u));
    alternate_return.instructions.push_back(
        returned_receiver_delay(0x4022u, 0x4020u));
    alternate_return.instructions.push_back(returned_receiver_return(0x4024u));
    alternate_return.instructions.push_back(
        returned_receiver_delay(0x4026u, 0x4024u));
    mismatched.blocks.push_back(std::move(alternate_return));
    fixture.program.push_back(std::move(mismatched));

    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x4000u, katana::sh4::InstructionKind::Bsr,
        katana::sh4::ControlFlowKind::Call, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x4002u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x4004u,
        katana::sh4::InstructionKind::MovRegister);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x4006u, katana::sh4::InstructionKind::Bt,
        katana::sh4::ControlFlowKind::ConditionalBranch, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x4008u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x4010u,
        katana::sh4::InstructionKind::MovRegister);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x4012u, katana::sh4::InstructionKind::Rts,
        katana::sh4::ControlFlowKind::Return, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x4014u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x4020u, katana::sh4::InstructionKind::Bsr,
        katana::sh4::ControlFlowKind::Call, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x4022u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x4024u, katana::sh4::InstructionKind::Rts,
        katana::sh4::ControlFlowKind::Return, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x4026u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);

    return fixture;
}

ReturnedReceiverFixture make_returned_receiver_jsr_fixture(
    const bool delay_clobbers_r14 = false) {
    ReturnedReceiverFixture fixture;
    fixture.image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    std::vector<std::uint8_t> bytes(0x200u, 0u);
    const std::uint32_t callee = 0x2000u;
    for (std::size_t byte = 0u; byte < sizeof(callee); ++byte)
        bytes[0x158u + byte] =
            static_cast<std::uint8_t>(callee >> (byte * 8u));
    fixture.image.add_segment({".returned-receiver-jsr-fixture",
                               0x1000u,
                               0u,
                               bytes.size(),
                               katana::io::SegmentKind::Mixed,
                               {true, true, true},
                               std::move(bytes)});
    fixture.image.add_immutable_range(
        {0x1000u, 0x200u, "returned-receiver-jsr-fixture-v1", 0u});

    katana::ir::Function function;
    function.entry_address = 0x1000u;
    katana::ir::BasicBlock entry;
    entry.start_address = 0x1000u;
    auto literal = ir_instruction(
        0x1000u, katana::ir::Operation::LoadLongPcRelative, 3u);
    literal.effective_address = 0x1158u;
    entry.instructions.push_back(literal);
    auto jsr = ir_instruction(
        0x1002u, katana::ir::Operation::CallRegister, 0u, 0u, 0, 3u);
    jsr.delay_slot = {katana::ir::DelaySlotRole::Owner, 0x1004u};
    entry.instructions.push_back(jsr);
    entry.instructions.push_back(
        returned_receiver_delay(0x1004u, 0x1002u));
    entry.instructions.push_back(ir_instruction(
        0x1006u, katana::ir::Operation::MovRegister, 14u, 0u));
    entry.instructions.push_back(returned_receiver_call(0x1008u, 0x2010u));
    if (delay_clobbers_r14) {
        auto delay = ir_instruction(
            0x100Au, katana::ir::Operation::MovImmediate, 14u);
        delay.immediate = 0;
        delay.delay_slot = {katana::ir::DelaySlotRole::Slot, 0x1008u};
        entry.instructions.push_back(delay);
    } else {
        entry.instructions.push_back(
            returned_receiver_delay(0x100Au, 0x1008u));
    }
    auto branch = ir_instruction(
        0x100Cu, katana::ir::Operation::BranchIfTrue);
    branch.target_address = 0x1010u;
    branch.delay_slot = {katana::ir::DelaySlotRole::Owner, 0x100Eu};
    entry.instructions.push_back(branch);
    entry.instructions.push_back(
        returned_receiver_delay(0x100Eu, 0x100Cu));
    entry.successors = {0x1010u, 0x1020u};
    function.blocks.push_back(std::move(entry));

    katana::ir::BasicBlock object_return;
    object_return.start_address = 0x1010u;
    object_return.instructions.push_back(ir_instruction(
        0x1010u, katana::ir::Operation::MovRegister, 0u, 14u));
    object_return.instructions.push_back(returned_receiver_return(0x1012u));
    object_return.instructions.push_back(
        returned_receiver_delay(0x1014u, 0x1012u));
    function.blocks.push_back(std::move(object_return));

    katana::ir::BasicBlock null_return;
    null_return.start_address = 0x1020u;
    auto null_value =
        ir_instruction(0x1020u, katana::ir::Operation::MovImmediate, 0u);
    null_value.immediate = 0;
    null_return.instructions.push_back(null_value);
    null_return.instructions.push_back(returned_receiver_return(0x1022u));
    null_return.instructions.push_back(
        returned_receiver_delay(0x1024u, 0x1022u));
    function.blocks.push_back(std::move(null_return));
    fixture.program.push_back(std::move(function));

    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1000u,
        katana::sh4::InstructionKind::MovLongLoadPcRelative);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1002u, katana::sh4::InstructionKind::Jsr,
        katana::sh4::ControlFlowKind::IndirectCall, true, false, 0u, 0u,
        3u);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1004u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1006u,
        katana::sh4::InstructionKind::MovRegister);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1008u, katana::sh4::InstructionKind::Bsr,
        katana::sh4::ControlFlowKind::Call, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x100Au,
        delay_clobbers_r14 ? katana::sh4::InstructionKind::MovImmediate
                           : katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true,
        delay_clobbers_r14 ? 14u : 0u);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x100Cu, katana::sh4::InstructionKind::Bt,
        katana::sh4::ControlFlowKind::ConditionalBranch, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x100Eu, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1010u,
        katana::sh4::InstructionKind::MovRegister);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1012u, katana::sh4::InstructionKind::Rts,
        katana::sh4::ControlFlowKind::Return, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1014u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1020u,
        katana::sh4::InstructionKind::MovImmediate);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1022u, katana::sh4::InstructionKind::Rts,
        katana::sh4::ControlFlowKind::Return, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1024u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    return fixture;
}

ReturnedReceiverFixture make_direct_return_fixture() {
    ReturnedReceiverFixture fixture;
    katana::ir::Function function;
    function.entry_address = 0x1100u;
    katana::ir::BasicBlock block;
    block.start_address = 0x1100u;
    block.instructions.push_back(
        returned_receiver_call(0x1100u, 0x2200u));
    block.instructions.push_back(
        returned_receiver_delay(0x1102u, 0x1100u));
    block.instructions.push_back(returned_receiver_return(0x1104u));
    block.instructions.push_back(
        returned_receiver_delay(0x1106u, 0x1104u));
    function.blocks.push_back(std::move(block));
    fixture.program.push_back(std::move(function));

    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1100u, katana::sh4::InstructionKind::Bsr,
        katana::sh4::ControlFlowKind::Call, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1102u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1104u, katana::sh4::InstructionKind::Rts,
        katana::sh4::ControlFlowKind::Return, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x1106u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    return fixture;
}
ReturnedReceiverFixture make_returned_receiver_tailcall_fixture() {
    ReturnedReceiverFixture fixture;
    fixture.image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);

    katana::ir::Function function;
    function.entry_address = 0x5000u;
    katana::ir::BasicBlock block;
    block.start_address = 0x5000u;
    block.instructions.push_back(
        returned_receiver_call(0x5000u, 0x6000u));
    block.instructions.push_back(
        returned_receiver_delay(0x5002u, 0x5000u));
    block.instructions.push_back(ir_instruction(
        0x5004u, katana::ir::Operation::MovRegister, 4u, 0u));
    auto jump = ir_instruction(
        0x5006u, katana::ir::Operation::JumpRegister, 0u, 0u, 0, 3u);
    jump.delay_slot = {katana::ir::DelaySlotRole::Owner, 0x5008u};
    block.instructions.push_back(jump);
    block.instructions.push_back(
        returned_receiver_delay(0x5008u, 0x5006u));
    function.blocks.push_back(std::move(block));
    fixture.program.push_back(std::move(function));

    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x5000u, katana::sh4::InstructionKind::Bsr,
        katana::sh4::ControlFlowKind::Call, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x5002u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x5004u,
        katana::sh4::InstructionKind::MovRegister);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x5006u, katana::sh4::InstructionKind::Jmp,
        katana::sh4::ControlFlowKind::IndirectBranch, true, false, 0u, 0u,
        3u);
    append_returned_receiver_decoded(
        fixture.decoded_lines, 0x5008u, katana::sh4::InstructionKind::Nop,
        katana::sh4::ControlFlowKind::None, false, true);
    return fixture;
}
void test_returned_receiver_domain() {
    const auto fixture = make_returned_receiver_fixture();
    using StaticCallbackFieldSinkContract =
        katana::analysis::StaticCallbackFieldSinkContract;
    const StaticCallbackFieldSinkContract matching_field{
        0x3000u, 0x3008u, 0x3006u, 24, 4u, true, 0x01u};
    const StaticCallbackFieldSinkContract nonmatching_argument{
        0x3000u, 0x3008u, 0x3006u, 24, 4u, true, 0x02u};
    const std::array fields{matching_field, nonmatching_argument};
    const auto inventory =
        katana::analysis::detail::discover_static_returned_receiver_contracts(
            fixture.image, fixture.program, fixture.decoded_lines, fields);

    require(!inventory.truncated && inventory.returned_receivers.size() == 1u,
            "Der vollstaendige ReturnedReceiver-Returnvertrag fehlt.");
    const auto& returned = inventory.returned_receivers.front();
    require(returned.function_address == 0x1000u &&
                returned.origin.call_instruction_address == 0x1000u &&
                returned.origin.callee_address == 0x2000u &&
                returned.optional_null && returned.complete &&
                returned.candidate_only && !returned.strict_eligible &&
                !returned.execution_eligible,
            "Der ReturnedReceiver-Returnvertrag besitzt falsche Herkunft.");

    require(inventory.field_candidates.size() == 1u,
            "Nur der identische Consumer-Receiver darf Candidate sein.");
    const auto& field_candidate = inventory.field_candidates.front();
    require(field_candidate.field == matching_field &&
                field_candidate.origin.call_instruction_address == 0x3000u &&
                field_candidate.origin.callee_address == 0x2000u &&
                !field_candidate.optional_null && !field_candidate.complete &&
                field_candidate.candidate_only &&
                !field_candidate.strict_eligible &&
                !field_candidate.execution_eligible,
            "Der ReturnedReceiver-Feldcandidate besitzt falsche Evidence.");
}

void test_returned_receiver_jsr_abi_and_delay() {
    const auto fixture = make_returned_receiver_jsr_fixture();
    const auto inventory =
        katana::analysis::detail::discover_static_returned_receiver_contracts(
            fixture.image, fixture.program, fixture.decoded_lines, {});
    require(inventory.returned_receivers.size() == 1u,
            "Der JSR-/R14-/Null-Returnvertrag wurde nicht erkannt.");
    const auto& summary = inventory.returned_receivers.front();
    require(summary.function_address == 0x1000u &&
                summary.origin.call_instruction_address == 0x1002u &&
                summary.origin.callee_address == 0x2000u &&
                summary.optional_null && summary.complete &&
                summary.candidate_only && !summary.strict_eligible &&
                !summary.execution_eligible,
            "Die JSR-Herkunft oder der SuperH-Erhalt ist falsch.");

    auto unknown_abi = fixture;
    unknown_abi.image.set_guest_call_abi(katana::io::GuestCallAbi::Unknown);
    require(katana::analysis::detail::discover_static_returned_receiver_contracts(
                unknown_abi.image, unknown_abi.program,
                unknown_abi.decoded_lines, {})
                .returned_receivers.empty(),
            "Unknown-ABI durfte r14 nicht ueber den Folgecall erhalten.");

    const auto delay_fixture = make_returned_receiver_jsr_fixture(true);
    require(katana::analysis::detail::discover_static_returned_receiver_contracts(
                delay_fixture.image, delay_fixture.program,
                delay_fixture.decoded_lines, {})
                .returned_receivers.empty(),
            "Ein Delay-Slot-Clobber von r14 wurde nicht verworfen.");
}

void test_returned_receiver_budget_and_unknown_abi() {
    const auto fixture = make_direct_return_fixture();

    auto known_abi = fixture;
    known_abi.image.set_guest_call_abi(katana::io::GuestCallAbi::SuperHC);
    const auto known_inventory =
        katana::analysis::detail::discover_static_returned_receiver_contracts(
            known_abi.image, known_abi.program, known_abi.decoded_lines, {});
    require(known_inventory.returned_receivers.size() == 1u &&
                known_inventory.returned_receivers.front().origin
                        .call_instruction_address == 0x1100u,
            "Der bekannte SuperH-Returnvertrag fehlt.");

    auto unknown_abi = fixture;
    unknown_abi.image.set_guest_call_abi(katana::io::GuestCallAbi::Unknown);
    const auto unknown_inventory =
        katana::analysis::detail::discover_static_returned_receiver_contracts(
            unknown_abi.image, unknown_abi.program, unknown_abi.decoded_lines, {});
    require(unknown_inventory.returned_receivers.empty() &&
                unknown_inventory.incomplete_functions == 0u,
            "Unknown-ABI durfte BSR-R0-RTS nicht als Receiver anerkennen.");

    const auto budget_inventory =
        katana::analysis::detail::discover_static_returned_receiver_contracts(
            known_abi.image, known_abi.program, known_abi.decoded_lines, {},
            2u);
    require(budget_inventory.truncated &&
                budget_inventory.incomplete_functions == 1u &&
                budget_inventory.work_items == 2u &&
                budget_inventory.instructions_examined >
                    budget_inventory.work_items &&
                budget_inventory.returned_receivers.empty(),
            "Das globale Workbudget belastet nicht die echten CFG-Besuche.");
}

void test_returned_receiver_tailcall_field_observation() {
    const auto fixture = make_returned_receiver_tailcall_fixture();
    using StaticCallbackFieldSinkContract =
        katana::analysis::StaticCallbackFieldSinkContract;
    const StaticCallbackFieldSinkContract tail_field{
        0x5000u, 0x5006u, 0x5004u, 24, 4u, false, 0x01u};

    const auto inventory =
        katana::analysis::detail::discover_static_returned_receiver_contracts(
            fixture.image, fixture.program, fixture.decoded_lines,
            std::array{tail_field});
    require(inventory.field_candidates.size() == 1u,
            "Der JumpRegister-Tailcall wurde nicht als Feldcandidate beobachtet.");
    const auto& candidate = inventory.field_candidates.front();
    require(candidate.field == tail_field &&
                candidate.origin.call_instruction_address == 0x5000u &&
                candidate.origin.callee_address == 0x6000u &&
                !candidate.complete && candidate.candidate_only &&
                !candidate.strict_eligible && !candidate.execution_eligible,
            "Der Tailcall-Receivercandidate besitzt falsche Herkunft.");
}
} // namespace

int main() {
    try {
        test_positive_and_fpu_preservation();
        test_fail_closed_variants();
        test_literal_jsr_resolution_and_delay_order();
        test_returned_receiver_domain();
        test_returned_receiver_jsr_abi_and_delay();
        test_returned_receiver_budget_and_unknown_abi();
        test_returned_receiver_tailcall_field_observation();
        std::cout << "Return/Object-Callback-Diagnose erfolgreich.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << error.what() << '\n';
        return 1;
    }
}

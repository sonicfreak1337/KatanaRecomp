#include "katana/sh4/decoder.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using katana::sh4::ControlFlowKind;
using katana::sh4::InstructionKind;
using katana::sh4::SpecialRegister;

struct SpecificationVector {
    std::uint16_t opcode;
    InstructionKind kind;
    std::uint8_t destination;
    std::uint8_t source;
    std::uint8_t branch;
    std::int32_t immediate;
    std::int32_t displacement;
    SpecialRegister special_register;
    ControlFlowKind control_flow;
    bool delay_slot;
    bool privileged;
};

constexpr std::array kSpecificationVectors = {
    SpecificationVector{0x0009u, InstructionKind::Nop, 0, 0, 0, 0, 0, SpecialRegister::None, ControlFlowKind::None, false, false},
    SpecificationVector{0x000Bu, InstructionKind::Rts, 0, 0, 0, 0, 0, SpecialRegister::None, ControlFlowKind::Return, true, false},
    SpecificationVector{0x002Bu, InstructionKind::ReturnFromException, 0, 0, 0, 0, 0, SpecialRegister::None, ControlFlowKind::ExceptionReturn, true, true},
    SpecificationVector{0x001Bu, InstructionKind::Sleep, 0, 0, 0, 0, 0, SpecialRegister::None, ControlFlowKind::Halt, false, true},
    SpecificationVector{0xC3FFu, InstructionKind::TrapAlways, 0, 0, 0, 255, 0, SpecialRegister::None, ControlFlowKind::Trap, false, false},
    SpecificationVector{0xE080u, InstructionKind::MovImmediate, 0, 0, 0, -128, 0, SpecialRegister::None, ControlFlowKind::None, false, false},
    SpecificationVector{0xEFFFu, InstructionKind::MovImmediate, 15, 0, 0, -1, 0, SpecialRegister::None, ControlFlowKind::None, false, false},
    SpecificationVector{0x707Fu, InstructionKind::AddImmediate, 0, 0, 0, 127, 0, SpecialRegister::None, ControlFlowKind::None, false, false},
    SpecificationVector{0x3FE8u, InstructionKind::SubRegister, 15, 14, 0, 0, 0, SpecialRegister::None, ControlFlowKind::None, false, false},
    SpecificationVector{0x6F0Bu, InstructionKind::NegateRegister, 15, 0, 0, 0, 0, SpecialRegister::None, ControlFlowKind::None, false, false},
    SpecificationVector{0x4F10u, InstructionKind::DecrementAndTest, 15, 0, 0, 0, 0, SpecialRegister::None, ControlFlowKind::None, false, false},
    SpecificationVector{0xA7FFu, InstructionKind::Bra, 0, 0, 0, 0, 4094, SpecialRegister::None, ControlFlowKind::UnconditionalBranch, true, false},
    SpecificationVector{0xA800u, InstructionKind::Bra, 0, 0, 0, 0, -4096, SpecialRegister::None, ControlFlowKind::UnconditionalBranch, true, false},
    SpecificationVector{0x8980u, InstructionKind::Bt, 0, 0, 0, 0, -256, SpecialRegister::None, ControlFlowKind::ConditionalBranch, false, false},
    SpecificationVector{0x4F2Bu, InstructionKind::Jmp, 0, 0, 15, 0, 0, SpecialRegister::None, ControlFlowKind::IndirectBranch, true, false},
    SpecificationVector{0x81FFu, InstructionKind::MovWordStoreDisplacement, 15, 0, 0, 0, 30, SpecialRegister::None, ControlFlowKind::None, false, false},
    SpecificationVector{0x5FEFu, InstructionKind::MovLongLoadDisplacement, 15, 14, 0, 0, 60, SpecialRegister::None, ControlFlowKind::None, false, false},
    SpecificationVector{0x0FE4u, InstructionKind::MovByteStoreR0Indexed, 15, 14, 0, 0, 0, SpecialRegister::None, ControlFlowKind::None, false, false},
    SpecificationVector{0xC6FFu, InstructionKind::MovLongLoadGbrDisplacement, 0, 0, 0, 0, 1020, SpecialRegister::None, ControlFlowKind::None, false, false},
    SpecificationVector{0x9FFFu, InstructionKind::MovWordLoadPcRelative, 15, 0, 0, 0, 510, SpecialRegister::None, ControlFlowKind::None, false, false},
    SpecificationVector{0xC7FFu, InstructionKind::MoveAddressPcRelative, 0, 0, 0, 0, 1020, SpecialRegister::None, ControlFlowKind::None, false, false},
    SpecificationVector{0x0AF2u, InstructionKind::StoreSpecialRegister, 10, 0, 0, 0, 0, SpecialRegister::Bank7, ControlFlowKind::None, false, true},
    SpecificationVector{0x4E0Eu, InstructionKind::LoadSpecialRegister, 0, 14, 0, 0, 0, SpecialRegister::Sr, ControlFlowKind::None, false, true},
    SpecificationVector{0x4D1Eu, InstructionKind::LoadSpecialRegister, 0, 13, 0, 0, 0, SpecialRegister::Gbr, ControlFlowKind::None, false, false},
    SpecificationVector{0x40F6u, InstructionKind::LoadSpecialRegisterPostIncrement, 0, 0, 0, 0, 0, SpecialRegister::Dbr, ControlFlowKind::None, false, true},
    SpecificationVector{0xFFFFu, InstructionKind::Unknown, 0, 0, 0, 0, 0, SpecialRegister::None, ControlFlowKind::None, false, false},
    SpecificationVector{0xF000u, InstructionKind::Unknown, 0, 0, 0, 0, 0, SpecialRegister::None, ControlFlowKind::None, false, false},
    SpecificationVector{0x407Au, InstructionKind::Unknown, 0, 0, 0, 0, 0, SpecialRegister::None, ControlFlowKind::None, false, false}
};

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}

int main() {
    for (const auto& expected : kSpecificationVectors) {
        const auto actual = katana::sh4::decode(expected.opcode);
        const auto prefix = "Spezifikationsvektor 0x" + std::to_string(expected.opcode) + ": ";
        require(actual.kind == expected.kind, prefix + "falsche Instruktionsart.");
        require(actual.destination_register == expected.destination, prefix + "falsches Zielregister.");
        require(actual.source_register == expected.source, prefix + "falsches Quellregister.");
        require(actual.branch_register == expected.branch, prefix + "falsches Sprungregister.");
        require(actual.immediate == expected.immediate, prefix + "falsches Immediate.");
        require(actual.displacement == expected.displacement, prefix + "falsches Displacement.");
        require(actual.special_register == expected.special_register, prefix + "falsches Spezialregister.");
        require(actual.control_flow == expected.control_flow, prefix + "falscher Kontrollfluss.");
        require(actual.has_delay_slot == expected.delay_slot, prefix + "falscher Delay-Slot-Status.");
        require(actual.is_privileged == expected.privileged, prefix + "falscher Privilegstatus.");
    }

    std::cout << "KR-1504 unabhaengige SH-4-Spezifikationsvektoren erfolgreich.\n";
    return EXIT_SUCCESS;
}

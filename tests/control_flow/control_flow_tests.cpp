#include "katana/sh4/decoder.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(
    const bool condition,
    const std::string& message
) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}

int main() {
    using katana::sh4::ControlFlowKind;
    using katana::sh4::InstructionKind;
    using katana::sh4::calculate_direct_branch_target;
    using katana::sh4::decode;

    const auto bra_forward = decode(0xA001u);
    require(
        bra_forward.kind == InstructionKind::Bra,
        "BRA wurde nicht erkannt."
    );
    require(
        bra_forward.displacement == 2,
        "BRA besitzt das falsche Displacement."
    );
    require(
        bra_forward.has_delay_slot,
        "BRA wurde nicht als verzÃ¶gerter Sprung markiert."
    );
    require(
        calculate_direct_branch_target(
            bra_forward,
            0x8C010000u
        ) == 0x8C010006u,
        "Das positive BRA-Sprungziel ist falsch."
    );

    const auto bra_backward = decode(0xAFFEu);
    require(
        bra_backward.displacement == -4,
        "Negatives BRA-Displacement wurde falsch erweitert."
    );
    require(
        calculate_direct_branch_target(
            bra_backward,
            0x8C010000u
        ) == 0x8C010000u,
        "Das negative BRA-Sprungziel ist falsch."
    );

    const auto bsr = decode(0xB002u);
    require(
        bsr.kind == InstructionKind::Bsr,
        "BSR wurde nicht erkannt."
    );
    require(
        bsr.control_flow == ControlFlowKind::Call,
        "BSR wurde nicht als Aufruf markiert."
    );
    require(
        calculate_direct_branch_target(
            bsr,
            0x8C010000u
        ) == 0x8C010008u,
        "Das BSR-Sprungziel ist falsch."
    );

    const auto bt = decode(0x8901u);
    require(
        bt.kind == InstructionKind::Bt,
        "BT wurde nicht erkannt."
    );
    require(
        !bt.has_delay_slot,
        "Normales BT darf keinen Delay Slot besitzen."
    );
    require(
        calculate_direct_branch_target(
            bt,
            0x8C010000u
        ) == 0x8C010006u,
        "Das BT-Sprungziel ist falsch."
    );

    const auto bf = decode(0x8BFFu);
    require(
        bf.kind == InstructionKind::Bf,
        "BF wurde nicht erkannt."
    );
    require(
        calculate_direct_branch_target(
            bf,
            0x8C010000u
        ) == 0x8C010002u,
        "Das negative BF-Sprungziel ist falsch."
    );

    const auto bt_s = decode(0x8D01u);
    require(
        bt_s.kind == InstructionKind::BtS,
        "BT/S wurde nicht erkannt."
    );
    require(
        bt_s.has_delay_slot,
        "BT/S wurde nicht als verzÃ¶gerter Sprung markiert."
    );

    const auto bf_s = decode(0x8FFFu);
    require(
        bf_s.kind == InstructionKind::BfS,
        "BF/S wurde nicht erkannt."
    );
    require(
        bf_s.has_delay_slot,
        "BF/S wurde nicht als verzÃ¶gerter Sprung markiert."
    );

    const auto jmp = decode(0x432Bu);
    require(
        jmp.kind == InstructionKind::Jmp,
        "JMP wurde nicht erkannt."
    );
    require(
        jmp.branch_register == 3,
        "JMP verwendet das falsche Register."
    );
    require(
        jmp.control_flow == ControlFlowKind::IndirectBranch,
        "JMP wurde nicht als indirekter Sprung markiert."
    );
    require(
        jmp.has_delay_slot,
        "JMP wurde nicht als verzÃ¶gerter Sprung markiert."
    );

    const auto jsr = decode(0x440Bu);
    require(
        jsr.kind == InstructionKind::Jsr,
        "JSR wurde nicht erkannt."
    );
    require(
        jsr.branch_register == 4,
        "JSR verwendet das falsche Register."
    );
    require(
        jsr.control_flow == ControlFlowKind::IndirectCall,
        "JSR wurde nicht als indirekter Aufruf markiert."
    );

    const auto rts = decode(0x000Bu);
    require(
        rts.control_flow == ControlFlowKind::Return,
        "RTS wurde nicht als RÃ¼cksprung markiert."
    );
    require(
        rts.has_delay_slot,
        "RTS wurde nicht als verzÃ¶gerter RÃ¼cksprung markiert."
    );

    constexpr std::array<std::uint8_t, 8> bytes = {
        0x00, 0xA0,
        0x09, 0x00,
        0x00, 0x89,
        0x09, 0x00
    };

    const auto lines = katana::sh4::disassemble(
        bytes,
        0x8C010000u
    );

    require(
        lines.size() == 4,
        "Kontrollfluss-Test besitzt falsche Zeilenanzahl."
    );
    require(
        !lines[0].is_delay_slot,
        "BRA selbst wurde fÃ¤lschlich als Delay Slot markiert."
    );
    require(
        lines[1].is_delay_slot,
        "Instruktion nach BRA wurde nicht als Delay Slot markiert."
    );
    require(
        !lines[3].is_delay_slot,
        "Instruktion nach normalem BT darf kein Delay Slot sein."
    );
    require(
        lines[0].target_address == 0x8C010004u,
        "BRA-Ziel in der Disassembly ist falsch."
    );
    require(
        lines[2].target_address == 0x8C010008u,
        "BT-Ziel in der Disassembly ist falsch."
    );

    std::cout
        << "Alle Kontrollfluss- und Delay-Slot-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}

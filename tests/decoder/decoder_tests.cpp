#include "katana/sh4/decoder.hpp"

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
    using katana::sh4::decode;
    using katana::sh4::InstructionKind;

    const auto nop = decode(0x0009u);
    require(nop.kind == InstructionKind::Nop, "NOP wurde nicht erkannt.");
    require(nop.text == "nop", "NOP besitzt falsche Ausgabe.");

    const auto rts = decode(0x000Bu);
    require(rts.kind == InstructionKind::Rts, "RTS wurde nicht erkannt.");
    require(rts.text == "rts", "RTS besitzt falsche Ausgabe.");

    const auto mov_immediate = decode(0xE1FFu);
    require(mov_immediate.kind == InstructionKind::MovImmediate,
            "MOV mit Immediate wurde nicht erkannt.");
    require(mov_immediate.destination_register == 1, "MOV verwendet das falsche Zielregister.");
    require(mov_immediate.immediate == -1, "MOV signiert den Immediate-Wert falsch.");
    require(mov_immediate.text == "mov #-1, r1", "MOV erzeugt falsche Disassembly.");

    const auto add_immediate = decode(0x72FEu);
    require(add_immediate.kind == InstructionKind::AddImmediate,
            "ADD mit Immediate wurde nicht erkannt.");
    require(add_immediate.destination_register == 2, "ADD verwendet das falsche Zielregister.");
    require(add_immediate.immediate == -2, "ADD signiert den Immediate-Wert falsch.");
    require(add_immediate.text == "add #-2, r2", "ADD Immediate erzeugt falsche Disassembly.");

    const auto mov_register = decode(0x6123u);
    require(mov_register.kind == InstructionKind::MovRegister,
            "MOV zwischen Registern wurde nicht erkannt.");
    require(mov_register.destination_register == 1, "MOV verwendet das falsche Zielregister.");
    require(mov_register.source_register == 2, "MOV verwendet das falsche Quellregister.");
    require(mov_register.text == "mov r2, r1", "MOV Register erzeugt falsche Disassembly.");

    const auto add_register = decode(0x312Cu);
    require(add_register.kind == InstructionKind::AddRegister,
            "ADD zwischen Registern wurde nicht erkannt.");
    require(add_register.destination_register == 1, "ADD verwendet das falsche Zielregister.");
    require(add_register.source_register == 2, "ADD verwendet das falsche Quellregister.");
    require(add_register.text == "add r2, r1", "ADD Register erzeugt falsche Disassembly.");

    const auto unknown = decode(0xFFFFu);
    require(unknown.kind == InstructionKind::Unknown,
            "Unbekannter Opcode wurde fälschlich erkannt.");
    require(!unknown.is_known(), "Unbekannter Opcode meldet sich als bekannt.");
    require(unknown.text == ".word 0xFFFF", "Unbekannter Opcode besitzt falsche Ausgabe.");

    std::cout << "Alle SH-4-Decoder-Tests erfolgreich.\n";
    return EXIT_SUCCESS;
}

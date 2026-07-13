#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
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
    using katana::ir::Operation;
    using katana::sh4::InstructionKind;

    const auto extub = katana::sh4::decode(0x621Cu);
    const auto extuw = katana::sh4::decode(0x631Du);
    const auto extsb = katana::sh4::decode(0x641Eu);
    const auto extsw = katana::sh4::decode(0x651Fu);
    const auto swapb = katana::sh4::decode(0x6618u);
    const auto swapw = katana::sh4::decode(0x6719u);
    const auto xtrct = katana::sh4::decode(0x298Du);

    require(
        extub.kind == InstructionKind::ExtendUnsignedByte &&
        extub.text == "extu.b r1, r2",
        "EXTU.B wurde falsch dekodiert."
    );

    require(
        extuw.kind == InstructionKind::ExtendUnsignedWord &&
        extuw.text == "extu.w r1, r3",
        "EXTU.W wurde falsch dekodiert."
    );

    require(
        extsb.kind == InstructionKind::ExtendSignedByte &&
        extsb.text == "exts.b r1, r4",
        "EXTS.B wurde falsch dekodiert."
    );

    require(
        extsw.kind == InstructionKind::ExtendSignedWord &&
        extsw.text == "exts.w r1, r5",
        "EXTS.W wurde falsch dekodiert."
    );

    require(
        swapb.kind == InstructionKind::SwapBytes &&
        swapb.text == "swap.b r1, r6",
        "SWAP.B wurde falsch dekodiert."
    );

    require(
        swapw.kind == InstructionKind::SwapWords &&
        swapw.text == "swap.w r1, r7",
        "SWAP.W wurde falsch dekodiert."
    );

    require(
        xtrct.kind == InstructionKind::ExtractMiddle &&
        xtrct.destination_register == 9u &&
        xtrct.source_register == 8u &&
        xtrct.text == "xtrct r8, r9",
        "XTRCT wurde falsch dekodiert."
    );

    constexpr std::array<std::uint8_t, 18> bytes = {
        0x1C, 0x62,
        0x1D, 0x63,
        0x1E, 0x64,
        0x1F, 0x65,
        0x18, 0x66,
        0x19, 0x67,
        0x8D, 0x29,
        0x0B, 0x00,
        0x09, 0x00
    };

    const auto lines = katana::sh4::disassemble(
        bytes,
        0x8C010000u
    );

    constexpr std::array<std::uint32_t, 1> seeds = {
        0x8C010000u
    };

    const auto functions =
        katana::analysis::discover_functions(
            lines,
            seeds
        );

    const auto program =
        katana::ir::lower_program(
            lines,
            functions
        );

    require(
        program.size() == 1u,
        "Der KR-1105-Test muss genau eine Funktion besitzen."
    );

    const auto& instructions =
        program[0].blocks[0].instructions;

    require(
        instructions.size() == 9u,
        "Der KR-1105-IR-Block besitzt eine falsche Groesse."
    );

    require(
        instructions[0].operation == Operation::ExtendUnsignedByte,
        "EXTU.B wurde falsch in IR abgesenkt."
    );
    require(
        instructions[1].operation == Operation::ExtendUnsignedWord,
        "EXTU.W wurde falsch in IR abgesenkt."
    );
    require(
        instructions[2].operation == Operation::ExtendSignedByte,
        "EXTS.B wurde falsch in IR abgesenkt."
    );
    require(
        instructions[3].operation == Operation::ExtendSignedWord,
        "EXTS.W wurde falsch in IR abgesenkt."
    );
    require(
        instructions[4].operation == Operation::SwapBytes,
        "SWAP.B wurde falsch in IR abgesenkt."
    );
    require(
        instructions[5].operation == Operation::SwapWords,
        "SWAP.W wurde falsch in IR abgesenkt."
    );
    require(
        instructions[6].operation == Operation::ExtractMiddle,
        "XTRCT wurde falsch in IR abgesenkt."
    );

    const auto source =
        katana::codegen::emit_cpp_program(
            program,
            0x8C010000u
        );

    require(
        source.find("& 0x000000FFu;") != std::string::npos,
        "EXTU.B fehlt im generierten C++."
    );

    require(
        source.find("& 0x0000FFFFu;") != std::string::npos,
        "EXTU.W fehlt im generierten C++."
    );

    require(
        source.find("value |= 0xFFFFFF00u;") !=
            std::string::npos,
        "EXTS.B fehlt im generierten C++."
    );

    require(
        source.find("value |= 0xFFFF0000u;") !=
            std::string::npos,
        "EXTS.W fehlt im generierten C++."
    );

    require(
        source.find("(value & 0x000000FFu) << 8u") !=
            std::string::npos,
        "SWAP.B fehlt im generierten C++."
    );

    require(
        source.find("(value << 16u) | (value >> 16u)") !=
            std::string::npos,
        "SWAP.W fehlt im generierten C++."
    );

    require(
        source.find("(source << 16u)") != std::string::npos &&
        source.find("(destination >> 16u)") != std::string::npos,
        "XTRCT fehlt im generierten C++."
    );

    std::cout
        << "Alle KR-1105 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}

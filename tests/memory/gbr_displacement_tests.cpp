#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t base_address = 0x8C010000u;

constexpr auto fixture_bytes = [] {
    std::array<std::uint8_t, 86> bytes{};

    for (std::size_t offset = 0; offset < bytes.size(); offset += 2u) {
        bytes[offset] = 0x09u;
        bytes[offset + 1u] = 0x00u;
    }

    const auto write_opcode = [&bytes](
        const std::size_t offset,
        const std::uint16_t opcode
    ) {
        bytes[offset] = static_cast<std::uint8_t>(opcode & 0x00FFu);
        bytes[offset + 1u] = static_cast<std::uint8_t>(opcode >> 8u);
    };

    write_opcode(0x00u, 0xB016u);
    write_opcode(0x04u, 0xB01Cu);
    write_opcode(0x08u, 0xB01Eu);
    write_opcode(0x0Cu, 0xB020u);
    write_opcode(0x10u, 0x000Bu);

    write_opcode(0x30u, 0xC0FFu);
    write_opcode(0x32u, 0xC1FFu);
    write_opcode(0x34u, 0xC2FFu);
    write_opcode(0x36u, 0x000Bu);

    write_opcode(0x40u, 0xC4FFu);
    write_opcode(0x42u, 0x000Bu);
    write_opcode(0x48u, 0xC5FFu);
    write_opcode(0x4Au, 0x000Bu);
    write_opcode(0x50u, 0xC6FFu);
    write_opcode(0x52u, 0x000Bu);

    return bytes;
}();

constexpr std::array<std::uint32_t, 1> function_seeds = {
    base_address
};

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::vector<katana::ir::Function> build_program() {
    const auto lines = katana::sh4::disassemble(
        fixture_bytes,
        base_address
    );
    const auto functions = katana::analysis::discover_functions(
        lines,
        function_seeds
    );
    return katana::ir::lower_program(lines, functions);
}

std::size_t count_operation(
    const std::span<const katana::ir::Function> program,
    const katana::ir::Operation operation
) {
    std::size_t count = 0;
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.operation == operation) {
                    ++count;
                }
            }
        }
    }
    return count;
}

int emit_fixture(const std::string& output_path) {
    const auto source = katana::codegen::emit_cpp_program(
        build_program(),
        base_address
    );
    std::ofstream output(
        output_path,
        std::ios::binary | std::ios::trunc
    );
    if (!output) {
        return EXIT_FAILURE;
    }
    output.write(
        source.data(),
        static_cast<std::streamsize>(source.size())
    );
    return output ? EXIT_SUCCESS : EXIT_FAILURE;
}

}

int main(const int argc, char* argv[]) {
    using katana::ir::Operation;
    using katana::sh4::InstructionKind;

    if (argc == 3 && std::string(argv[1]) == "--emit-cpp") {
        return emit_fixture(argv[2]);
    }
    require(argc == 1, "Unerwartete Argumente fuer den KR-1404-Test.");

    const auto store_b = katana::sh4::decode(0xC0FFu);
    const auto store_w = katana::sh4::decode(0xC1FFu);
    const auto store_l = katana::sh4::decode(0xC2FFu);
    const auto load_b = katana::sh4::decode(0xC4FFu);
    const auto load_w = katana::sh4::decode(0xC5FFu);
    const auto load_l = katana::sh4::decode(0xC6FFu);

    require(
        store_b.kind == InstructionKind::MovByteStoreGbrDisplacement &&
        store_b.source_register == 0u &&
        store_b.displacement == 255 &&
        store_b.text == "mov.b r0, @(255, gbr)",
        "MOV.B GBR-Store wurde falsch dekodiert."
    );
    require(
        store_w.kind == InstructionKind::MovWordStoreGbrDisplacement &&
        store_w.displacement == 510 &&
        store_w.text == "mov.w r0, @(510, gbr)",
        "MOV.W GBR-Store wurde falsch skaliert."
    );
    require(
        store_l.kind == InstructionKind::MovLongStoreGbrDisplacement &&
        store_l.displacement == 1020 &&
        store_l.text == "mov.l r0, @(1020, gbr)",
        "MOV.L GBR-Store wurde falsch skaliert."
    );
    require(
        load_b.kind == InstructionKind::MovByteLoadGbrDisplacement &&
        load_b.destination_register == 0u &&
        load_b.displacement == 255 &&
        load_b.text == "mov.b @(255, gbr), r0",
        "MOV.B GBR-Load wurde falsch dekodiert."
    );
    require(
        load_w.kind == InstructionKind::MovWordLoadGbrDisplacement &&
        load_w.displacement == 510,
        "MOV.W GBR-Load wurde falsch skaliert."
    );
    require(
        load_l.kind == InstructionKind::MovLongLoadGbrDisplacement &&
        load_l.displacement == 1020,
        "MOV.L GBR-Load wurde falsch skaliert."
    );
    require(
        katana::sh4::decode(0xC000u).displacement == 0,
        "Ein GBR-Displacement von null wurde falsch dekodiert."
    );

    const auto program = build_program();
    require(
        program.size() == 5u,
        "Der KR-1404-Test muss Einstiegspunkt und vier Funktionen erzeugen."
    );
    for (const auto operation : {
        Operation::StoreByteGbrDisplacement,
        Operation::StoreWordGbrDisplacement,
        Operation::StoreLongGbrDisplacement,
        Operation::LoadByteSignedGbrDisplacement,
        Operation::LoadWordSignedGbrDisplacement,
        Operation::LoadLongGbrDisplacement
    }) {
        require(
            count_operation(program, operation) == 1u,
            "Eine GBR-relative Operation fehlt in der IR."
        );
    }

    const auto source = katana::codegen::emit_cpp_program(
        program,
        base_address
    );
    require(
        source.find("std::uint32_t gbr = 0;") != std::string::npos,
        "Der generierte CPU-Zustand besitzt kein GBR."
    );
    require(
        source.find("cpu.gbr + 255u") != std::string::npos &&
        source.find("cpu.gbr + 510u") != std::string::npos &&
        source.find("cpu.gbr + 1020u") != std::string::npos,
        "Der Codegenerator verwendet falsche GBR-Adressen."
    );

    std::cout
        << "Alle KR-1404 Decoder-, IR- und Codegen-Tests erfolgreich.\n";
    return EXIT_SUCCESS;
}

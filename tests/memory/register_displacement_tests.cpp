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

constexpr std::uint32_t entry_address = 0x8C010000u;

constexpr std::array<std::uint8_t, 16> fixture_bytes = {
    0x1F,
    0x80, // mov.b r0, @(15, r1)
    0x2F,
    0x81, // mov.w r0, @(30, r2)
    0x3F,
    0x14, // mov.l r3, @(60, r4)
    0x5F,
    0x84, // mov.b @(15, r5), r0
    0x6F,
    0x85, // mov.w @(30, r6), r0
    0x7F,
    0x58, // mov.l @(60, r7), r8
    0x0B,
    0x00, // rts
    0x09,
    0x00 // nop (delay slot)
};

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::vector<katana::ir::Function> build_program() {
    const auto lines = katana::sh4::disassemble(fixture_bytes, entry_address);

    constexpr std::array<std::uint32_t, 1> seeds = {entry_address};

    const auto functions = katana::analysis::discover_functions(lines, seeds);

    return katana::ir::lower_program(lines, functions);
}

const katana::ir::Instruction* find_operation(const std::span<const katana::ir::Function> program,
                                              const katana::ir::Operation operation) {
    for (const auto& function : program) {
        for (const auto& block : function.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.operation == operation) {
                    return &instruction;
                }
            }
        }
    }

    return nullptr;
}

int emit_fixture(const std::string& output_path) {
    const auto program = build_program();
    const auto source = katana::codegen::emit_cpp_program(program, entry_address);

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);

    if (!output) {
        std::cerr << "Ausgabedatei konnte nicht geoeffnet werden.\n";
        return EXIT_FAILURE;
    }

    output.write(source.data(), static_cast<std::streamsize>(source.size()));

    return output ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(const int argc, char* argv[]) {
    using katana::ir::Operation;
    using katana::sh4::InstructionKind;

    if (argc == 3 && std::string(argv[1]) == "--emit-cpp") {
        return emit_fixture(argv[2]);
    }

    require(argc == 1, "Unerwartete Argumente fuer den KR-1402-Test.");

    const auto store_b = katana::sh4::decode(0x801Fu);
    const auto store_w = katana::sh4::decode(0x812Fu);
    const auto store_l = katana::sh4::decode(0x143Fu);
    const auto load_b = katana::sh4::decode(0x845Fu);
    const auto load_w = katana::sh4::decode(0x856Fu);
    const auto load_l = katana::sh4::decode(0x587Fu);

    require(store_b.kind == InstructionKind::MovByteStoreDisplacement &&
                store_b.source_register == 0u && store_b.destination_register == 1u &&
                store_b.displacement == 15 && store_b.text == "mov.b r0, @(15, r1)",
            "MOV.B Displacement-Store wurde falsch dekodiert.");

    require(store_w.kind == InstructionKind::MovWordStoreDisplacement &&
                store_w.source_register == 0u && store_w.destination_register == 2u &&
                store_w.displacement == 30 && store_w.text == "mov.w r0, @(30, r2)",
            "MOV.W Displacement-Store wurde falsch skaliert.");

    require(store_l.kind == InstructionKind::MovLongStoreDisplacement &&
                store_l.source_register == 3u && store_l.destination_register == 4u &&
                store_l.displacement == 60 && store_l.text == "mov.l r3, @(60, r4)",
            "MOV.L Displacement-Store wurde falsch dekodiert.");

    require(load_b.kind == InstructionKind::MovByteLoadDisplacement &&
                load_b.source_register == 5u && load_b.destination_register == 0u &&
                load_b.displacement == 15 && load_b.text == "mov.b @(15, r5), r0",
            "MOV.B Displacement-Load wurde falsch dekodiert.");

    require(load_w.kind == InstructionKind::MovWordLoadDisplacement &&
                load_w.source_register == 6u && load_w.destination_register == 0u &&
                load_w.displacement == 30 && load_w.text == "mov.w @(30, r6), r0",
            "MOV.W Displacement-Load wurde falsch skaliert.");

    require(load_l.kind == InstructionKind::MovLongLoadDisplacement &&
                load_l.source_register == 7u && load_l.destination_register == 8u &&
                load_l.displacement == 60 && load_l.text == "mov.l @(60, r7), r8",
            "MOV.L Displacement-Load wurde falsch dekodiert.");

    const auto zero_displacement = katana::sh4::decode(0x8030u);
    require(zero_displacement.displacement == 0,
            "Ein Displacement von null wurde falsch dekodiert.");

    const auto program = build_program();
    require(program.size() == 1u, "Der KR-1402-Test muss genau eine Funktion erzeugen.");

    for (const auto operation : {Operation::StoreByteDisplacement,
                                 Operation::StoreWordDisplacement,
                                 Operation::StoreLongDisplacement,
                                 Operation::LoadByteSignedDisplacement,
                                 Operation::LoadWordSignedDisplacement,
                                 Operation::LoadLongDisplacement}) {
        const auto* instruction = find_operation(program, operation);
        require(instruction != nullptr, "Eine Register-Displacement-Operation fehlt in der IR.");
    }

    require(find_operation(program, Operation::StoreLongDisplacement)->displacement == 60,
            "Das skalierte Long-Displacement wurde nicht in die IR uebernommen.");

    const auto source = katana::codegen::emit_cpp_program(program, entry_address);

    require(source.find("cpu.r[1] + 15u") != std::string::npos &&
                source.find("cpu.r[2] + 30u") != std::string::npos &&
                source.find("cpu.r[4] + 60u") != std::string::npos,
            "Der Codegenerator verwendet falsche Store-Adressen.");

    require(source.find("guest_read_s8(cpu, cpu.r[5] + 15u)") != std::string::npos &&
                source.find("guest_read_s16(cpu, cpu.r[6] + 30u)") != std::string::npos &&
                source.find("guest_read_u32(cpu, cpu.r[7] + 60u)") != std::string::npos,
            "Der Codegenerator verwendet falsche Load-Adressen oder Breiten.");

    std::cout << "Alle KR-1402 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}

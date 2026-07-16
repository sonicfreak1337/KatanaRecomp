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

constexpr std::array<std::uint8_t, 20> fixture_bytes = {
    0x14, 0x02, // mov.b r1, @(r0, r2)
    0x35, 0x04, // mov.w r3, @(r0, r4)
    0x56, 0x06, // mov.l r5, @(r0, r6)
    0x7C, 0x08, // mov.b @(r0, r7), r8
    0x9D, 0x0A, // mov.w @(r0, r9), r10
    0xBE, 0x0C, // mov.l @(r0, r11), r12
    0x04, 0x0D, // mov.b r0, @(r0, r13)
    0xEC, 0x00, // mov.b @(r0, r14), r0
    0x0B, 0x00, // rts
    0x09, 0x00  // nop (delay slot)
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

std::size_t count_operation(const std::span<const katana::ir::Function> program,
                            const katana::ir::Operation operation) {
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
    const auto source = katana::codegen::emit_cpp_program(build_program(), entry_address);

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);

    if (!output) {
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

    require(argc == 1, "Unerwartete Argumente fuer den KR-1403-Test.");

    const auto store_b = katana::sh4::decode(0x0214u);
    const auto store_w = katana::sh4::decode(0x0435u);
    const auto store_l = katana::sh4::decode(0x0656u);
    const auto load_b = katana::sh4::decode(0x087Cu);
    const auto load_w = katana::sh4::decode(0x0A9Du);
    const auto load_l = katana::sh4::decode(0x0CBEu);
    const auto overlap_store = katana::sh4::decode(0x0D04u);
    const auto overlap_load = katana::sh4::decode(0x00ECu);

    require(store_b.kind == InstructionKind::MovByteStoreR0Indexed &&
                store_b.source_register == 1u && store_b.destination_register == 2u &&
                store_b.text == "mov.b r1, @(r0, r2)",
            "MOV.B R0-indexierter Store wurde falsch dekodiert.");
    require(store_w.kind == InstructionKind::MovWordStoreR0Indexed &&
                store_w.source_register == 3u && store_w.destination_register == 4u,
            "MOV.W R0-indexierter Store wurde falsch dekodiert.");
    require(store_l.kind == InstructionKind::MovLongStoreR0Indexed &&
                store_l.source_register == 5u && store_l.destination_register == 6u,
            "MOV.L R0-indexierter Store wurde falsch dekodiert.");
    require(load_b.kind == InstructionKind::MovByteLoadR0Indexed && load_b.source_register == 7u &&
                load_b.destination_register == 8u && load_b.text == "mov.b @(r0, r7), r8",
            "MOV.B R0-indexierter Load wurde falsch dekodiert.");
    require(load_w.kind == InstructionKind::MovWordLoadR0Indexed && load_w.source_register == 9u &&
                load_w.destination_register == 10u,
            "MOV.W R0-indexierter Load wurde falsch dekodiert.");
    require(load_l.kind == InstructionKind::MovLongLoadR0Indexed && load_l.source_register == 11u &&
                load_l.destination_register == 12u,
            "MOV.L R0-indexierter Load wurde falsch dekodiert.");
    require(overlap_store.source_register == 0u && overlap_store.destination_register == 13u &&
                overlap_load.source_register == 14u && overlap_load.destination_register == 0u,
            "R0-Ueberlappungsfaelle wurden falsch dekodiert.");

    const auto program = build_program();
    require(program.size() == 1u, "Der KR-1403-Test muss genau eine Funktion erzeugen.");
    require(count_operation(program, Operation::StoreByteR0Indexed) == 2u &&
                count_operation(program, Operation::StoreWordR0Indexed) == 1u &&
                count_operation(program, Operation::StoreLongR0Indexed) == 1u,
            "R0-indexierte Stores wurden falsch in die IR abgesenkt.");
    require(count_operation(program, Operation::LoadByteSignedR0Indexed) == 2u &&
                count_operation(program, Operation::LoadWordSignedR0Indexed) == 1u &&
                count_operation(program, Operation::LoadLongR0Indexed) == 1u,
            "R0-indexierte Loads wurden falsch in die IR abgesenkt.");

    const auto source = katana::codegen::emit_cpp_program(program, entry_address);
    require(source.find("write_u8(cpu.r[0] + cpu.r[2]") != std::string::npos &&
                source.find("write_u16(cpu.r[0] + cpu.r[4]") != std::string::npos &&
                source.find("write_u32(cpu.r[0] + cpu.r[6]") != std::string::npos,
            "Der Codegenerator verwendet falsche R0-indexierte Store-Adressen.");
    require(source.find("read_s8(cpu.r[0] + cpu.r[7]") != std::string::npos &&
                source.find("read_s16(cpu.r[0] + cpu.r[9]") != std::string::npos &&
                source.find("read_u32(cpu.r[0] + cpu.r[11]") != std::string::npos,
            "Der Codegenerator verwendet falsche R0-indexierte Load-Adressen.");

    std::cout << "Alle KR-1403 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}

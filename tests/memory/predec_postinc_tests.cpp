#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
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

std::size_t count_operation(
    const std::span<const katana::ir::Function> functions,
    const katana::ir::Operation operation
) {
    std::size_t count = 0;

    for (const auto& function : functions) {
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

}

int main() {
    using katana::ir::Operation;
    using katana::sh4::InstructionKind;

    const auto store_b = katana::sh4::decode(0x2214u);
    const auto store_w = katana::sh4::decode(0x2435u);
    const auto store_l = katana::sh4::decode(0x2656u);
    const auto load_b = katana::sh4::decode(0x6874u);
    const auto load_w = katana::sh4::decode(0x6A95u);
    const auto load_l = katana::sh4::decode(0x6CB6u);
    const auto same_store = katana::sh4::decode(0x2DD4u);
    const auto same_load = katana::sh4::decode(0x6EE4u);

    require(
        store_b.kind ==
            InstructionKind::MovByteStorePreDecrement &&
        store_b.source_register == 1u &&
        store_b.destination_register == 2u &&
        store_b.text == "mov.b r1, @-r2",
        "MOV.B Pre-Decrement wurde falsch dekodiert."
    );

    require(
        store_w.kind ==
            InstructionKind::MovWordStorePreDecrement &&
        store_w.source_register == 3u &&
        store_w.destination_register == 4u &&
        store_w.text == "mov.w r3, @-r4",
        "MOV.W Pre-Decrement wurde falsch dekodiert."
    );

    require(
        store_l.kind ==
            InstructionKind::MovLongStorePreDecrement &&
        store_l.source_register == 5u &&
        store_l.destination_register == 6u &&
        store_l.text == "mov.l r5, @-r6",
        "MOV.L Pre-Decrement wurde falsch dekodiert."
    );

    require(
        load_b.kind ==
            InstructionKind::MovByteLoadPostIncrement &&
        load_b.source_register == 7u &&
        load_b.destination_register == 8u &&
        load_b.text == "mov.b @r7+, r8",
        "MOV.B Post-Increment wurde falsch dekodiert."
    );

    require(
        load_w.kind ==
            InstructionKind::MovWordLoadPostIncrement &&
        load_w.source_register == 9u &&
        load_w.destination_register == 10u &&
        load_w.text == "mov.w @r9+, r10",
        "MOV.W Post-Increment wurde falsch dekodiert."
    );

    require(
        load_l.kind ==
            InstructionKind::MovLongLoadPostIncrement &&
        load_l.source_register == 11u &&
        load_l.destination_register == 12u &&
        load_l.text == "mov.l @r11+, r12",
        "MOV.L Post-Increment wurde falsch dekodiert."
    );

    require(
        same_store.source_register ==
            same_store.destination_register &&
        same_store.text == "mov.b r13, @-r13",
        "Identischer Pre-Decrement-Registerfall wurde falsch dekodiert."
    );

    require(
        same_load.source_register ==
            same_load.destination_register &&
        same_load.text == "mov.b @r14+, r14",
        "Identischer Post-Increment-Registerfall wurde falsch dekodiert."
    );

    constexpr std::array<std::uint8_t, 20> bytes = {
        0x14, 0x22,
        0x35, 0x24,
        0x56, 0x26,
        0x74, 0x68,
        0x95, 0x6A,
        0xB6, 0x6C,
        0xD4, 0x2D,
        0xE4, 0x6E,
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
        "Der KR-1401-Unit-Test muss genau eine Funktion besitzen."
    );

    require(
        count_operation(
            program,
            Operation::StoreBytePreDecrement
        ) == 2u,
        "StoreBytePreDecrement wurde nicht zweimal abgesenkt."
    );

    require(
        count_operation(
            program,
            Operation::StoreWordPreDecrement
        ) == 1u &&
        count_operation(
            program,
            Operation::StoreLongPreDecrement
        ) == 1u,
        "Word- oder Long-Pre-Decrement wurde falsch abgesenkt."
    );

    require(
        count_operation(
            program,
            Operation::LoadByteSignedPostIncrement
        ) == 2u,
        "LoadByteSignedPostIncrement wurde nicht zweimal abgesenkt."
    );

    require(
        count_operation(
            program,
            Operation::LoadWordSignedPostIncrement
        ) == 1u &&
        count_operation(
            program,
            Operation::LoadLongPostIncrement
        ) == 1u,
        "Word- oder Long-Post-Increment wurde falsch abgesenkt."
    );

    const auto source =
        katana::codegen::emit_cpp_program(
            program,
            0x8C010000u
        );

    require(
        source.find(
            "const std::uint32_t address = cpu.r[2] - 1u;"
        ) != std::string::npos,
        "Byte-Pre-Decrement berechnet die Adresse nicht vor dem Store."
    );

    require(
        source.find(
            "const std::uint32_t value = cpu.r[13];"
        ) != std::string::npos,
        "Der identische Pre-Decrement-Fall sichert den alten Quellwert nicht."
    );

    require(
        source.find("const bool same_register = true;") !=
            std::string::npos &&
        source.find("const bool same_register = false;") !=
            std::string::npos,
        "Der Post-Increment-Sonderfall wird im Codegen nicht unterschieden."
    );

    require(
        source.find("if (!same_register)") != std::string::npos,
        "Post-Increment besitzt keine Schutzbedingung fuer identische Register."
    );

    require(
        source.find("cpu.memory.read_s8(address)") !=
            std::string::npos &&
        source.find("cpu.memory.read_s16(address)") !=
            std::string::npos &&
        source.find("cpu.memory.read_u32(address)") !=
            std::string::npos,
        "Post-Increment-Loads verwenden falsche Speicherbreiten."
    );

    std::cout
        << "Alle KR-1401 Decoder-, IR- und Codegen-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}

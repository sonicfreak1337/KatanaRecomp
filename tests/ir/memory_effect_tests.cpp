#include "katana/analysis/function_analysis.hpp"
#include "katana/ir/ir.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
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
    using katana::ir::AddressUpdateKind;
    using katana::ir::instruction_memory_effects;
    using katana::ir::MemoryAccessKind;
    using katana::ir::OperandWidth;
    using katana::ir::Operation;

    const auto load = instruction_memory_effects(Operation::LoadByteSigned);
    require(load.access == MemoryAccessKind::Read && load.width == OperandWidth::Bits8 &&
                load.access_count == 1u && load.address_update == AddressUpdateKind::None,
            "Einfacher Byte-Load besitzt falsche Speichereffekte.");

    const auto store = instruction_memory_effects(Operation::StoreLongPreDecrement);
    require(store.access == MemoryAccessKind::Write && store.width == OperandWidth::Bits32 &&
                store.address_update == AddressUpdateKind::PreDecrement &&
                store.updated_register_count == 1u,
            "Pre-Decrement-Store modelliert seine Adressaenderung nicht.");

    const auto mac = instruction_memory_effects(Operation::MultiplyAccumulateWord);
    require(mac.access == MemoryAccessKind::Read && mac.width == OperandWidth::Bits16 &&
                mac.access_count == 2u && mac.address_update == AddressUpdateKind::PostIncrement &&
                mac.updated_register_count == 2u,
            "MAC.W modelliert Doppelzugriff oder Registerupdates nicht.");

    const auto add = instruction_memory_effects(Operation::AddRegister);
    require(add.access == MemoryAccessKind::None && add.width == OperandWidth::None &&
                add.access_count == 0u,
            "Speicherneutrale Operation besitzt erfundene Effekte.");

    constexpr std::array postincrement_operations = {Operation::LoadByteSignedPostIncrement,
                                                     Operation::LoadWordSignedPostIncrement,
                                                     Operation::LoadLongPostIncrement};
    for (const auto operation : postincrement_operations) {
        const auto distinct = instruction_memory_effects(operation, 1u, 2u);
        const auto same = instruction_memory_effects(operation, 2u, 2u);
        require(distinct.address_update == AddressUpdateKind::PostIncrement &&
                    distinct.updated_register_count == 1u &&
                    same.address_update == AddressUpdateKind::None &&
                    same.updated_register_count == 0u,
                "MOV.B/W/L Post-Increment beruecksichtigt Rm == Rn nicht.");
    }

    constexpr std::array<std::uint8_t, 6> bytes = {
        0x20u,
        0x61u, // MOV.B @R2,R1
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP
    };
    const auto lines = katana::sh4::disassemble(bytes, 0x8C010000u);
    constexpr std::array<std::uint32_t, 1> seeds = {0x8C010000u};
    const auto functions = katana::analysis::discover_functions(lines, seeds);
    const auto program = katana::ir::lower_program(lines, functions);
    const auto& lowered = program.front().blocks.front().instructions.front();
    require(lowered.memory_effects.access == MemoryAccessKind::Read &&
                lowered.memory_effects.width == OperandWidth::Bits8,
            "Lowering uebernimmt die Speichereffekte nicht in die IR.");

    constexpr std::array<std::uint8_t, 6> same_register_bytes = {
        0x24u,
        0x62u, // MOV.B @R2+,R2
        0x0Bu,
        0x00u, // RTS
        0x09u,
        0x00u // NOP
    };
    const auto same_lines = katana::sh4::disassemble(same_register_bytes, 0x8C020000u);
    constexpr std::array<std::uint32_t, 1> same_seeds = {0x8C020000u};
    const auto same_functions = katana::analysis::discover_functions(same_lines, same_seeds);
    const auto same_program = katana::ir::lower_program(same_lines, same_functions);
    const auto& same_lowered = same_program.front().blocks.front().instructions.front();
    require(same_lowered.operation == Operation::LoadByteSignedPostIncrement &&
                same_lowered.destination_register == same_lowered.source_register &&
                same_lowered.memory_effects.updated_register_count == 0u &&
                same_lowered.memory_effects.address_update == AddressUpdateKind::None,
            "Lowering meldet bei MOV.B @Rm+,Rm ein nicht stattfindendes Update.");

    std::cout << "KR-1903 Speicher-Seiteneffekte erfolgreich.\n";
    return EXIT_SUCCESS;
}

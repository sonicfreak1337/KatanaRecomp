$ErrorActionPreference = "Stop"

$projekt = (Get-Location).Path
$cmakeDatei = Join-Path $projekt "CMakeLists.txt"

if (-not (Test-Path $cmakeDatei)) {
    throw "CMakeLists.txt wurde im aktuellen Ordner nicht gefunden. Starte das Skript im KatanaRecomp-Projektordner."
}

$zeit = Get-Date -Format "yyyyMMdd_HHmmss"
$backup = Join-Path $projekt ".katana_backup_control_flow_$zeit"

New-Item -ItemType Directory -Force $backup | Out-Null

$zuSichern = @(
    "include\katana\sh4\instruction.hpp",
    "include\katana\sh4\decoder.hpp",
    "src\decoder\decoder.cpp",
    "include\katana\sh4\disassembler.hpp",
    "src\analysis\disassembler.cpp",
    "src\cli\main.cpp",
    "CMakeLists.txt"
)

foreach ($relativ in $zuSichern) {
    $quelle = Join-Path $projekt $relativ

    if (Test-Path $quelle) {
        $ziel = Join-Path $backup $relativ
        $zielOrdner = Split-Path $ziel -Parent
        New-Item -ItemType Directory -Force $zielOrdner | Out-Null
        Copy-Item $quelle $ziel
    }
}

New-Item -ItemType Directory -Force `
    ".\include\katana\sh4", `
    ".\src\decoder", `
    ".\src\analysis", `
    ".\src\cli", `
    ".\tests\control_flow" | Out-Null

@'
#pragma once

#include <cstdint>
#include <string>

namespace katana::sh4 {

enum class InstructionKind {
    Unknown,
    Nop,
    Rts,
    MovImmediate,
    AddImmediate,
    MovRegister,
    AddRegister,
    Bra,
    Bsr,
    Bt,
    Bf,
    BtS,
    BfS,
    Jmp,
    Jsr
};

enum class ControlFlowKind {
    None,
    UnconditionalBranch,
    ConditionalBranch,
    Call,
    Return,
    IndirectBranch,
    IndirectCall
};

struct DecodedInstruction {
    std::uint16_t opcode = 0;
    InstructionKind kind = InstructionKind::Unknown;

    std::uint8_t destination_register = 0;
    std::uint8_t source_register = 0;
    std::uint8_t branch_register = 0;

    std::int32_t immediate = 0;
    std::int32_t displacement = 0;

    ControlFlowKind control_flow = ControlFlowKind::None;
    bool has_delay_slot = false;

    std::string text;

    [[nodiscard]] bool is_known() const noexcept {
        return kind != InstructionKind::Unknown;
    }

    [[nodiscard]] bool changes_control_flow() const noexcept {
        return control_flow != ControlFlowKind::None;
    }
};

}
'@ | Set-Content ".\include\katana\sh4\instruction.hpp" -Encoding utf8

@'
#pragma once

#include "katana/sh4/instruction.hpp"

#include <cstdint>
#include <optional>

namespace katana::sh4 {

[[nodiscard]] DecodedInstruction decode(std::uint16_t opcode);

[[nodiscard]] std::optional<std::uint32_t>
calculate_direct_branch_target(
    const DecodedInstruction& instruction,
    std::uint32_t instruction_address
);

}
'@ | Set-Content ".\include\katana\sh4\decoder.hpp" -Encoding utf8

@'
#include "katana/sh4/decoder.hpp"

#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace katana::sh4 {
namespace {

std::string register_name(const std::uint8_t index) {
    return "r" + std::to_string(index);
}

std::int32_t sign_extend_8(const std::uint16_t value) {
    const auto raw = static_cast<std::uint8_t>(value & 0x00FFu);

    if ((raw & 0x80u) == 0u) {
        return static_cast<std::int32_t>(raw);
    }

    return static_cast<std::int32_t>(raw) - 0x100;
}

std::int32_t sign_extend_12(const std::uint16_t value) {
    const auto raw = static_cast<std::uint16_t>(value & 0x0FFFu);

    if ((raw & 0x0800u) == 0u) {
        return static_cast<std::int32_t>(raw);
    }

    return static_cast<std::int32_t>(raw) - 0x1000;
}

std::string unknown_instruction_text(const std::uint16_t opcode) {
    std::ostringstream output;

    output
        << ".word 0x"
        << std::hex
        << std::uppercase
        << std::setw(4)
        << std::setfill('0')
        << opcode;

    return output.str();
}

}

DecodedInstruction decode(const std::uint16_t opcode) {
    DecodedInstruction instruction;
    instruction.opcode = opcode;

    if (opcode == 0x0009u) {
        instruction.kind = InstructionKind::Nop;
        instruction.text = "nop";
        return instruction;
    }

    if (opcode == 0x000Bu) {
        instruction.kind = InstructionKind::Rts;
        instruction.control_flow = ControlFlowKind::Return;
        instruction.has_delay_slot = true;
        instruction.text = "rts";
        return instruction;
    }

    if ((opcode & 0xF000u) == 0xA000u) {
        instruction.kind = InstructionKind::Bra;
        instruction.displacement = sign_extend_12(opcode) * 2;
        instruction.control_flow = ControlFlowKind::UnconditionalBranch;
        instruction.has_delay_slot = true;
        instruction.text = "bra";
        return instruction;
    }

    if ((opcode & 0xF000u) == 0xB000u) {
        instruction.kind = InstructionKind::Bsr;
        instruction.displacement = sign_extend_12(opcode) * 2;
        instruction.control_flow = ControlFlowKind::Call;
        instruction.has_delay_slot = true;
        instruction.text = "bsr";
        return instruction;
    }

    if ((opcode & 0xFF00u) == 0x8900u) {
        instruction.kind = InstructionKind::Bt;
        instruction.displacement = sign_extend_8(opcode) * 2;
        instruction.control_flow = ControlFlowKind::ConditionalBranch;
        instruction.text = "bt";
        return instruction;
    }

    if ((opcode & 0xFF00u) == 0x8B00u) {
        instruction.kind = InstructionKind::Bf;
        instruction.displacement = sign_extend_8(opcode) * 2;
        instruction.control_flow = ControlFlowKind::ConditionalBranch;
        instruction.text = "bf";
        return instruction;
    }

    if ((opcode & 0xFF00u) == 0x8D00u) {
        instruction.kind = InstructionKind::BtS;
        instruction.displacement = sign_extend_8(opcode) * 2;
        instruction.control_flow = ControlFlowKind::ConditionalBranch;
        instruction.has_delay_slot = true;
        instruction.text = "bt/s";
        return instruction;
    }

    if ((opcode & 0xFF00u) == 0x8F00u) {
        instruction.kind = InstructionKind::BfS;
        instruction.displacement = sign_extend_8(opcode) * 2;
        instruction.control_flow = ControlFlowKind::ConditionalBranch;
        instruction.has_delay_slot = true;
        instruction.text = "bf/s";
        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x402Bu) {
        instruction.kind = InstructionKind::Jmp;
        instruction.branch_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.control_flow = ControlFlowKind::IndirectBranch;
        instruction.has_delay_slot = true;
        instruction.text =
            "jmp @" + register_name(instruction.branch_register);
        return instruction;
    }

    if ((opcode & 0xF0FFu) == 0x400Bu) {
        instruction.kind = InstructionKind::Jsr;
        instruction.branch_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.control_flow = ControlFlowKind::IndirectCall;
        instruction.has_delay_slot = true;
        instruction.text =
            "jsr @" + register_name(instruction.branch_register);
        return instruction;
    }

    if ((opcode & 0xF000u) == 0xE000u) {
        instruction.kind = InstructionKind::MovImmediate;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.immediate = sign_extend_8(opcode);

        instruction.text =
            "mov #" +
            std::to_string(instruction.immediate) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF000u) == 0x7000u) {
        instruction.kind = InstructionKind::AddImmediate;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.immediate = sign_extend_8(opcode);

        instruction.text =
            "add #" +
            std::to_string(instruction.immediate) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x6003u) {
        instruction.kind = InstructionKind::MovRegister;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.source_register =
            static_cast<std::uint8_t>((opcode >> 4u) & 0x0Fu);

        instruction.text =
            "mov " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    if ((opcode & 0xF00Fu) == 0x300Cu) {
        instruction.kind = InstructionKind::AddRegister;
        instruction.destination_register =
            static_cast<std::uint8_t>((opcode >> 8u) & 0x0Fu);
        instruction.source_register =
            static_cast<std::uint8_t>((opcode >> 4u) & 0x0Fu);

        instruction.text =
            "add " +
            register_name(instruction.source_register) +
            ", " +
            register_name(instruction.destination_register);

        return instruction;
    }

    instruction.text = unknown_instruction_text(opcode);
    return instruction;
}

std::optional<std::uint32_t> calculate_direct_branch_target(
    const DecodedInstruction& instruction,
    const std::uint32_t instruction_address
) {
    switch (instruction.kind) {
        case InstructionKind::Bra:
        case InstructionKind::Bsr:
        case InstructionKind::Bt:
        case InstructionKind::Bf:
        case InstructionKind::BtS:
        case InstructionKind::BfS:
            break;

        default:
            return std::nullopt;
    }

    const auto expanded_target =
        static_cast<std::int64_t>(instruction_address) +
        4 +
        static_cast<std::int64_t>(instruction.displacement);

    return static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(expanded_target) & 0xFFFFFFFFull
    );
}

}
'@ | Set-Content ".\src\decoder\decoder.cpp" -Encoding utf8

@'
#pragma once

#include "katana/sh4/instruction.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace katana::sh4 {

struct DisassemblyLine {
    std::uint32_t address = 0;
    std::uint16_t opcode = 0;
    DecodedInstruction instruction;

    bool is_delay_slot = false;
    std::optional<std::uint32_t> target_address;
};

[[nodiscard]] std::vector<DisassemblyLine> disassemble(
    std::span<const std::uint8_t> bytes,
    std::uint32_t base_address = 0
);

}
'@ | Set-Content ".\include\katana\sh4\disassembler.hpp" -Encoding utf8

@'
#include "katana/sh4/disassembler.hpp"

#include "katana/io/binary_reader.hpp"
#include "katana/sh4/decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace katana::sh4 {

std::vector<DisassemblyLine> disassemble(
    const std::span<const std::uint8_t> bytes,
    const std::uint32_t base_address
) {
    if ((bytes.size() % 2u) != 0u) {
        throw std::invalid_argument(
            "Die Binärdatei besitzt eine ungerade Anzahl Bytes."
        );
    }

    std::vector<DisassemblyLine> lines;
    lines.reserve(bytes.size() / 2u);

    bool previous_instruction_has_delay_slot = false;

    for (std::size_t offset = 0; offset < bytes.size(); offset += 2u) {
        const auto expanded_address =
            static_cast<std::uint64_t>(base_address) +
            static_cast<std::uint64_t>(offset);

        if (
            expanded_address >
            std::numeric_limits<std::uint32_t>::max()
        ) {
            throw std::overflow_error(
                "Die berechnete Instruktionsadresse überschreitet 32 Bit."
            );
        }

        const auto opcode = katana::io::read_u16_le(bytes, offset);

        DisassemblyLine line;
        line.address = static_cast<std::uint32_t>(expanded_address);
        line.opcode = opcode;
        line.instruction = decode(opcode);
        line.is_delay_slot = previous_instruction_has_delay_slot;
        line.target_address = calculate_direct_branch_target(
            line.instruction,
            line.address
        );

        previous_instruction_has_delay_slot =
            line.instruction.has_delay_slot;

        lines.push_back(line);
    }

    return lines;
}

}
'@ | Set-Content ".\src\analysis\disassembler.cpp" -Encoding utf8

@'
#include "katana/io/binary_reader.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/disassembler.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::uint32_t parse_hex_value(
    std::string text,
    const std::uint32_t maximum,
    const std::string& description
) {
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.erase(0, 2);
    }

    if (text.empty()) {
        throw std::invalid_argument(
            description + " darf nicht leer sein."
        );
    }

    const auto is_valid_hex = std::all_of(
        text.begin(),
        text.end(),
        [](const unsigned char character) {
            return std::isxdigit(character) != 0;
        }
    );

    if (!is_valid_hex) {
        throw std::invalid_argument(
            description + " enthält ungültige Hex-Zeichen."
        );
    }

    std::size_t parsed_characters = 0;
    const auto value = std::stoull(
        text,
        &parsed_characters,
        16
    );

    if (
        parsed_characters != text.length() ||
        value > maximum
    ) {
        throw std::invalid_argument(
            description + " liegt außerhalb des erlaubten Bereichs."
        );
    }

    return static_cast<std::uint32_t>(value);
}

std::string format_disassembly_text(
    const katana::sh4::DisassemblyLine& line
) {
    std::ostringstream output;
    output << line.instruction.text;

    if (line.target_address.has_value()) {
        output
            << " 0x"
            << std::hex
            << std::uppercase
            << std::setw(8)
            << std::setfill('0')
            << *line.target_address;
    }

    if (line.is_delay_slot) {
        output << "  ; delay slot";
    }

    return output.str();
}

int decode_single_opcode(const std::string& text) {
    const auto opcode = static_cast<std::uint16_t>(
        parse_hex_value(text, 0xFFFFu, "Der Opcode")
    );

    const auto instruction = katana::sh4::decode(opcode);

    std::cout
        << "Opcode:      0x"
        << std::hex
        << std::uppercase
        << std::setw(4)
        << std::setfill('0')
        << opcode
        << '\n'
        << "Instruktion: "
        << instruction.text
        << '\n'
        << "Status:      "
        << (instruction.is_known() ? "erkannt" : "unbekannt")
        << '\n'
        << "Kontrollfluss: "
        << (instruction.changes_control_flow() ? "ja" : "nein")
        << '\n'
        << "Delay Slot:   "
        << (instruction.has_delay_slot ? "ja" : "nein")
        << '\n';

    return instruction.is_known() ? 0 : 1;
}

int disassemble_file(
    const std::filesystem::path& path,
    const std::uint32_t base_address
) {
    const auto bytes = katana::io::read_binary_file(path);
    const auto lines = katana::sh4::disassemble(
        bytes,
        base_address
    );

    std::size_t unknown_count = 0;
    std::size_t control_flow_count = 0;
    std::size_t delay_slot_count = 0;

    std::cout
        << "Datei:        " << path.string() << '\n'
        << "Größe:        " << std::dec << bytes.size() << " Bytes\n"
        << "Basisadresse: 0x"
        << std::hex
        << std::uppercase
        << std::setw(8)
        << std::setfill('0')
        << base_address
        << "\n\n";

    for (const auto& line : lines) {
        if (!line.instruction.is_known()) {
            ++unknown_count;
        }

        if (line.instruction.changes_control_flow()) {
            ++control_flow_count;
        }

        if (line.is_delay_slot) {
            ++delay_slot_count;
        }

        std::cout
            << "0x"
            << std::hex
            << std::uppercase
            << std::setw(8)
            << std::setfill('0')
            << line.address
            << "  "
            << std::setw(4)
            << line.opcode
            << "  "
            << format_disassembly_text(line)
            << '\n';
    }

    std::cout
        << "\nInstruktionen:       "
        << std::dec
        << lines.size()
        << "\nKontrollfluss:       "
        << control_flow_count
        << "\nMarkierte Delay Slots: "
        << delay_slot_count
        << "\nUnbekannte Opcodes:  "
        << unknown_count
        << '\n';

    return 0;
}

void print_usage() {
    std::cerr
        << "Verwendung:\n"
        << "  katana-recomp <Opcode>\n"
        << "  katana-recomp opcode <Opcode>\n"
        << "  katana-recomp disasm <Datei> [Basisadresse]\n\n"
        << "Beispiele:\n"
        << "  katana-recomp E1FF\n"
        << "  katana-recomp opcode A001\n"
        << "  katana-recomp disasm programm.bin 8C010000\n";
}

}

int main(const int argc, char* argv[]) {
    try {
        if (argc == 2) {
            return decode_single_opcode(argv[1]);
        }

        if (
            argc == 3 &&
            std::string(argv[1]) == "opcode"
        ) {
            return decode_single_opcode(argv[2]);
        }

        if (
            (argc == 3 || argc == 4) &&
            std::string(argv[1]) == "disasm"
        ) {
            const auto base_address =
                argc == 4
                    ? parse_hex_value(
                        argv[3],
                        std::numeric_limits<std::uint32_t>::max(),
                        "Die Basisadresse"
                    )
                    : 0u;

            return disassemble_file(
                std::filesystem::path(argv[2]),
                base_address
            );
        }

        print_usage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Fehler: " << error.what() << '\n';
        return 2;
    }
}
'@ | Set-Content ".\src\cli\main.cpp" -Encoding utf8

@'
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
        "BRA wurde nicht als verzögerter Sprung markiert."
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
        "BT/S wurde nicht als verzögerter Sprung markiert."
    );

    const auto bf_s = decode(0x8FFFu);
    require(
        bf_s.kind == InstructionKind::BfS,
        "BF/S wurde nicht erkannt."
    );
    require(
        bf_s.has_delay_slot,
        "BF/S wurde nicht als verzögerter Sprung markiert."
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
        "JMP wurde nicht als verzögerter Sprung markiert."
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
        "RTS wurde nicht als Rücksprung markiert."
    );
    require(
        rts.has_delay_slot,
        "RTS wurde nicht als verzögerter Rücksprung markiert."
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
        "BRA selbst wurde fälschlich als Delay Slot markiert."
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
'@ | Set-Content ".\tests\control_flow\control_flow_tests.cpp" -Encoding utf8

@'
cmake_minimum_required(VERSION 3.25)

project(
    KatanaRecomp
    VERSION 0.3.0
    DESCRIPTION "Dreamcast SH-4 static recompilation framework"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(katana_enable_warnings target)
    if(MSVC)
        target_compile_options(
            ${target}
            PRIVATE
                /W4
                /permissive-
                /EHsc
        )
    else()
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()
endfunction()

add_library(katana_core STATIC
    src/decoder/decoder.cpp
    src/io/binary_reader.cpp
    src/analysis/disassembler.cpp
)

target_include_directories(
    katana_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

katana_enable_warnings(katana_core)

add_executable(katana-recomp
    src/cli/main.cpp
)

target_link_libraries(
    katana-recomp
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-recomp)

enable_testing()

add_executable(katana-decoder-tests
    tests/decoder/decoder_tests.cpp
)

target_link_libraries(
    katana-decoder-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-decoder-tests)

add_test(
    NAME katana-decoder-tests
    COMMAND katana-decoder-tests
)

add_executable(katana-disassembler-tests
    tests/disassembler/disassembler_tests.cpp
)

target_link_libraries(
    katana-disassembler-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-disassembler-tests)

add_test(
    NAME katana-disassembler-tests
    COMMAND katana-disassembler-tests
)

add_executable(katana-control-flow-tests
    tests/control_flow/control_flow_tests.cpp
)

target_link_libraries(
    katana-control-flow-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-control-flow-tests)

add_test(
    NAME katana-control-flow-tests
    COMMAND katana-control-flow-tests
)
'@ | Set-Content ".\CMakeLists.txt" -Encoding utf8

Write-Host ""
Write-Host "Kontrollfluss-Meilenstein wurde angewendet." -ForegroundColor Green
Write-Host "Sicherung: $backup"
Write-Host ""
Write-Host "Jetzt ausführen:"
Write-Host '  Remove-Item ".\build" -Recurse -Force -ErrorAction SilentlyContinue'
Write-Host '  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug'
Write-Host '  cmake --build build'
Write-Host '  ctest --test-dir build --output-on-failure'

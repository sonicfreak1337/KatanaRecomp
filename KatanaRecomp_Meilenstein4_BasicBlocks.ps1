$ErrorActionPreference = "Stop"

$projekt = (Get-Location).Path
$cmakeDatei = Join-Path $projekt "CMakeLists.txt"

if (-not (Test-Path $cmakeDatei)) {
    throw "CMakeLists.txt wurde nicht gefunden. Starte das Skript im KatanaRecomp-Projektordner."
}

$zeit = Get-Date -Format "yyyyMMdd_HHmmss"
$backup = Join-Path $projekt ".katana_backup_basic_blocks_$zeit"

New-Item -ItemType Directory -Force $backup | Out-Null

$zuSichern = @(
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
    ".\include\katana\analysis", `
    ".\src\analysis", `
    ".\tests\basic_blocks" | Out-Null

@'
#pragma once

#include "katana/sh4/disassembler.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace katana::analysis {

struct BasicBlock {
    std::size_t id = 0;
    std::uint32_t start_address = 0;
    std::uint32_t end_address = 0;

    std::vector<katana::sh4::DisassemblyLine> lines;
    std::vector<std::uint32_t> successors;

    bool has_indirect_successor = false;
};

[[nodiscard]] std::vector<BasicBlock> build_basic_blocks(
    std::span<const katana::sh4::DisassemblyLine> lines
);

}
'@ | Set-Content ".\include\katana\analysis\basic_blocks.hpp" -Encoding ascii

@'
#include "katana/analysis/basic_blocks.hpp"

#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace katana::analysis {
namespace {

std::size_t terminal_index(
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::size_t control_index
) {
    if (
        lines[control_index].instruction.has_delay_slot &&
        control_index + 1u < lines.size()
    ) {
        return control_index + 1u;
    }

    return control_index;
}

void add_unique_successor(
    std::vector<std::uint32_t>& successors,
    const std::uint32_t address
) {
    if (
        std::find(
            successors.begin(),
            successors.end(),
            address
        ) == successors.end()
    ) {
        successors.push_back(address);
    }
}

std::size_t find_controlling_instruction(
    const BasicBlock& block
) {
    if (block.lines.empty()) {
        return 0;
    }

    const auto last = block.lines.size() - 1u;

    if (
        block.lines[last].is_delay_slot &&
        last > 0u
    ) {
        return last - 1u;
    }

    return last;
}

}

std::vector<BasicBlock> build_basic_blocks(
    const std::span<const katana::sh4::DisassemblyLine> lines
) {
    if (lines.empty()) {
        return {};
    }

    std::unordered_map<std::uint32_t, std::size_t> address_to_index;
    address_to_index.reserve(lines.size());

    for (std::size_t index = 0; index < lines.size(); ++index) {
        address_to_index.emplace(lines[index].address, index);
    }

    std::set<std::size_t> leaders;
    leaders.insert(0u);

    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto& line = lines[index];

        if (line.target_address.has_value()) {
            const auto target = address_to_index.find(
                *line.target_address
            );

            if (target != address_to_index.end()) {
                leaders.insert(target->second);
            }
        }

        if (!line.instruction.changes_control_flow()) {
            continue;
        }

        const auto end_index = terminal_index(lines, index);
        const auto next_index = end_index + 1u;

        if (next_index < lines.size()) {
            leaders.insert(next_index);
        }
    }

    const std::vector<std::size_t> ordered_leaders(
        leaders.begin(),
        leaders.end()
    );

    std::vector<BasicBlock> blocks;
    blocks.reserve(ordered_leaders.size());

    for (
        std::size_t leader_position = 0;
        leader_position < ordered_leaders.size();
        ++leader_position
    ) {
        const auto first_index = ordered_leaders[leader_position];

        const auto end_exclusive =
            leader_position + 1u < ordered_leaders.size()
                ? ordered_leaders[leader_position + 1u]
                : lines.size();

        BasicBlock block;
        block.id = blocks.size();
        block.start_address = lines[first_index].address;
        block.end_address = lines[end_exclusive - 1u].address;

        block.lines.insert(
            block.lines.end(),
            lines.begin() + static_cast<std::ptrdiff_t>(first_index),
            lines.begin() + static_cast<std::ptrdiff_t>(end_exclusive)
        );

        blocks.push_back(std::move(block));
    }

    std::unordered_map<std::uint32_t, std::size_t> block_by_start;
    block_by_start.reserve(blocks.size());

    for (std::size_t index = 0; index < blocks.size(); ++index) {
        block_by_start.emplace(blocks[index].start_address, index);
    }

    for (std::size_t block_index = 0; block_index < blocks.size(); ++block_index) {
        auto& block = blocks[block_index];

        if (block.lines.empty()) {
            continue;
        }

        const auto control_index = find_controlling_instruction(block);
        const auto& control_line = block.lines[control_index];
        const auto& instruction = control_line.instruction;

        const auto next_block_address =
            block_index + 1u < blocks.size()
                ? blocks[block_index + 1u].start_address
                : 0u;

        if (!instruction.changes_control_flow()) {
            if (block_index + 1u < blocks.size()) {
                add_unique_successor(
                    block.successors,
                    next_block_address
                );
            }

            continue;
        }

        if (control_line.target_address.has_value()) {
            add_unique_successor(
                block.successors,
                *control_line.target_address
            );
        }

        switch (instruction.control_flow) {
            case katana::sh4::ControlFlowKind::ConditionalBranch:
                if (block_index + 1u < blocks.size()) {
                    add_unique_successor(
                        block.successors,
                        next_block_address
                    );
                }
                break;

            case katana::sh4::ControlFlowKind::Call:
                if (block_index + 1u < blocks.size()) {
                    add_unique_successor(
                        block.successors,
                        next_block_address
                    );
                }
                break;

            case katana::sh4::ControlFlowKind::IndirectCall:
                block.has_indirect_successor = true;

                if (block_index + 1u < blocks.size()) {
                    add_unique_successor(
                        block.successors,
                        next_block_address
                    );
                }
                break;

            case katana::sh4::ControlFlowKind::IndirectBranch:
                block.has_indirect_successor = true;
                break;

            case katana::sh4::ControlFlowKind::UnconditionalBranch:
            case katana::sh4::ControlFlowKind::Return:
            case katana::sh4::ControlFlowKind::None:
                break;
        }

        std::sort(
            block.successors.begin(),
            block.successors.end()
        );
    }

    return blocks;
}

}
'@ | Set-Content ".\src\analysis\basic_blocks.cpp" -Encoding ascii

@'
#include "katana/analysis/basic_blocks.hpp"
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
            description + " enthaelt ungueltige Hex-Zeichen."
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
            description + " liegt ausserhalb des erlaubten Bereichs."
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

void print_address(const std::uint32_t address) {
    std::cout
        << "0x"
        << std::hex
        << std::uppercase
        << std::setw(8)
        << std::setfill('0')
        << address;
}

int decode_single_opcode(const std::string& text) {
    const auto opcode = static_cast<std::uint16_t>(
        parse_hex_value(text, 0xFFFFu, "Der Opcode")
    );

    const auto instruction = katana::sh4::decode(opcode);

    std::cout
        << "Opcode:        0x"
        << std::hex
        << std::uppercase
        << std::setw(4)
        << std::setfill('0')
        << opcode
        << '\n'
        << "Instruktion:   "
        << instruction.text
        << '\n'
        << "Status:        "
        << (instruction.is_known() ? "erkannt" : "unbekannt")
        << '\n'
        << "Kontrollfluss: "
        << (instruction.changes_control_flow() ? "ja" : "nein")
        << '\n'
        << "Delay Slot:    "
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
        << "Datei:         " << path.string() << '\n'
        << "Dateigroesse:  " << std::dec << bytes.size() << " Bytes\n"
        << "Basisadresse:  0x"
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

        print_address(line.address);

        std::cout
            << "  "
            << std::setw(4)
            << line.opcode
            << "  "
            << format_disassembly_text(line)
            << '\n';
    }

    std::cout
        << "\nInstruktionen:         "
        << std::dec
        << lines.size()
        << "\nKontrollfluss:         "
        << control_flow_count
        << "\nMarkierte Delay Slots: "
        << delay_slot_count
        << "\nUnbekannte Opcodes:    "
        << unknown_count
        << '\n';

    return 0;
}

int analyze_blocks(
    const std::filesystem::path& path,
    const std::uint32_t base_address
) {
    const auto bytes = katana::io::read_binary_file(path);
    const auto lines = katana::sh4::disassemble(
        bytes,
        base_address
    );
    const auto blocks = katana::analysis::build_basic_blocks(lines);

    std::cout
        << "Datei:         " << path.string() << '\n'
        << "Dateigroesse:  " << std::dec << bytes.size() << " Bytes\n"
        << "Basic Blocks:  " << blocks.size()
        << "\n\n";

    for (const auto& block : blocks) {
        std::cout
            << "Block "
            << std::dec
            << block.id
            << ": ";

        print_address(block.start_address);
        std::cout << " - ";
        print_address(block.end_address);
        std::cout << '\n';

        for (const auto& line : block.lines) {
            std::cout << "  ";
            print_address(line.address);

            std::cout
                << "  "
                << std::setw(4)
                << line.opcode
                << "  "
                << format_disassembly_text(line)
                << '\n';
        }

        std::cout << "  Nachfolger: ";

        if (
            block.successors.empty() &&
            !block.has_indirect_successor
        ) {
            std::cout << "keine";
        } else {
            bool first = true;

            for (const auto successor : block.successors) {
                if (!first) {
                    std::cout << ", ";
                }

                print_address(successor);
                first = false;
            }

            if (block.has_indirect_successor) {
                if (!first) {
                    std::cout << ", ";
                }

                std::cout << "indirekt";
            }
        }

        std::cout << "\n\n";
    }

    return 0;
}

void print_usage() {
    std::cerr
        << "Verwendung:\n"
        << "  katana-recomp <Opcode>\n"
        << "  katana-recomp opcode <Opcode>\n"
        << "  katana-recomp disasm <Datei> [Basisadresse]\n"
        << "  katana-recomp blocks <Datei> [Basisadresse]\n\n"
        << "Beispiele:\n"
        << "  katana-recomp E1FF\n"
        << "  katana-recomp disasm programm.bin 8C010000\n"
        << "  katana-recomp blocks programm.bin 8C010000\n";
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

        if (
            (argc == 3 || argc == 4) &&
            std::string(argv[1]) == "blocks"
        ) {
            const auto base_address =
                argc == 4
                    ? parse_hex_value(
                        argv[3],
                        std::numeric_limits<std::uint32_t>::max(),
                        "Die Basisadresse"
                    )
                    : 0u;

            return analyze_blocks(
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
'@ | Set-Content ".\src\cli\main.cpp" -Encoding ascii

@'
#include "katana/analysis/basic_blocks.hpp"
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
    constexpr std::array<std::uint8_t, 20> bytes = {
        0x02, 0x89,
        0x01, 0xE0,
        0x03, 0xA0,
        0x09, 0x00,
        0x01, 0x70,
        0x0B, 0x00,
        0x09, 0x00,
        0x09, 0x00,
        0x0B, 0x00,
        0x09, 0x00
    };

    const auto lines = katana::sh4::disassemble(
        bytes,
        0x8C010000u
    );

    const auto blocks =
        katana::analysis::build_basic_blocks(lines);

    require(
        blocks.size() == 4,
        "Es wurden nicht genau vier Basic Blocks erkannt."
    );

    require(
        blocks[0].start_address == 0x8C010000u,
        "Block 0 besitzt eine falsche Startadresse."
    );
    require(
        blocks[0].end_address == 0x8C010000u,
        "Block 0 besitzt eine falsche Endadresse."
    );
    require(
        blocks[0].successors.size() == 2,
        "Block 0 muss zwei Nachfolger besitzen."
    );
    require(
        blocks[0].successors[0] == 0x8C010002u,
        "Der Fallthrough von Block 0 ist falsch."
    );
    require(
        blocks[0].successors[1] == 0x8C010008u,
        "Das Sprungziel von Block 0 ist falsch."
    );

    require(
        blocks[1].start_address == 0x8C010002u,
        "Block 1 besitzt eine falsche Startadresse."
    );
    require(
        blocks[1].end_address == 0x8C010006u,
        "Block 1 muss BRA und Delay Slot enthalten."
    );
    require(
        blocks[1].lines.size() == 3,
        "Block 1 besitzt die falsche Instruktionsanzahl."
    );
    require(
        blocks[1].successors.size() == 1,
        "Block 1 muss genau einen Nachfolger besitzen."
    );
    require(
        blocks[1].successors[0] == 0x8C01000Eu,
        "Das BRA-Ziel von Block 1 ist falsch."
    );

    require(
        blocks[2].start_address == 0x8C010008u,
        "Block 2 besitzt eine falsche Startadresse."
    );
    require(
        blocks[2].end_address == 0x8C01000Cu,
        "Block 2 muss RTS und Delay Slot enthalten."
    );
    require(
        blocks[2].successors.empty(),
        "Ein RTS-Block darf keinen direkten Nachfolger besitzen."
    );

    require(
        blocks[3].start_address == 0x8C01000Eu,
        "Block 3 besitzt eine falsche Startadresse."
    );
    require(
        blocks[3].end_address == 0x8C010012u,
        "Block 3 muss RTS und Delay Slot enthalten."
    );
    require(
        blocks[3].successors.empty(),
        "Der letzte RTS-Block darf keinen Nachfolger besitzen."
    );

    std::cout
        << "Alle Basic-Block- und CFG-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
'@ | Set-Content ".\tests\basic_blocks\basic_block_tests.cpp" -Encoding ascii

@'
cmake_minimum_required(VERSION 3.25)

project(
    KatanaRecomp
    VERSION 0.4.0
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
                /utf-8
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
    src/analysis/basic_blocks.cpp
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

add_executable(katana-basic-block-tests
    tests/basic_blocks/basic_block_tests.cpp
)

target_link_libraries(
    katana-basic-block-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-basic-block-tests)

add_test(
    NAME katana-basic-block-tests
    COMMAND katana-basic-block-tests
)
'@ | Set-Content ".\CMakeLists.txt" -Encoding ascii

Write-Host ""
Write-Host "Basic-Block- und CFG-Meilenstein wurde angewendet." -ForegroundColor Green
Write-Host "Sicherung: $backup"
Write-Host ""

Remove-Item ".\build" -Recurse -Force -ErrorAction SilentlyContinue

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

Write-Host ""
Write-Host "Erzeuge CFG-Demodatei..."

[Environment]::CurrentDirectory = (Get-Location).Path
New-Item -ItemType Directory -Force ".\samples" | Out-Null

[System.IO.File]::WriteAllBytes(
    (Join-Path (Get-Location).Path "samples\basic_blocks_demo.bin"),
    [byte[]](
        0x02, 0x89,
        0x01, 0xE0,
        0x03, 0xA0,
        0x09, 0x00,
        0x01, 0x70,
        0x0B, 0x00,
        0x09, 0x00,
        0x09, 0x00,
        0x0B, 0x00,
        0x09, 0x00
    )
)

Write-Host ""
Write-Host "Basic-Block-Analyse:"
& ".\build\katana-recomp.exe" blocks ".\samples\basic_blocks_demo.bin" 8C010000

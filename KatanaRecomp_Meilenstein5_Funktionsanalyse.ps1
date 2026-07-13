$ErrorActionPreference = "Stop"

$projekt = (Get-Location).Path
$cmakeDatei = Join-Path $projekt "CMakeLists.txt"

if (-not (Test-Path $cmakeDatei)) {
    throw "CMakeLists.txt wurde nicht gefunden. Starte das Skript im KatanaRecomp-Projektordner."
}

$zeit = Get-Date -Format "yyyyMMdd_HHmmss"
$backup = Join-Path $projekt ".katana_backup_functions_$zeit"

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
    ".\tests\functions" | Out-Null

@'
#pragma once

#include "katana/sh4/disassembler.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace katana::analysis {

struct FunctionInfo {
    std::size_t id = 0;
    std::uint32_t entry_address = 0;

    std::vector<std::uint32_t> block_addresses;
    std::vector<std::uint32_t> direct_callees;
    std::vector<std::uint32_t> indirect_call_sites;
};

[[nodiscard]] std::vector<FunctionInfo> discover_functions(
    std::span<const katana::sh4::DisassemblyLine> lines,
    std::span<const std::uint32_t> seed_entries
);

}
'@ | Set-Content ".\include\katana\analysis\function_analysis.hpp" -Encoding ascii

@'
#include "katana/analysis/function_analysis.hpp"

#include "katana/analysis/basic_blocks.hpp"
#include "katana/sh4/instruction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace katana::analysis {
namespace {

const katana::sh4::DisassemblyLine& controlling_line(
    const BasicBlock& block
) {
    const auto last_index = block.lines.size() - 1u;

    if (
        block.lines[last_index].is_delay_slot &&
        last_index > 0u
    ) {
        return block.lines[last_index - 1u];
    }

    return block.lines[last_index];
}

void add_sorted_unique(
    std::vector<std::uint32_t>& values,
    const std::uint32_t value
) {
    if (
        std::find(
            values.begin(),
            values.end(),
            value
        ) == values.end()
    ) {
        values.push_back(value);
        std::sort(values.begin(), values.end());
    }
}

}

std::vector<FunctionInfo> discover_functions(
    const std::span<const katana::sh4::DisassemblyLine> lines,
    const std::span<const std::uint32_t> seed_entries
) {
    const auto blocks = build_basic_blocks(lines);

    if (blocks.empty()) {
        return {};
    }

    std::unordered_map<std::uint32_t, std::size_t> block_by_start;
    block_by_start.reserve(blocks.size());

    for (std::size_t index = 0; index < blocks.size(); ++index) {
        block_by_start.emplace(
            blocks[index].start_address,
            index
        );
    }

    std::set<std::uint32_t> known_entries;

    for (const auto entry : seed_entries) {
        if (block_by_start.contains(entry)) {
            known_entries.insert(entry);
        }
    }

    for (const auto& block : blocks) {
        if (block.lines.empty()) {
            continue;
        }

        const auto& control = controlling_line(block);

        if (
            control.instruction.control_flow ==
                katana::sh4::ControlFlowKind::Call &&
            control.target_address.has_value() &&
            block_by_start.contains(*control.target_address)
        ) {
            known_entries.insert(*control.target_address);
        }
    }

    std::deque<std::uint32_t> pending_entries(
        known_entries.begin(),
        known_entries.end()
    );

    std::unordered_set<std::uint32_t> processed_entries;
    std::vector<FunctionInfo> functions;

    while (!pending_entries.empty()) {
        const auto entry = pending_entries.front();
        pending_entries.pop_front();

        if (processed_entries.contains(entry)) {
            continue;
        }

        const auto entry_block = block_by_start.find(entry);

        if (entry_block == block_by_start.end()) {
            continue;
        }

        processed_entries.insert(entry);

        FunctionInfo function;
        function.id = functions.size();
        function.entry_address = entry;

        std::deque<std::uint32_t> pending_blocks;
        std::unordered_set<std::uint32_t> visited_blocks;

        pending_blocks.push_back(entry);

        while (!pending_blocks.empty()) {
            const auto block_address = pending_blocks.front();
            pending_blocks.pop_front();

            if (visited_blocks.contains(block_address)) {
                continue;
            }

            if (
                block_address != entry &&
                known_entries.contains(block_address)
            ) {
                continue;
            }

            const auto block_iterator =
                block_by_start.find(block_address);

            if (block_iterator == block_by_start.end()) {
                continue;
            }

            visited_blocks.insert(block_address);
            add_sorted_unique(
                function.block_addresses,
                block_address
            );

            const auto& block = blocks[block_iterator->second];

            if (block.lines.empty()) {
                continue;
            }

            const auto& control = controlling_line(block);
            const auto flow = control.instruction.control_flow;

            if (
                flow == katana::sh4::ControlFlowKind::Call &&
                control.target_address.has_value()
            ) {
                add_sorted_unique(
                    function.direct_callees,
                    *control.target_address
                );

                if (
                    block_by_start.contains(*control.target_address) &&
                    !processed_entries.contains(*control.target_address)
                ) {
                    pending_entries.push_back(
                        *control.target_address
                    );
                }
            }

            if (
                flow ==
                katana::sh4::ControlFlowKind::IndirectCall
            ) {
                add_sorted_unique(
                    function.indirect_call_sites,
                    control.address
                );
            }

            for (const auto successor : block.successors) {
                if (
                    flow == katana::sh4::ControlFlowKind::Call &&
                    control.target_address.has_value() &&
                    successor == *control.target_address
                ) {
                    continue;
                }

                if (
                    successor != entry &&
                    known_entries.contains(successor)
                ) {
                    continue;
                }

                pending_blocks.push_back(successor);
            }
        }

        functions.push_back(std::move(function));
    }

    std::sort(
        functions.begin(),
        functions.end(),
        [](const FunctionInfo& left, const FunctionInfo& right) {
            return left.entry_address < right.entry_address;
        }
    );

    for (std::size_t index = 0; index < functions.size(); ++index) {
        functions[index].id = index;
    }

    return functions;
}

}
'@ | Set-Content ".\src\analysis\function_analysis.cpp" -Encoding ascii

@'
#include "katana/analysis/function_analysis.hpp"
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
    constexpr std::array<std::uint8_t, 16> bytes = {
        0x02, 0xB0,
        0x09, 0x00,
        0x0B, 0x00,
        0x09, 0x00,
        0x0B, 0x43,
        0x09, 0x00,
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

    require(
        functions.size() == 2,
        "Es wurden nicht genau zwei Funktionen erkannt."
    );

    const auto& main_function = functions[0];
    const auto& sub_function = functions[1];

    require(
        main_function.entry_address == 0x8C010000u,
        "Die erste Funktion besitzt die falsche Startadresse."
    );
    require(
        main_function.block_addresses.size() == 2,
        "Die erste Funktion muss zwei Basic Blocks besitzen."
    );
    require(
        main_function.block_addresses[0] == 0x8C010000u,
        "Der erste Block der Hauptfunktion ist falsch."
    );
    require(
        main_function.block_addresses[1] == 0x8C010004u,
        "Der zweite Block der Hauptfunktion ist falsch."
    );
    require(
        main_function.direct_callees.size() == 1,
        "Die Hauptfunktion muss genau einen direkten Aufruf besitzen."
    );
    require(
        main_function.direct_callees[0] == 0x8C010008u,
        "Das direkte Aufrufziel der Hauptfunktion ist falsch."
    );
    require(
        main_function.indirect_call_sites.empty(),
        "Die Hauptfunktion darf keinen indirekten Aufruf besitzen."
    );

    require(
        sub_function.entry_address == 0x8C010008u,
        "Die zweite Funktion besitzt die falsche Startadresse."
    );
    require(
        sub_function.block_addresses.size() == 2,
        "Die zweite Funktion muss zwei Basic Blocks besitzen."
    );
    require(
        sub_function.block_addresses[0] == 0x8C010008u,
        "Der erste Block der Unterfunktion ist falsch."
    );
    require(
        sub_function.block_addresses[1] == 0x8C01000Cu,
        "Der zweite Block der Unterfunktion ist falsch."
    );
    require(
        sub_function.direct_callees.empty(),
        "Die Unterfunktion darf keinen direkten Aufruf besitzen."
    );
    require(
        sub_function.indirect_call_sites.size() == 1,
        "Die Unterfunktion muss einen indirekten Aufruf besitzen."
    );
    require(
        sub_function.indirect_call_sites[0] == 0x8C010008u,
        "Die Adresse des indirekten Aufrufs ist falsch."
    );

    std::cout
        << "Alle Funktionsanalyse-Tests erfolgreich.\n";

    return EXIT_SUCCESS;
}
'@ | Set-Content ".\tests\functions\function_analysis_tests.cpp" -Encoding ascii

$main = @'
#include "katana/analysis/basic_blocks.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/io/binary_reader.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/disassembler.hpp"

#include <algorithm>
#include <array>
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

int analyze_functions(
    const std::filesystem::path& path,
    const std::uint32_t entry_address,
    const std::uint32_t base_address
) {
    const auto bytes = katana::io::read_binary_file(path);
    const auto lines = katana::sh4::disassemble(
        bytes,
        base_address
    );

    const std::array<std::uint32_t, 1> seeds = {
        entry_address
    };

    const auto functions =
        katana::analysis::discover_functions(
            lines,
            seeds
        );

    std::cout
        << "Datei:         " << path.string() << '\n'
        << "Dateigroesse:  " << std::dec << bytes.size() << " Bytes\n"
        << "Einstieg:      ";

    print_address(entry_address);

    std::cout
        << "\nFunktionen:    "
        << std::dec
        << functions.size()
        << "\n\n";

    for (const auto& function : functions) {
        std::cout
            << "Funktion "
            << std::dec
            << function.id
            << ": ";

        print_address(function.entry_address);
        std::cout << '\n';

        std::cout << "  Basic Blocks: ";

        if (function.block_addresses.empty()) {
            std::cout << "keine";
        } else {
            for (
                std::size_t index = 0;
                index < function.block_addresses.size();
                ++index
            ) {
                if (index != 0u) {
                    std::cout << ", ";
                }

                print_address(function.block_addresses[index]);
            }
        }

        std::cout << "\n  Direkte Aufrufe: ";

        if (function.direct_callees.empty()) {
            std::cout << "keine";
        } else {
            for (
                std::size_t index = 0;
                index < function.direct_callees.size();
                ++index
            ) {
                if (index != 0u) {
                    std::cout << ", ";
                }

                print_address(function.direct_callees[index]);
            }
        }

        std::cout << "\n  Indirekte Aufrufe: ";

        if (function.indirect_call_sites.empty()) {
            std::cout << "keine";
        } else {
            for (
                std::size_t index = 0;
                index < function.indirect_call_sites.size();
                ++index
            ) {
                if (index != 0u) {
                    std::cout << ", ";
                }

                print_address(function.indirect_call_sites[index]);
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
        << "  katana-recomp blocks <Datei> [Basisadresse]\n"
        << "  katana-recomp functions <Datei> <Einstieg> [Basisadresse]\n\n"
        << "Beispiele:\n"
        << "  katana-recomp E1FF\n"
        << "  katana-recomp blocks programm.bin 8C010000\n"
        << "  katana-recomp functions programm.bin 8C010000 8C010000\n";
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

        if (
            (argc == 4 || argc == 5) &&
            std::string(argv[1]) == "functions"
        ) {
            const auto entry_address =
                parse_hex_value(
                    argv[3],
                    std::numeric_limits<std::uint32_t>::max(),
                    "Die Einstiegsadresse"
                );

            const auto base_address =
                argc == 5
                    ? parse_hex_value(
                        argv[4],
                        std::numeric_limits<std::uint32_t>::max(),
                        "Die Basisadresse"
                    )
                    : 0u;

            return analyze_functions(
                std::filesystem::path(argv[2]),
                entry_address,
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
'@

[System.IO.File]::WriteAllText(
    (Join-Path $projekt "src\cli\main.cpp"),
    $main,
    [System.Text.Encoding]::ASCII
)

@'
cmake_minimum_required(VERSION 3.25)

project(
    KatanaRecomp
    VERSION 0.5.0
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
    src/analysis/function_analysis.cpp
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

add_executable(katana-function-analysis-tests
    tests/functions/function_analysis_tests.cpp
)

target_link_libraries(
    katana-function-analysis-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-function-analysis-tests)

add_test(
    NAME katana-function-analysis-tests
    COMMAND katana-function-analysis-tests
)
'@ | Set-Content ".\CMakeLists.txt" -Encoding ascii

Write-Host ""
Write-Host "Funktionsanalyse-Meilenstein wurde angewendet." -ForegroundColor Green
Write-Host "Sicherung: $backup"
Write-Host ""

Remove-Item ".\build" -Recurse -Force -ErrorAction SilentlyContinue

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure

Write-Host ""
Write-Host "Erzeuge Funktionsanalyse-Demodatei..."

[Environment]::CurrentDirectory = (Get-Location).Path
New-Item -ItemType Directory -Force ".\samples" | Out-Null

[System.IO.File]::WriteAllBytes(
    (Join-Path (Get-Location).Path "samples\functions_demo.bin"),
    [byte[]](
        0x02, 0xB0,
        0x09, 0x00,
        0x0B, 0x00,
        0x09, 0x00,
        0x0B, 0x43,
        0x09, 0x00,
        0x0B, 0x00,
        0x09, 0x00
    )
)

Write-Host ""
Write-Host "Funktionsanalyse:"
& ".\build\katana-recomp.exe" functions ".\samples\functions_demo.bin" 8C010000 8C010000

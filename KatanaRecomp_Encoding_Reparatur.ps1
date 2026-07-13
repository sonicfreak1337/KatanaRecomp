$ErrorActionPreference = "Stop"

$projekt = (Get-Location).Path
$mainDatei = Join-Path $projekt "src\cli\main.cpp"
$cmakeDatei = Join-Path $projekt "CMakeLists.txt"

if (-not (Test-Path $mainDatei)) {
    throw "src\cli\main.cpp wurde nicht gefunden. Starte das Skript im KatanaRecomp-Projektordner."
}

if (-not (Test-Path $cmakeDatei)) {
    throw "CMakeLists.txt wurde nicht gefunden. Starte das Skript im KatanaRecomp-Projektordner."
}

$zeit = Get-Date -Format "yyyyMMdd_HHmmss"
$backup = Join-Path $projekt ".katana_backup_encoding_$zeit"

New-Item -ItemType Directory -Force (Join-Path $backup "src\cli") | Out-Null
Copy-Item $mainDatei (Join-Path $backup "src\cli\main.cpp")
Copy-Item $cmakeDatei (Join-Path $backup "CMakeLists.txt")

$main = @'
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
'@

[System.IO.File]::WriteAllText(
    $mainDatei,
    $main,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host ""
Write-Host "CLI-Ausgabe wurde auf robuste ASCII-Texte umgestellt." -ForegroundColor Green
Write-Host "Sicherung: $backup"
Write-Host ""

cmake --build build
ctest --test-dir build --output-on-failure

Write-Host ""
Write-Host "Kontrollfluss-Demo:"
& ".\build\katana-recomp.exe" disasm ".\samples\control_flow_demo.bin" 8C010000

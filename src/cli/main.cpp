#include "katana/sh4/decoder.hpp"

#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::uint16_t parse_opcode(std::string text) {
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.erase(0, 2);
    }

    if (text.empty() || text.length() > 4) {
        throw std::invalid_argument("Der Opcode muss zwischen 1 und 4 Hex-Zeichen enthalten.");
    }

    std::size_t parsed_characters = 0;
    const auto value = std::stoul(text, &parsed_characters, 16);

    if (parsed_characters != text.length() || value > 0xFFFFu) {
        throw std::invalid_argument("Ungültiger 16-Bit-Opcode.");
    }

    return static_cast<std::uint16_t>(value);
}

}

int main(const int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr
            << "Verwendung:\n"
            << "  katana-recomp <SH-4-Opcode>\n\n"
            << "Beispiel:\n"
            << "  katana-recomp E1FF\n";

        return 2;
    }

    try {
        const auto opcode = parse_opcode(argv[1]);
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
            << '\n';

        return instruction.is_known() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Fehler: " << error.what() << '\n';
        return 2;
    }
}

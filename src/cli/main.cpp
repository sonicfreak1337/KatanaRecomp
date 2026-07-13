#include "katana/analysis/basic_blocks.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/io/binary_reader.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/disassembler.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
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

void print_ir_instruction(
    const katana::ir::Instruction& instruction
) {
    print_address(instruction.source_address);

    std::cout
        << "  "
        << katana::ir::operation_name(
            instruction.operation
        );

    switch (instruction.operation) {
        case katana::ir::Operation::MovImmediate:
        case katana::ir::Operation::AddImmediate:
        case katana::ir::Operation::AndImmediate:
        case katana::ir::Operation::OrImmediate:
        case katana::ir::Operation::XorImmediate:
        case katana::ir::Operation::CompareEqualImmediate:
        case katana::ir::Operation::TestImmediate:
            std::cout
                << " r"
                << std::dec
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << ", "
                << instruction.immediate;
            break;

        case katana::ir::Operation::MovRegister:
        case katana::ir::Operation::AddRegister:
        case katana::ir::Operation::SubRegister:
        case katana::ir::Operation::NegateRegister:
        case katana::ir::Operation::NotRegister:
        case katana::ir::Operation::AddWithCarry:
        case katana::ir::Operation::AddWithOverflow:
        case katana::ir::Operation::SubWithCarry:
        case katana::ir::Operation::SubWithOverflow:
        case katana::ir::Operation::NegateWithCarry:
        case katana::ir::Operation::ExtendUnsignedByte:
        case katana::ir::Operation::ExtendUnsignedWord:
        case katana::ir::Operation::ExtendSignedByte:
        case katana::ir::Operation::ExtendSignedWord:
        case katana::ir::Operation::SwapBytes:
        case katana::ir::Operation::SwapWords:
        case katana::ir::Operation::ExtractMiddle:
        case katana::ir::Operation::ShiftArithmeticDynamic:
        case katana::ir::Operation::ShiftLogicalDynamic:
        case katana::ir::Operation::MultiplyLong:
        case katana::ir::Operation::MultiplySignedWord:
        case katana::ir::Operation::MultiplyUnsignedWord:
        case katana::ir::Operation::DoubleMultiplySignedLong:
        case katana::ir::Operation::DoubleMultiplyUnsignedLong:
        case katana::ir::Operation::AndRegister:
        case katana::ir::Operation::OrRegister:
        case katana::ir::Operation::XorRegister:
        case katana::ir::Operation::CompareEqualRegister:
        case katana::ir::Operation::CompareHigherOrSame:
        case katana::ir::Operation::CompareGreaterOrEqual:
        case katana::ir::Operation::CompareHigher:
        case katana::ir::Operation::CompareGreaterThan:
        case katana::ir::Operation::CompareString:
        case katana::ir::Operation::TestRegister:
            std::cout
                << " r"
                << std::dec
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << ", r"
                << static_cast<unsigned>(
                    instruction.source_register
                );
            break;

        case katana::ir::Operation::ComparePositiveOrZero:
        case katana::ir::Operation::ComparePositive:
            std::cout
                << " r"
                << std::dec
                << static_cast<unsigned>(
                    instruction.destination_register
                );
            break;
        case katana::ir::Operation::DecrementAndTest:
        case katana::ir::Operation::MoveT:
        case katana::ir::Operation::ShiftLogicalLeftOne:
        case katana::ir::Operation::ShiftLogicalRightOne:
        case katana::ir::Operation::ShiftArithmeticLeftOne:
        case katana::ir::Operation::ShiftArithmeticRightOne:
        case katana::ir::Operation::ShiftLogicalLeftTwo:
        case katana::ir::Operation::ShiftLogicalLeftEight:
        case katana::ir::Operation::ShiftLogicalLeftSixteen:
        case katana::ir::Operation::ShiftLogicalRightTwo:
        case katana::ir::Operation::ShiftLogicalRightEight:
        case katana::ir::Operation::ShiftLogicalRightSixteen:
        case katana::ir::Operation::RotateLeft:
        case katana::ir::Operation::RotateRight:
        case katana::ir::Operation::RotateLeftThroughT:
        case katana::ir::Operation::RotateRightThroughT:
            std::cout
                << " r"
                << std::dec
                << static_cast<unsigned>(
                    instruction.destination_register
                );
            break;
        case katana::ir::Operation::LoadByteSigned:
        case katana::ir::Operation::LoadWordSigned:
        case katana::ir::Operation::LoadLong:
            std::cout
                << " r"
                << std::dec
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << ", [r"
                << static_cast<unsigned>(
                    instruction.source_register
                )
                << "]";
            break;

        case katana::ir::Operation::StoreByte:
        case katana::ir::Operation::StoreWord:
        case katana::ir::Operation::StoreLong:
            std::cout
                << " [r"
                << std::dec
                << static_cast<unsigned>(
                    instruction.destination_register
                )
                << "], r"
                << static_cast<unsigned>(
                    instruction.source_register
                );
            break;
        case katana::ir::Operation::Branch:
        case katana::ir::Operation::Call:
        case katana::ir::Operation::BranchIfTrue:
        case katana::ir::Operation::BranchIfFalse:
            if (instruction.target_address.has_value()) {
                std::cout << " ";
                print_address(*instruction.target_address);
            }
            break;

        case katana::ir::Operation::JumpRegister:
        case katana::ir::Operation::CallRegister:
            std::cout
                << " r"
                << std::dec
                << static_cast<unsigned>(
                    instruction.branch_register
                );
            break;

        case katana::ir::Operation::Unknown:
        case katana::ir::Operation::Nop:
        case katana::ir::Operation::ClearT:
        case katana::ir::Operation::SetT:
        case katana::ir::Operation::Return:
            break;
    }

    if (instruction.has_delay_slot) {
        std::cout << " [delayed]";
    }

    if (instruction.is_delay_slot) {
        std::cout << " [delay-slot]";
    }

    std::cout << '\n';
}

std::vector<katana::ir::Function> build_ir_program(
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

    const auto discovered =
        katana::analysis::discover_functions(
            lines,
            seeds
        );

    return katana::ir::lower_program(
        lines,
        discovered
    );
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

int analyze_ir(
    const std::filesystem::path& path,
    const std::uint32_t entry_address,
    const std::uint32_t base_address
) {
    const auto program = build_ir_program(
        path,
        entry_address,
        base_address
    );

    std::cout
        << "Datei:         " << path.string() << '\n'
        << "IR-Funktionen: "
        << std::dec
        << program.size()
        << "\n\n";

    for (const auto& function : program) {
        std::cout << "Funktion ";
        print_address(function.entry_address);
        std::cout << '\n';

        for (const auto& block : function.blocks) {
            std::cout << "  Block ";
            print_address(block.start_address);
            std::cout << '\n';

            for (const auto& instruction : block.instructions) {
                std::cout << "    ";
                print_ir_instruction(instruction);
            }

            std::cout << "    Nachfolger: ";

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
    }

    return 0;
}

int emit_cpp(
    const std::filesystem::path& input_path,
    const std::uint32_t entry_address,
    const std::filesystem::path& output_path,
    const std::uint32_t base_address
) {
    const auto program = build_ir_program(
        input_path,
        entry_address,
        base_address
    );

    const auto source =
        katana::codegen::emit_cpp_program(
            program,
            entry_address
        );

    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(
            output_path.parent_path()
        );
    }

    std::ofstream output(
        output_path,
        std::ios::binary
    );

    if (!output) {
        throw std::runtime_error(
            "Die Ausgabedatei konnte nicht geoeffnet werden."
        );
    }

    output.write(
        source.data(),
        static_cast<std::streamsize>(source.size())
    );

    if (!output) {
        throw std::runtime_error(
            "Der generierte C++-Code konnte nicht gespeichert werden."
        );
    }

    std::cout
        << "C++-Code erzeugt: "
        << output_path.string()
        << '\n'
        << "Funktionen:       "
        << std::dec
        << program.size()
        << '\n'
        << "Zeichen:          "
        << source.size()
        << '\n';

    return 0;
}

void print_usage() {
    std::cerr
        << "Verwendung:\n"
        << "  katana-recomp <Opcode>\n"
        << "  katana-recomp opcode <Opcode>\n"
        << "  katana-recomp disasm <Datei> [Basisadresse]\n"
        << "  katana-recomp blocks <Datei> [Basisadresse]\n"
        << "  katana-recomp functions <Datei> <Einstieg> [Basisadresse]\n"
        << "  katana-recomp ir <Datei> <Einstieg> [Basisadresse]\n"
        << "  katana-recomp emit-cpp <Datei> <Einstieg> <Ausgabe.cpp> [Basisadresse]\n\n"
        << "Beispiel:\n"
        << "  katana-recomp emit-cpp programm.bin 8C010000 generated.cpp 8C010000\n";
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

        if (
            (argc == 4 || argc == 5) &&
            std::string(argv[1]) == "ir"
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

            return analyze_ir(
                std::filesystem::path(argv[2]),
                entry_address,
                base_address
            );
        }

        if (
            (argc == 5 || argc == 6) &&
            std::string(argv[1]) == "emit-cpp"
        ) {
            const auto entry_address =
                parse_hex_value(
                    argv[3],
                    std::numeric_limits<std::uint32_t>::max(),
                    "Die Einstiegsadresse"
                );

            const auto base_address =
                argc == 6
                    ? parse_hex_value(
                        argv[5],
                        std::numeric_limits<std::uint32_t>::max(),
                        "Die Basisadresse"
                    )
                    : 0u;

            return emit_cpp(
                std::filesystem::path(argv[2]),
                entry_address,
                std::filesystem::path(argv[4]),
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
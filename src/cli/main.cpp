#include "katana/analysis/analysis_overrides.hpp"
#include "katana/analysis/basic_blocks.hpp"
#include "katana/analysis/control_flow_analysis.hpp"
#include "katana/analysis/control_flow_report.hpp"
#include "katana/analysis/function_analysis.hpp"
#include "katana/analysis/graph_export.hpp"
#include "katana/analysis/recursive_analysis.hpp"
#include "katana/app/application.hpp"
#include "katana/cli/exit_code.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/codegen/port_export.hpp"
#include "katana/codegen/probe.hpp"
#include "katana/io/elf32_sh_loader.hpp"
#include "katana/io/input_output_error.hpp"
#include "katana/io/input_provenance.hpp"
#include "katana/io/project_manifest.hpp"
#include "katana/io/raw_binary_loader.hpp"
#include "katana/ir/lower.hpp"
#include "katana/ir/optimize.hpp"
#include "katana/ir/serialize.hpp"
#include "katana/platform/dreamcast_disc.hpp"
#include "katana/platform/firmware_diagnostics.hpp"
#include "katana/runtime/packed_disc.hpp"
#include "katana/sh4/decoder.hpp"
#include "katana/sh4/disassembler.hpp"
#include "katana/sh4/isa_coverage.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#undef CompareString
#endif

namespace {

std::uint32_t
parse_hex_value(std::string text, const std::uint32_t maximum, const std::string& description) {
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.erase(0, 2);
    }

    if (text.empty()) {
        throw std::invalid_argument(description + " darf nicht leer sein.");
    }

    const auto is_valid_hex =
        std::all_of(text.begin(), text.end(), [](const unsigned char character) {
            return std::isxdigit(character) != 0;
        });

    if (!is_valid_hex) {
        throw std::invalid_argument(description + " enthaelt ungueltige Hex-Zeichen.");
    }

    std::size_t parsed_characters = 0;
    const auto value = std::stoull(text, &parsed_characters, 16);

    if (parsed_characters != text.length() || value > maximum) {
        throw std::invalid_argument(description + " liegt ausserhalb des erlaubten Bereichs.");
    }

    return static_cast<std::uint32_t>(value);
}

std::string format_disassembly_text(const katana::sh4::DisassemblyLine& line) {
    std::ostringstream output;
    output << line.instruction.text;

    if (line.target_address.has_value()) {
        output << " 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
               << *line.target_address;
    }

    if (line.is_delay_slot) {
        output << "  ; delay slot";
    }

    return output.str();
}

void print_address(const std::uint32_t address) {
    std::cout << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << address;
}

std::string attach_execution_profile_json(std::string document,
                                          const katana::io::ProjectManifest& profile) {
    const auto object = document.find('{');
    if (object == std::string::npos) {
        throw std::runtime_error("JSON-Bericht besitzt kein Wurzelobjekt.");
    }
    document.insert(object + 1u,
                    "\"execution_profile\":" +
                        katana::io::format_project_execution_profile_json(profile) + ",");
    return document;
}

void require_cpp_profile_capabilities(const katana::io::ProjectManifest& profile) {
    try {
        katana::app::require_cpp_profile_capabilities(profile);
    } catch (const std::exception& error) {
        throw katana::cli::Error(katana::cli::ExitCode::CodeGenerationFailure, error.what());
    }
}

std::string_view special_register_name(const katana::ir::SpecialRegister special_register) {
    using Register = katana::ir::SpecialRegister;
    switch (special_register) {
    case Register::None:
        return "none";
    case Register::Mach:
        return "mach";
    case Register::Macl:
        return "macl";
    case Register::Pr:
        return "pr";
    case Register::Fpul:
        return "fpul";
    case Register::Fpscr:
        return "fpscr";
    case Register::Sr:
        return "sr";
    case Register::Gbr:
        return "gbr";
    case Register::Vbr:
        return "vbr";
    case Register::Ssr:
        return "ssr";
    case Register::Spc:
        return "spc";
    case Register::Sgr:
        return "sgr";
    case Register::Dbr:
        return "dbr";
    case Register::Bank0:
        return "r0_bank";
    case Register::Bank1:
        return "r1_bank";
    case Register::Bank2:
        return "r2_bank";
    case Register::Bank3:
        return "r3_bank";
    case Register::Bank4:
        return "r4_bank";
    case Register::Bank5:
        return "r5_bank";
    case Register::Bank6:
        return "r6_bank";
    case Register::Bank7:
        return "r7_bank";
    }
    return "none";
}

void print_ir_instruction(const katana::ir::Instruction& instruction) {
    print_address(instruction.source_address);

    std::cout << "  " << katana::ir::operation_name(instruction.operation);

    switch (instruction.operation) {
    case katana::ir::Operation::MovImmediate:
    case katana::ir::Operation::AddImmediate:
    case katana::ir::Operation::AndImmediate:
    case katana::ir::Operation::OrImmediate:
    case katana::ir::Operation::XorImmediate:
    case katana::ir::Operation::CompareEqualImmediate:
    case katana::ir::Operation::TestImmediate:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", " << instruction.immediate;
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
    case katana::ir::Operation::MultiplyAccumulateWord:
    case katana::ir::Operation::MultiplyAccumulateLong:
    case katana::ir::Operation::DivideInitializeSigned:
    case katana::ir::Operation::DivideStep:
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
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", r" << static_cast<unsigned>(instruction.source_register);
        break;

    case katana::ir::Operation::ComparePositiveOrZero:
    case katana::ir::Operation::ComparePositive:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register);
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
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register);
        break;
    case katana::ir::Operation::LoadByteSignedPostIncrement:
    case katana::ir::Operation::LoadWordSignedPostIncrement:
    case katana::ir::Operation::LoadLongPostIncrement:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", [r" << static_cast<unsigned>(instruction.source_register) << "+]";
        break;

    case katana::ir::Operation::LoadByteSignedDisplacement:
    case katana::ir::Operation::LoadWordSignedDisplacement:
    case katana::ir::Operation::LoadLongDisplacement:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", [r" << static_cast<unsigned>(instruction.source_register) << " + "
                  << instruction.displacement << "]";
        break;

    case katana::ir::Operation::LoadByteSignedR0Indexed:
    case katana::ir::Operation::LoadWordSignedR0Indexed:
    case katana::ir::Operation::LoadLongR0Indexed:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", [r0 + r" << static_cast<unsigned>(instruction.source_register) << "]";
        break;

    case katana::ir::Operation::LoadByteSignedGbrDisplacement:
    case katana::ir::Operation::LoadWordSignedGbrDisplacement:
    case katana::ir::Operation::LoadLongGbrDisplacement:
        std::cout << " r0, [gbr + " << std::dec << instruction.displacement << "]";
        break;

    case katana::ir::Operation::LoadWordSignedPcRelative:
    case katana::ir::Operation::LoadLongPcRelative:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", [";
        if (instruction.effective_address.has_value()) {
            print_address(*instruction.effective_address);
        }
        std::cout << "]";
        break;

    case katana::ir::Operation::MoveAddressPcRelative:
        std::cout << " r0, ";
        if (instruction.effective_address.has_value()) {
            print_address(*instruction.effective_address);
        }
        break;

    case katana::ir::Operation::StoreSpecialRegister:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", " << special_register_name(instruction.special_register);
        break;

    case katana::ir::Operation::StoreSpecialRegisterPreDecrement:
        std::cout << " [--r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << "], " << special_register_name(instruction.special_register);
        break;

    case katana::ir::Operation::LoadSpecialRegister:
        std::cout << " " << special_register_name(instruction.special_register) << ", r" << std::dec
                  << static_cast<unsigned>(instruction.source_register);
        break;

    case katana::ir::Operation::LoadSpecialRegisterPostIncrement:
        std::cout << " " << special_register_name(instruction.special_register) << ", [r"
                  << std::dec << static_cast<unsigned>(instruction.source_register) << "+]";
        break;

    case katana::ir::Operation::LoadByteSigned:
    case katana::ir::Operation::LoadWordSigned:
    case katana::ir::Operation::LoadLong:
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << ", [r" << static_cast<unsigned>(instruction.source_register) << "]";
        break;

    case katana::ir::Operation::StoreBytePreDecrement:
    case katana::ir::Operation::StoreWordPreDecrement:
    case katana::ir::Operation::StoreLongPreDecrement:
        std::cout << " [--r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << "], r" << static_cast<unsigned>(instruction.source_register);
        break;

    case katana::ir::Operation::StoreByteDisplacement:
    case katana::ir::Operation::StoreWordDisplacement:
    case katana::ir::Operation::StoreLongDisplacement:
        std::cout << " [r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << " + " << instruction.displacement << "], r"
                  << static_cast<unsigned>(instruction.source_register);
        break;

    case katana::ir::Operation::StoreByteR0Indexed:
    case katana::ir::Operation::StoreWordR0Indexed:
    case katana::ir::Operation::StoreLongR0Indexed:
        std::cout << " [r0 + r" << std::dec
                  << static_cast<unsigned>(instruction.destination_register) << "], r"
                  << static_cast<unsigned>(instruction.source_register);
        break;

    case katana::ir::Operation::StoreByteGbrDisplacement:
    case katana::ir::Operation::StoreWordGbrDisplacement:
    case katana::ir::Operation::StoreLongGbrDisplacement:
        std::cout << " [gbr + " << std::dec << instruction.displacement << "], r0";
        break;

    case katana::ir::Operation::StoreByte:
    case katana::ir::Operation::StoreWord:
    case katana::ir::Operation::StoreLong:
        std::cout << " [r" << std::dec << static_cast<unsigned>(instruction.destination_register)
                  << "], r" << static_cast<unsigned>(instruction.source_register);
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
        std::cout << " r" << std::dec << static_cast<unsigned>(instruction.branch_register);
        break;

    case katana::ir::Operation::TrapAlways:
        std::cout << " #" << std::dec << instruction.immediate;
        break;

    case katana::ir::Operation::Unknown:
    case katana::ir::Operation::Nop:
    case katana::ir::Operation::DivideInitializeUnsigned:
    case katana::ir::Operation::ClearS:
    case katana::ir::Operation::SetS:
    case katana::ir::Operation::ClearT:
    case katana::ir::Operation::SetT:
    case katana::ir::Operation::Return:
    case katana::ir::Operation::ReturnFromException:
    case katana::ir::Operation::Sleep:
        break;
    }

    if (instruction.delay_slot.role == katana::ir::DelaySlotRole::Owner) {
        std::cout << " [delayed]";
    }

    if (instruction.delay_slot.role == katana::ir::DelaySlotRole::Slot) {
        std::cout << " [delay-slot]";
    }

    if (instruction.is_privileged) {
        std::cout << " [privileged]";
    }

    std::cout << '\n';
}

std::vector<katana::ir::Function>
build_ir_program(const std::filesystem::path& path,
                 const std::uint32_t entry_address,
                 const std::uint32_t base_address,
                 const std::optional<std::filesystem::path>& override_path = std::nullopt,
                 std::optional<katana::io::ProjectManifest>* execution_profile = nullptr) {
    auto extension = path.extension().string();
    std::transform(
        extension.begin(), extension.end(), extension.begin(), [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });

    katana::io::ExecutableImage image;
    if (extension == ".katana" || extension == ".manifest") {
        auto project = katana::io::load_project(path);
        if (execution_profile != nullptr) {
            *execution_profile = project.execution_profile;
        }
        image = std::move(project.image);
    } else {
        std::ifstream input(path, std::ios::binary);
        std::array<unsigned char, 4> magic{};
        input.read(reinterpret_cast<char*>(magic.data()),
                   static_cast<std::streamsize>(magic.size()));
        const bool is_elf = input.gcount() == static_cast<std::streamsize>(magic.size()) &&
                            magic[0] == 0x7Fu && magic[1] == 'E' && magic[2] == 'L' &&
                            magic[3] == 'F';
        if (is_elf) {
            image = katana::io::load_elf32_sh(path);
        } else {
            katana::io::RawBinaryLoadOptions options;
            options.base_address = base_address;
            options.entry_point = entry_address;
            image = katana::io::load_raw_binary(path, options);
        }
    }
    image.add_entry_point(entry_address);

    std::optional<katana::analysis::AnalysisOverrides> overrides;
    if (override_path) {
        overrides = katana::analysis::parse_analysis_overrides(*override_path);
    }
    const auto analysis =
        katana::analysis::analyze_control_flow(image, overrides ? &*overrides : nullptr);
    return katana::ir::lower_program(analysis);
}

int decode_single_opcode(const std::string& text) {
    const auto opcode = static_cast<std::uint16_t>(parse_hex_value(text, 0xFFFFu, "Der Opcode"));

    const auto instruction = katana::sh4::decode(opcode);

    std::cout << "Opcode:        0x" << std::hex << std::uppercase << std::setw(4)
              << std::setfill('0') << opcode << '\n'
              << "Instruktion:   " << instruction.text << '\n'
              << "Status:        " << (instruction.is_known() ? "erkannt" : "unbekannt") << '\n'
              << "Kontrollfluss: " << (instruction.changes_control_flow() ? "ja" : "nein") << '\n'
              << "Delay Slot:    " << (instruction.has_delay_slot ? "ja" : "nein") << '\n';

    return instruction.is_known() ? 0 : 1;
}

int analyze_manifest(const std::filesystem::path& path,
                     const std::optional<std::filesystem::path>& override_path = std::nullopt,
                     const bool json = false) {
    const auto project = katana::io::load_project(path);
    std::optional<katana::analysis::AnalysisOverrides> overrides;
    if (override_path.has_value()) {
        overrides = katana::analysis::parse_analysis_overrides(*override_path);
    }
    const auto analysis = katana::analysis::analyze_control_flow(
        project.image, overrides.has_value() ? &*overrides : nullptr);
    if (json) {
        std::cout << attach_execution_profile_json(
            katana::analysis::format_control_flow_analysis_json(analysis),
            project.execution_profile);
    } else {
        std::cout << katana::io::format_project_execution_profile_text(project.execution_profile)
                  << '\n';
        std::cout << katana::analysis::format_recursive_analysis_report(
            analysis.recursive, analysis.symbolic_addresses);
        std::cout << katana::analysis::format_indirect_control_flow_report(
            analysis.indirect_control_flow, analysis.jump_tables, analysis.symbolic_addresses);
    }
    return 0;
}

int export_analysis_graph(const std::filesystem::path& path,
                          const std::optional<std::filesystem::path>& override_path,
                          const std::string_view command) {
    const auto project = katana::io::load_project(path);
    std::optional<katana::analysis::AnalysisOverrides> overrides;
    if (override_path.has_value()) {
        overrides = katana::analysis::parse_analysis_overrides(*override_path);
    }
    const auto analysis = katana::analysis::analyze_control_flow(
        project.image, overrides.has_value() ? &*overrides : nullptr);
    const bool call_graph = command.starts_with("callgraph-");
    const auto graph = call_graph ? katana::analysis::build_call_graph(analysis)
                                  : katana::analysis::build_control_flow_graph(analysis);
    if (command.ends_with("-json")) {
        std::cout << attach_execution_profile_json(
            katana::analysis::serialize_analysis_graph_json(graph), project.execution_profile);
    } else {
        std::cout << "// "
                  << katana::io::format_project_execution_profile_text(project.execution_profile)
                  << '\n'
                  << katana::analysis::serialize_analysis_graph_dot(graph);
    }
    return 0;
}

int diagnose_firmware(const std::filesystem::path& path,
                      const katana::platform::FirmwareImageKind kind,
                      const katana::platform::FirmwareDiagnosticOptions& options) {
    const auto report = katana::platform::inspect_firmware_file(path, kind, options);
    std::cout << katana::platform::format_firmware_diagnostic_json(report);
    return katana::cli::exit_status(report.valid() ? katana::cli::ExitCode::Success
                                                   : katana::cli::ExitCode::ProcessingFailure);
}

int disassemble_file(const std::filesystem::path& path, const std::uint32_t base_address) {
    katana::io::RawBinaryLoadOptions options;
    options.base_address = base_address;
    const auto image = katana::io::load_raw_binary(path, options);
    const auto lines = katana::sh4::disassemble(image);

    std::size_t unknown_count = 0;
    std::size_t control_flow_count = 0;
    std::size_t delay_slot_count = 0;

    std::cout << "Datei:         " << path.string() << '\n'
              << "Dateigroesse:  " << std::dec << image.segments()[0].bytes.size() << " Bytes\n"
              << "Basisadresse:  0x" << std::hex << std::uppercase << std::setw(8)
              << std::setfill('0') << base_address << "\n\n";

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

        std::cout << "  " << std::setw(4) << line.opcode << "  " << format_disassembly_text(line)
                  << '\n';
    }

    std::cout << "\nInstruktionen:         " << std::dec << lines.size()
              << "\nKontrollfluss:         " << control_flow_count
              << "\nMarkierte Delay Slots: " << delay_slot_count
              << "\nUnbekannte Opcodes:    " << unknown_count << '\n';

    return 0;
}

int analyze_blocks(const std::filesystem::path& path, const std::uint32_t base_address) {
    katana::io::RawBinaryLoadOptions options;
    options.base_address = base_address;
    options.entry_point = base_address;
    const auto image = katana::io::load_raw_binary(path, options);
    const auto lines = katana::analysis::analyze_reachable_code(image).instructions;
    const auto blocks = katana::analysis::build_basic_blocks(lines);

    std::cout << "Datei:         " << path.string() << '\n'
              << "Dateigroesse:  " << std::dec << image.segments()[0].bytes.size() << " Bytes\n"
              << "Basic Blocks:  " << blocks.size() << "\n\n";

    for (const auto& block : blocks) {
        std::cout << "Block " << std::dec << block.id << ": ";

        print_address(block.start_address);
        std::cout << " - ";
        print_address(block.end_address);
        std::cout << '\n';

        for (const auto& line : block.lines) {
            std::cout << "  ";
            print_address(line.address);

            std::cout << "  " << std::setw(4) << line.opcode << "  "
                      << format_disassembly_text(line) << '\n';
        }

        std::cout << "  Nachfolger: ";

        if (block.successors.empty() && !block.has_indirect_successor) {
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

int analyze_functions(const std::filesystem::path& path,
                      const std::uint32_t entry_address,
                      const std::uint32_t base_address) {
    katana::io::RawBinaryLoadOptions options;
    options.base_address = base_address;
    options.entry_point = entry_address;
    const auto image = katana::io::load_raw_binary(path, options);
    const auto lines = katana::analysis::analyze_reachable_code(image).instructions;

    const std::array<std::uint32_t, 1> seeds = {entry_address};

    const auto functions = katana::analysis::discover_functions(lines, seeds);

    std::cout << "Datei:         " << path.string() << '\n'
              << "Dateigroesse:  " << std::dec << image.segments()[0].bytes.size() << " Bytes\n"
              << "Einstieg:      ";

    print_address(entry_address);

    std::cout << "\nFunktionen:    " << std::dec << functions.size() << "\n\n";

    for (const auto& function : functions) {
        std::cout << "Funktion " << std::dec << function.id << ": ";

        print_address(function.entry_address);
        std::cout << '\n';

        std::cout << "  Basic Blocks: ";

        if (function.block_addresses.empty()) {
            std::cout << "keine";
        } else {
            for (std::size_t index = 0; index < function.block_addresses.size(); ++index) {
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
            for (std::size_t index = 0; index < function.direct_callees.size(); ++index) {
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
            for (std::size_t index = 0; index < function.indirect_call_sites.size(); ++index) {
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

int analyze_ir(const std::filesystem::path& path,
               const std::uint32_t entry_address,
               const std::uint32_t base_address,
               const bool json,
               const std::optional<std::filesystem::path>& override_path) {
    std::optional<katana::io::ProjectManifest> execution_profile;
    const auto program =
        build_ir_program(path, entry_address, base_address, override_path, &execution_profile);

    if (json) {
        auto report = katana::ir::emit_ir_json(program);
        std::cout << (execution_profile
                          ? attach_execution_profile_json(std::move(report), *execution_profile)
                          : report);
    } else {
        std::cout << katana::ir::emit_ir_text(program);
        if (execution_profile) {
            std::cout << katana::io::format_project_execution_profile_text(*execution_profile)
                      << '\n';
        }
    }

    return 0;
}

int emit_cpp(const std::filesystem::path& input_path,
             const std::uint32_t entry_address,
             const std::filesystem::path& output_path,
             const std::uint32_t base_address,
             katana::ir::OptimizationOptions optimization_options,
             const std::optional<std::filesystem::path>& dump_prefix,
             const std::optional<std::filesystem::path>& override_path) {
    std::optional<katana::io::ProjectManifest> execution_profile;
    auto program = build_ir_program(
        input_path, entry_address, base_address, override_path, &execution_profile);
    if (execution_profile) require_cpp_profile_capabilities(*execution_profile);

    const auto before_optimization =
        dump_prefix ? katana::ir::emit_ir_text(program) : std::string{};
    optimization_options.capture_dumps = dump_prefix.has_value();
    const auto optimization_report = katana::ir::optimize_program(program, optimization_options);

    if (dump_prefix) {
        const auto write_dump = [](const std::filesystem::path& path, const std::string& contents) {
            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path());
            }
            std::ofstream output(path, std::ios::binary);
            if (!output) {
                throw katana::io::InputOutputError("Der IR-Dump konnte nicht geoeffnet werden.");
            }
            output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (!output) {
                throw katana::io::InputOutputError("Der IR-Dump konnte nicht gespeichert werden.");
            }
        };
        write_dump(dump_prefix->string() + ".before.ir", before_optimization);
        write_dump(dump_prefix->string() + ".after.ir", katana::ir::emit_ir_text(program));
    }

    const auto source = katana::codegen::emit_cpp_program(program, entry_address);

    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    std::ofstream output(output_path, std::ios::binary);

    if (!output) {
        throw katana::io::InputOutputError("Die Ausgabedatei konnte nicht geoeffnet werden.");
    }

    output.write(source.data(), static_cast<std::streamsize>(source.size()));

    if (!output) {
        throw katana::io::InputOutputError(
            "Der generierte C++-Code konnte nicht gespeichert werden.");
    }

    std::cout << "C++-Code erzeugt: " << output_path.string() << '\n'
              << "Funktionen:       " << std::dec << program.size() << '\n'
              << "Zeichen:          " << source.size() << '\n'
              << "Optimierungen:    " << optimization_report.total_changes << '\n';

    return 0;
}

int emit_phase6_probe_source(const std::filesystem::path& gdi_path,
                             const std::filesystem::path& output_path) {
    const auto disc = katana::platform::load_dreamcast_gdi_boot(gdi_path);
    const auto image = katana::platform::make_dreamcast_disc_executable(disc);
    const auto analysis = katana::analysis::analyze_control_flow(image);
    if (!analysis.recursive.diagnostics.empty()) {
        throw std::runtime_error("Der Phase-6-Bootblock enthaelt eine unbekannte Instruktion.");
    }
    const auto program = katana::ir::lower_program(analysis);
    const auto function =
        std::find_if(program.begin(), program.end(), [](const katana::ir::Function& value) {
            return value.entry_address == katana::platform::dreamcast_disc_boot_address;
        });
    if (function == program.end()) {
        throw std::runtime_error("Der Phase-6-Programmeinstieg wurde nicht analysiert.");
    }
    const auto block = std::find_if(
        function->blocks.begin(), function->blocks.end(), [](const katana::ir::BasicBlock& value) {
            return value.start_address == katana::platform::dreamcast_disc_boot_address;
        });
    if (block == function->blocks.end() || block->instructions.empty()) {
        throw std::runtime_error("Der Phase-6-Einstiegsblock ist leer oder fehlt.");
    }
    if (katana::codegen::block_requires_call_dispatch(*block)) {
        throw std::runtime_error(
            "Der Phase-6-Einstiegsblock braucht fuer diese Probe bereits einen Call-Dispatch.");
    }

    katana::ir::Function probe;
    probe.entry_address = function->entry_address;
    probe.blocks.push_back(*block);
    probe.blocks.front().successors.clear();
    const std::array<katana::ir::Function, 1u> probe_program = {std::move(probe)};
    auto source = katana::codegen::emit_cpp_program(probe_program,
                                                    katana::platform::dreamcast_disc_boot_address);
    source += R"cpp(

#include "katana/platform/phase6_gate.hpp"
#include <exception>
#include <filesystem>
#include <iostream>

int main(const int argc, const char* const* argv) {
    if (argc != 3) {
        std::cerr << "Phase-6-Probe erwartet GDI-Quelle und Berichtsausgabe.\n";
        return 2;
    }
    try {
        const auto report = katana::platform::run_phase6_gate(
            std::filesystem::path(argv[1]),
            katana_generated::run,
)cpp";
    source += std::to_string(block->instructions.size());
    source += R"cpp(u
        );
        katana::platform::write_phase6_gate_report(report, std::filesystem::path(argv[2]));
        std::cout << report.checkpoint << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Phase-6-Gate fehlgeschlagen: " << error.what() << '\n';
        return 1;
    }
}
)cpp";

    if (output_path.has_parent_path()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw katana::io::InputOutputError(
            "Die lokale Phase-6-Probe konnte nicht geoeffnet werden.");
    }
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    if (!output) {
        throw katana::io::InputOutputError(
            "Die lokale Phase-6-Probe konnte nicht geschrieben werden.");
    }
    std::cout << "Lokale, temporaere Phase-6-Blockprobe erzeugt.\n";
    return 0;
}

std::filesystem::path discover_source_root_for_protection() {
    std::vector<std::filesystem::path> starts{std::filesystem::current_path()};
#ifdef _WIN32
    std::wstring executable(32'768u, L'\0');
    const auto length =
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length != 0u && length < executable.size()) {
        executable.resize(length);
        starts.push_back(std::filesystem::path(executable).parent_path());
    }
#else
    std::error_code link_error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", link_error);
    if (!link_error) starts.push_back(executable.parent_path());
#endif
    for (auto start : starts) {
        for (;;) {
            if (std::filesystem::exists(start / ".git") &&
                std::filesystem::exists(start / "include" / "katana") &&
                std::filesystem::exists(start / "CMakeLists.txt"))
                return std::filesystem::canonical(start);
            const auto parent = start.parent_path();
            if (parent.empty() || parent == start) break;
            start = parent;
        }
    }
    return {};
}

std::filesystem::path discover_runtime_root_for_build(const std::filesystem::path& source_root) {
#ifdef _WIN32
    char* configured_value = nullptr;
    std::size_t configured_size = 0u;
    const auto configured_result =
        _dupenv_s(&configured_value, &configured_size, "KATANA_RUNTIME_ROOT");
    const auto configured = configured_result == 0 && configured_value != nullptr
                                ? std::filesystem::path(configured_value)
                                : std::filesystem::path{};
    std::free(configured_value);
    if (!configured.empty()) {
#else
    if (const auto* configured = std::getenv("KATANA_RUNTIME_ROOT");
        configured != nullptr && *configured != '\0') {
#endif
        const auto root = std::filesystem::absolute(configured).lexically_normal();
        if (std::filesystem::exists(root / "CMakeLists.txt") &&
            std::filesystem::exists(root / "include" / "katana" / "runtime"))
            return root;
        throw std::invalid_argument("KATANA_RUNTIME_ROOT bezeichnet kein kompatibles Runtime-SDK.");
    }
    if (!source_root.empty()) return source_root;

    std::filesystem::path executable;
#ifdef _WIN32
    std::wstring buffer(32'768u, L'\0');
    const auto length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length != 0u && length < buffer.size()) {
        buffer.resize(length);
        executable = std::filesystem::path(buffer);
    }
#else
    std::error_code link_error;
    executable = std::filesystem::read_symlink("/proc/self/exe", link_error);
#endif
    if (!executable.empty()) {
        const auto packaged_sdk = executable.parent_path() / "runtime-sdk";
        if (std::filesystem::exists(packaged_sdk / "CMakeLists.txt") &&
            std::filesystem::exists(packaged_sdk / "include" / "katana" / "runtime"))
            return packaged_sdk;
    }
    throw std::runtime_error(
        "Runtime-SDK fuer Portbuild fehlt; KATANA_RUNTIME_ROOT kann es explizit angeben.");
}

int export_port_project(const std::filesystem::path& gdi_path,
                        const std::filesystem::path& output_path,
                        const std::string& target_name,
                        const bool diagnostic_partial = false) {
    const auto source_root = discover_source_root_for_protection();
    const auto runtime_root = discover_runtime_root_for_build(source_root);
    const auto absolute_output = std::filesystem::absolute(output_path).lexically_normal();
    if (!source_root.empty()) {
        const auto relative_to_source = absolute_output.lexically_relative(source_root);
        if (!relative_to_source.empty() && !relative_to_source.is_absolute() &&
            *relative_to_source.begin() != "..") {
            throw std::invalid_argument(
                "Port-Ausgabe muss ausserhalb des KatanaRecomp-Quellbaums liegen.");
        }
    }
    const auto shell_quote = [](const std::filesystem::path& path) {
        const auto text = path.string();
#ifdef _WIN32
        if (text.find('"') != std::string::npos) {
            throw std::invalid_argument("Hostbuildpfad enthaelt ein Anfuehrungszeichen.");
        }
        return '"' + text + '"';
#else
        std::string quoted = "'";
        for (const auto character : text) {
            character == '\'' ? quoted += "'\\''" : quoted += character;
        }
        return quoted + "'";
#endif
    };
    const auto stage_key =
        katana::io::sha256_bytes(absolute_output.generic_string() + ':' + target_name);
    const auto stage =
        absolute_output.parent_path() / (".katana-port-stage-" + stage_key.substr(0u, 12u));
    std::error_code cleanup_error;
    std::filesystem::remove_all(stage, cleanup_error);
    if (cleanup_error) throw std::runtime_error("Altes Port-Staging konnte nicht entfernt werden.");
    try {
        const auto report = katana::codegen::export_dreamcast_port_project(
            gdi_path,
            stage,
            {target_name, KATANA_RECOMP_VERSION, {}, source_root, diagnostic_partial});
        const auto build_path = report.output_root / "build";
        auto configure = std::string("cmake -S ") + shell_quote(report.output_root) + " -B " +
                         shell_quote(build_path);
#ifdef _WIN32
        configure += " -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG=" + shell_quote(build_path);
#else
        configure += " -G Ninja";
#endif
        configure += " -DCMAKE_BUILD_TYPE=Debug -DKATANA_RUNTIME_ROOT=" + shell_quote(runtime_root);
        if (std::system(configure.c_str()) != 0) {
            throw katana::cli::Error(katana::cli::ExitCode::BuildFailure,
                                     "Port-Hostbuild konnte nicht konfiguriert werden.");
        }
        const auto build =
            std::string("cmake --build ") + shell_quote(build_path) + " --target " + target_name;
        if (std::system(build.c_str()) != 0) {
            throw katana::cli::Error(katana::cli::ExitCode::BuildFailure,
                                     "Port-Hosttarget konnte nicht gebaut werden.");
        }
        auto built_executable = build_path / target_name;
#ifdef _WIN32
        built_executable += ".exe";
#endif
        if (!std::filesystem::is_regular_file(built_executable))
            throw katana::cli::Error(katana::cli::ExitCode::BuildFailure,
                                     "Port-Hostbuild besitzt kein ausfuehrbares Artefakt.");
        const auto published_executable = report.output_root / built_executable.filename();
        std::filesystem::copy_file(built_executable,
                                   published_executable,
                                   std::filesystem::copy_options::overwrite_existing);
        auto packed = katana::runtime::PackedDiscSource::open(report.packed_disc);
        packed->verify_all_chunks();
        const auto packed_info = packed->info();
        packed.reset();
        const auto pack_sha256 =
            katana::io::capture_input_provenance("packed-disc", report.packed_disc).sha256;
        const auto executable_sha256 =
            katana::io::capture_input_provenance("host-executable", published_executable).sha256;
        std::ofstream manifest(report.packed_disc_manifest, std::ios::binary | std::ios::trunc);
        manifest << katana::runtime::format_packed_disc_manifest(
            packed_info,
            pack_sha256,
            executable_sha256,
            (std::filesystem::path("..") / published_executable.filename()).generic_string(),
            std::filesystem::file_size(published_executable));
        if (!manifest)
            throw std::runtime_error("Disc-Pack-Manifest konnte nicht finalisiert werden.");
        manifest.close();
        std::filesystem::create_directories(report.output_root / "runtime");
        std::ofstream runtime_manifest(report.output_root / "runtime" / "runtime-dependencies.json",
                                       std::ios::binary | std::ios::trunc);
        runtime_manifest << "{\"schema\":\"katana-runtime-dependencies\",\"version\":1,"
                            "\"linkage\":\"static\",\"job_generation\":\""
                         << packed_info.job_generation << "\",\"files\":[]}\n";
        if (!runtime_manifest)
            throw std::runtime_error(
                "Runtime-Abhaengigkeitsmanifest konnte nicht geschrieben werden.");
        runtime_manifest.close();
        std::filesystem::create_directories(report.output_root / "user-data");
        const auto stale = std::filesystem::path(absolute_output.string() + ".katana-stale-port");
        std::filesystem::remove_all(stale, cleanup_error);
        if (cleanup_error)
            throw std::runtime_error("Altes Port-Backup konnte nicht entfernt werden.");
        if (std::filesystem::exists(absolute_output))
            std::filesystem::rename(absolute_output, stale);
        try {
            std::filesystem::rename(report.output_root, absolute_output);
        } catch (...) {
            if (std::filesystem::exists(stale)) std::filesystem::rename(stale, absolute_output);
            throw;
        }
        std::filesystem::remove_all(stale, cleanup_error);
        std::cout << "Portpaket erzeugt: " << absolute_output.string() << '\n'
                  << "Funktionen: " << report.functions << '\n'
                  << "Partitionen: " << report.partitions << '\n'
                  << "Disc-Pack-Sektoren: " << report.packed_sectors << '\n'
                  << "Disc-Pack-Bytes: " << report.packed_disc_bytes << '\n'
                  << "Debug-Hostbuild erfolgreich: " << target_name << '\n';
        return 0;
    } catch (...) {
        std::filesystem::remove_all(stage, cleanup_error);
        throw;
    }
}

void print_usage(std::ostream& output) {
    output << "Verwendung:\n"
           << "  katana-recomp <Opcode>\n"
           << "  katana-recomp opcode <Opcode>\n"
           << "  katana-recomp isa-report [--json]\n"
           << "  katana-recomp analyze <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp analyze-json <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp cfg-json <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp cfg-dot <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp callgraph-json <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp callgraph-dot <Projektmanifest> [Override-Datei]\n"
           << "  katana-recomp firmware-diagnose <bios|flash> <Datei> [--sha256 <Hash>] "
              "[--include-sensitive]\n"
           << "  katana-recomp disasm <Datei> [Basisadresse]\n"
           << "  katana-recomp blocks <Datei> [Basisadresse]\n"
           << "  katana-recomp functions <Datei> <Einstieg> [Basisadresse]\n"
           << "  katana-recomp ir <Raw|ELF|Manifest> <Einstieg> [Basisadresse] [--directives "
              "<Datei>]\n"
           << "  katana-recomp ir-json <Raw|ELF|Manifest> <Einstieg> [Basisadresse] [--directives "
              "<Datei>]\n"
           << "  katana-recomp emit-cpp <Raw|ELF|Manifest> <Einstieg> <Ausgabe.cpp> [Basisadresse] "
              "[--no-opt] [--dump-ir <Praefix>] [--directives <Datei>]\n\n"
           << "  katana-recomp phase6-probe-source <GDI> <Ausgabe.cpp>\n\n"
           << "  katana-recomp port <Quelle.gdi> --output <Ordner> --target-name <Name>\n"
           << "  katana-recomp probe-port <Quelle.gdi> --output <Ordner> --target-name <Name>\n\n"
           << "  katana-recomp workflow <validate|analyze|codegen|build|run-preflight> "
              "<Projektmanifest> --output <Ordner>\n\n"
           << "Beispiel:\n"
           << "  katana-recomp emit-cpp programm.bin 8C010000 generated.cpp 8C010000\n";
}

} // namespace

int main(const int argc, char* argv[]) {
    using katana::cli::exit_status;
    using katana::cli::ExitCode;
    try {
        if (argc == 2 &&
            (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
            print_usage(std::cout);
            return exit_status(ExitCode::Success);
        }
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            std::cout << "KatanaRecomp " << KATANA_RECOMP_VERSION << '\n';
            return exit_status(ExitCode::Success);
        }
        if (argc == 2 && std::string(argv[1]) == "isa-report") {
            std::cout << katana::sh4::format_isa_coverage_report(
                katana::sh4::build_isa_coverage_report());
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "isa-report" &&
            std::string_view(argv[2]) == "--json") {
            std::cout << katana::sh4::format_alpha_isa_json(
                             katana::sh4::build_isa_coverage_report())
                      << '\n';
            return 0;
        }

        if (argc == 6 && std::string_view(argv[1]) == "workflow" &&
            std::string_view(argv[4]) == "--output") {
            const std::string_view kind_name = argv[2];
            katana::app::JobKind kind;
            if (kind_name == "validate")
                kind = katana::app::JobKind::Validate;
            else if (kind_name == "analyze")
                kind = katana::app::JobKind::Analyze;
            else if (kind_name == "codegen")
                kind = katana::app::JobKind::Codegen;
            else if (kind_name == "build")
                kind = katana::app::JobKind::Build;
            else if (kind_name == "run-preflight")
                kind = katana::app::JobKind::RunPreflight;
            else
                throw std::invalid_argument("workflow erhielt einen unbekannten Jobtyp.");
            katana::app::ApplicationService service;
            const auto result =
                service.execute({"cli-workflow", kind, argv[3], argv[5], KATANA_RECOMP_VERSION},
                                {},
                                [](const katana::app::JobEvent& event) {
                                    std::cerr << katana::app::format_job_event_json(event);
                                    std::cerr.flush();
                                });
            std::cout << katana::app::format_job_result_json(result);
            if (result.state == katana::app::JobState::Completed)
                return exit_status(ExitCode::Success);
            switch (result.failure_category) {
            case katana::app::JobFailureCategory::InputOutput:
                return exit_status(ExitCode::InputOutput);
            case katana::app::JobFailureCategory::CodeGeneration:
                return exit_status(ExitCode::CodeGenerationFailure);
            case katana::app::JobFailureCategory::Build:
                return exit_status(ExitCode::BuildFailure);
            case katana::app::JobFailureCategory::Internal:
                return exit_status(ExitCode::InternalError);
            case katana::app::JobFailureCategory::None:
            case katana::app::JobFailureCategory::Processing:
                return exit_status(ExitCode::ProcessingFailure);
            }
            return exit_status(ExitCode::InternalError);
        }

        if (argc == 7 &&
            (std::string_view(argv[1]) == "port" || std::string_view(argv[1]) == "probe-port")) {
            const bool diagnostic_partial = std::string_view(argv[1]) == "probe-port";
            std::optional<std::filesystem::path> output_path;
            std::optional<std::string> target_name;
            for (std::size_t argument = 3u; argument < 7u; argument += 2u) {
                const std::string_view option = argv[argument];
                if (option == "--output" && !output_path.has_value()) {
                    output_path = std::filesystem::path(argv[argument + 1u]);
                } else if (option == "--target-name" && !target_name.has_value()) {
                    target_name = argv[argument + 1u];
                } else {
                    throw std::invalid_argument(
                        "port erwartet --output und --target-name jeweils genau einmal.");
                }
            }
            if (!output_path.has_value() || !target_name.has_value()) {
                throw std::invalid_argument(
                    "port erwartet --output und --target-name jeweils genau einmal.");
            }
            return export_port_project(
                std::filesystem::path(argv[2]), *output_path, *target_name, diagnostic_partial);
        }

        if ((argc == 3 || argc == 4) &&
            (std::string(argv[1]) == "analyze" || std::string(argv[1]) == "analyze-json")) {
            return analyze_manifest(
                std::filesystem::path(argv[2]),
                argc == 4 ? std::optional<std::filesystem::path>{std::filesystem::path(argv[3])}
                          : std::nullopt,
                std::string(argv[1]) == "analyze-json");
        }

        if (argc == 3 || argc == 4) {
            const std::string_view command = argv[1];
            if (command == "cfg-json" || command == "cfg-dot" || command == "callgraph-json" ||
                command == "callgraph-dot") {
                return export_analysis_graph(
                    std::filesystem::path(argv[2]),
                    argc == 4 ? std::optional<std::filesystem::path>{std::filesystem::path(argv[3])}
                              : std::nullopt,
                    command);
            }
        }

        if (argc >= 4 && argc <= 7 && std::string_view(argv[1]) == "firmware-diagnose") {
            const std::string_view kind_name = argv[2];
            katana::platform::FirmwareImageKind kind;
            if (kind_name == "bios") {
                kind = katana::platform::FirmwareImageKind::Bios;
            } else if (kind_name == "flash") {
                kind = katana::platform::FirmwareImageKind::Flash;
            } else {
                throw std::invalid_argument("firmware-diagnose erwartet bios oder flash.");
            }
            katana::platform::FirmwareDiagnosticOptions options;
            std::size_t argument = 4u;
            while (argument < static_cast<std::size_t>(argc)) {
                const std::string_view option = argv[argument++];
                if (option == "--sha256" && !options.expected_sha256.has_value() &&
                    argument < static_cast<std::size_t>(argc)) {
                    options.expected_sha256 = argv[argument++];
                } else if (option == "--include-sensitive" && !options.include_sensitive) {
                    options.include_sensitive = true;
                } else {
                    throw std::invalid_argument("Ungueltige firmware-diagnose-Option.");
                }
            }
            return diagnose_firmware(std::filesystem::path(argv[3]), kind, options);
        }

        if (argc == 2) {
            return decode_single_opcode(argv[1]);
        }

        if (argc == 3 && std::string(argv[1]) == "opcode") {
            return decode_single_opcode(argv[2]);
        }

        if ((argc == 3 || argc == 4) && std::string(argv[1]) == "disasm") {
            const auto base_address =
                argc == 4 ? parse_hex_value(argv[3],
                                            std::numeric_limits<std::uint32_t>::max(),
                                            "Die Basisadresse")
                          : 0u;

            return disassemble_file(std::filesystem::path(argv[2]), base_address);
        }

        if ((argc == 3 || argc == 4) && std::string(argv[1]) == "blocks") {
            const auto base_address =
                argc == 4 ? parse_hex_value(argv[3],
                                            std::numeric_limits<std::uint32_t>::max(),
                                            "Die Basisadresse")
                          : 0u;

            return analyze_blocks(std::filesystem::path(argv[2]), base_address);
        }

        if ((argc == 4 || argc == 5) && std::string(argv[1]) == "functions") {
            const auto entry_address = parse_hex_value(
                argv[3], std::numeric_limits<std::uint32_t>::max(), "Die Einstiegsadresse");

            const auto base_address =
                argc == 5 ? parse_hex_value(argv[4],
                                            std::numeric_limits<std::uint32_t>::max(),
                                            "Die Basisadresse")
                          : 0u;

            return analyze_functions(std::filesystem::path(argv[2]), entry_address, base_address);
        }

        if ((argc >= 4 && argc <= 7) &&
            (std::string(argv[1]) == "ir" || std::string(argv[1]) == "ir-json")) {
            const auto entry_address = parse_hex_value(
                argv[3], std::numeric_limits<std::uint32_t>::max(), "Die Einstiegsadresse");

            std::size_t argument = 4u;
            std::uint32_t base_address = 0u;
            if (argument < static_cast<std::size_t>(argc) &&
                !std::string_view(argv[argument]).starts_with("--")) {
                base_address = parse_hex_value(argv[argument++],
                                               std::numeric_limits<std::uint32_t>::max(),
                                               "Die Basisadresse");
            }
            std::optional<std::filesystem::path> override_path;
            while (argument < static_cast<std::size_t>(argc)) {
                const std::string_view option = argv[argument++];
                if ((option != "--overrides" && option != "--directives") || override_path ||
                    argument >= static_cast<std::size_t>(argc)) {
                    throw std::invalid_argument(
                        "Ungueltige IR-Option; erwartet wird --directives <Datei>.");
                }
                override_path = std::filesystem::path(argv[argument++]);
            }

            return analyze_ir(std::filesystem::path(argv[2]),
                              entry_address,
                              base_address,
                              std::string(argv[1]) == "ir-json",
                              override_path);
        }

        if (argc == 4 && std::string(argv[1]) == "phase6-probe-source") {
            return emit_phase6_probe_source(std::filesystem::path(argv[2]),
                                            std::filesystem::path(argv[3]));
        }

        if ((argc >= 5 && argc <= 11) && std::string(argv[1]) == "emit-cpp") {
            const auto entry_address = parse_hex_value(
                argv[3], std::numeric_limits<std::uint32_t>::max(), "Die Einstiegsadresse");

            std::size_t argument = 5u;
            std::uint32_t base_address = 0u;
            if (argument < static_cast<std::size_t>(argc) &&
                !std::string_view(argv[argument]).starts_with("--")) {
                base_address = parse_hex_value(argv[argument++],
                                               std::numeric_limits<std::uint32_t>::max(),
                                               "Die Basisadresse");
            }

            katana::ir::OptimizationOptions optimization_options;
            std::optional<std::filesystem::path> dump_prefix;
            std::optional<std::filesystem::path> override_path;
            while (argument < static_cast<std::size_t>(argc)) {
                const std::string_view option = argv[argument++];
                if (option == "--no-opt") {
                    optimization_options.enabled = false;
                } else if (option == "--dump-ir") {
                    if (dump_prefix || argument >= static_cast<std::size_t>(argc)) {
                        throw std::invalid_argument("--dump-ir erwartet genau ein Praefix.");
                    }
                    dump_prefix = std::filesystem::path(argv[argument++]);
                } else if (option == "--overrides" || option == "--directives") {
                    if (override_path || argument >= static_cast<std::size_t>(argc)) {
                        throw std::invalid_argument("--directives erwartet genau eine Datei.");
                    }
                    override_path = std::filesystem::path(argv[argument++]);
                } else {
                    throw std::invalid_argument("Unbekannte emit-cpp-Option: " +
                                                std::string(option));
                }
            }

            return emit_cpp(std::filesystem::path(argv[2]),
                            entry_address,
                            std::filesystem::path(argv[4]),
                            base_address,
                            optimization_options,
                            dump_prefix,
                            override_path);
        }

        print_usage(std::cerr);
        return exit_status(ExitCode::Usage);
    } catch (const katana::cli::Error& error) {
        std::cerr << "Fehler [" << katana::cli::exit_code_name(error.code())
                  << "]: " << error.what() << '\n';
        return exit_status(error.code());
    } catch (const std::invalid_argument& error) {
        std::cerr << "Fehler [invalid-input]: " << error.what() << '\n';
        return exit_status(ExitCode::InvalidInput);
    } catch (const std::filesystem::filesystem_error& error) {
        std::cerr << "Fehler [input-output]: " << error.what() << '\n';
        return exit_status(ExitCode::InputOutput);
    } catch (const katana::io::InputOutputError& error) {
        std::cerr << "Fehler [input-output]: " << error.what() << '\n';
        return exit_status(ExitCode::InputOutput);
    } catch (const std::runtime_error& error) {
        std::cerr << "Fehler [processing-failure]: " << error.what() << '\n';
        return exit_status(ExitCode::ProcessingFailure);
    } catch (const std::exception& error) {
        std::cerr << "Fehler [internal-error]: " << error.what() << '\n';
        return exit_status(ExitCode::InternalError);
    }
}

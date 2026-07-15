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
#include <string>

namespace {

constexpr std::uint32_t base_address = 0x100u;
constexpr std::array<std::uint8_t, 54> fixture = {
    0x9Du, 0xF0u, 0x9Du, 0xF1u, 0x00u, 0xF1u, 0x02u, 0xF1u,
    0x0Du, 0xF2u, 0x1Du, 0xF3u, 0x0Bu, 0x00u, 0x09u, 0x00u,
    0xFDu, 0xFBu, 0xFDu, 0xF3u, 0x0Bu, 0x00u, 0x09u, 0x00u,
    0x05u, 0xF1u, 0x2Du, 0xF2u, 0x3Du, 0xF2u, 0x0Bu, 0x00u,
    0x09u, 0x00u, 0x00u, 0xF2u, 0xBDu, 0xF2u, 0xADu, 0xF4u,
    0x0Bu, 0x00u, 0x09u, 0x00u, 0x48u, 0xF5u, 0x5Au, 0xF6u,
    0x49u, 0xF7u, 0x7Bu, 0xF6u, 0x5Cu, 0xF8u
};

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::vector<katana::ir::Function> build_program() {
    auto bytes = std::vector<std::uint8_t>(fixture.begin(), fixture.end());
    const std::array<std::uint8_t, 64> tail = {
        0x46u, 0xF9u, 0x97u, 0xF6u, 0x0Bu, 0x00u, 0x09u, 0x00u,
        0x00u, 0xA0u, 0x00u, 0xF1u, 0x0Bu, 0x00u, 0x09u, 0x00u,
        0x10u, 0xF3u, 0x0Bu, 0x00u, 0x09u, 0x00u,
        0x49u, 0xF4u, 0x0Bu, 0x00u, 0x09u, 0x00u,
        0x4Bu, 0xF4u, 0x0Bu, 0x00u, 0x09u, 0x00u,
        0xFDu, 0xF2u, 0x0Bu, 0x00u, 0x09u, 0x00u,
        0x7Du, 0xF4u, 0x0Bu, 0x00u, 0x09u, 0x00u,
        0xEDu, 0xF1u, 0x0Bu, 0x00u, 0x09u, 0x00u,
        0xFDu, 0xF9u, 0x0Bu, 0x00u, 0x09u, 0x00u,
        0x83u, 0x03u, 0x0Bu, 0x00u, 0x09u, 0x00u
    };
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    const auto lines = katana::sh4::disassemble(bytes, base_address);
    constexpr std::array<std::uint32_t, 14> seeds = {
        0x100u, 0x110u, 0x118u, 0x122u, 0x12Cu, 0x13Eu, 0x146u,
        0x14Cu, 0x152u, 0x158u, 0x15Eu, 0x164u, 0x16Au, 0x170u
    };
    const auto functions = katana::analysis::discover_functions(lines, seeds);
    return katana::ir::lower_program(lines, functions);
}

int emit_fixture(const std::string& output_path) {
    const auto source = katana::codegen::emit_cpp_program(build_program(), base_address);
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    return output ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(const int argc, char* argv[]) {
    using katana::sh4::InstructionKind;
    if (argc == 3 && std::string(argv[1]) == "--emit-cpp") {
        return emit_fixture(argv[2]);
    }
    require(argc == 1, "Unerwartete Argumente fuer den FPU-Pipeline-Test.");

    require(katana::sh4::decode(0xF120u).kind == InstructionKind::Fadd,
        "FADD wird nicht dekodiert.");
    require(katana::sh4::decode(0xF123u).kind == InstructionKind::Fdiv,
        "FDIV wird nicht dekodiert.");
    require(katana::sh4::decode(0xF125u).kind == InstructionKind::FcmpGreater,
        "FCMP/GT wird nicht dekodiert.");
    require(katana::sh4::decode(0xF43Du).kind == InstructionKind::Ftrc,
        "FTRC wird nicht dekodiert.");
    require(katana::sh4::decode(0xF4ADu).kind == InstructionKind::FcnvSingleToDouble,
        "FCNVSD wird nicht dekodiert.");
    require(katana::sh4::decode(0xFBFDu).kind == InstructionKind::Frchg,
        "FRCHG wird nicht dekodiert.");
    require(katana::sh4::decode(0xF47Du).kind == InstructionKind::Fsrra,
        "FSRRA wird nicht dekodiert.");
    require(katana::sh4::decode(0xF2FDu).kind == InstructionKind::Fsca,
        "FSCA wird nicht dekodiert.");
    require(katana::sh4::decode(0xF5EDu).kind == InstructionKind::Fipr,
        "FIPR wird nicht dekodiert.");
    require(katana::sh4::decode(0xF9FDu).kind == InstructionKind::Ftrv,
        "FTRV wird nicht dekodiert.");
    for (std::uint16_t index = 0u; index < 16u; ++index) {
        require(
            katana::sh4::decode(static_cast<std::uint16_t>((index << 8u) | 0x0083u)).kind ==
                InstructionKind::Prefetch,
            "PREF @Rn wird nicht fuer jedes Register dekodiert."
        );
    }

    const auto program = build_program();
    const auto source = katana::codegen::emit_cpp_program(program, base_address);
    require(
        source.find("katana::runtime::fpu_binary") != std::string::npos &&
        source.find("cpu.toggle_fpu_register_bank()") != std::string::npos &&
        source.find("write_fpu_pair_bits") != std::string::npos &&
        source.find("raise_fpu_disabled") != std::string::npos,
        "FPU-IR erreicht den Runtime-basierten C++-Emitter nicht vollstaendig."
    );

    std::cout << "FPU-Decoder-, IR- und Codegen-Pipeline erfolgreich.\n";
    return EXIT_SUCCESS;
}

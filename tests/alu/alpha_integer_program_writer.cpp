#include "katana/analysis/function_analysis.hpp"
#include "katana/codegen/cpp_emitter.hpp"
#include "katana/ir/lower.hpp"
#include "katana/sh4/disassembler.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>

int main(const int argc, char* argv[]) {
    if (argc != 2) return EXIT_FAILURE;
    constexpr std::array<std::uint8_t, 18u> bytes = {
        0x28u,
        0x00u, // clrmac
        0x0Fu,
        0xCCu, // tst.b #0x0F,@(r0,gbr)
        0x29u,
        0x02u, // movt r2
        0x0Fu,
        0xCDu, // and.b #0x0F,@(r0,gbr)
        0x30u,
        0xCFu, // or.b #0x30,@(r0,gbr)
        0x3Fu,
        0xCEu, // xor.b #0x3F,@(r0,gbr)
        0x1Bu,
        0x41u, // tas.b @r1
        0x0Bu,
        0x00u, // rts
        0x09u,
        0x00u // delay-slot nop
    };
    const auto lines = katana::sh4::disassemble(bytes, 0x1000u);
    constexpr std::array<std::uint32_t, 1u> seeds{0x1000u};
    const auto functions = katana::analysis::discover_functions(lines, seeds);
    const auto program = katana::ir::lower_program(lines, functions);
    const auto source = katana::codegen::emit_cpp_program(program, 0x1000u);
    std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
    output.write(source.data(), static_cast<std::streamsize>(source.size()));
    return output ? EXIT_SUCCESS : EXIT_FAILURE;
}

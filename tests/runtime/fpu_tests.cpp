#include "katana/runtime/fpu.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}

int main() {
    using namespace katana::runtime;

    CpuState cpu;
    write_fr_single(cpu, 0u, 2.0f);
    write_fr_single(cpu, 1u, 3.5f);
    fpu_binary(cpu, FpuBinaryOperation::Add, 0u, 1u);
    require(read_fr_single(cpu, 1u) == 5.5f, "FADD single liefert ein falsches Ergebnis.");
    fpu_binary(cpu, FpuBinaryOperation::Subtract, 0u, 1u);
    require(read_fr_single(cpu, 1u) == 3.5f, "FSUB single liefert ein falsches Ergebnis.");
    fpu_binary(cpu, FpuBinaryOperation::Multiply, 0u, 1u);
    require(read_fr_single(cpu, 1u) == 7.0f, "FMUL single liefert ein falsches Ergebnis.");
    fpu_binary(cpu, FpuBinaryOperation::Divide, 0u, 1u);
    require(read_fr_single(cpu, 1u) == 3.5f, "FDIV single liefert ein falsches Ergebnis.");

    cpu.fpul = static_cast<std::uint32_t>(-17);
    fpu_float_from_fpul(cpu, 2u);
    require(read_fr_single(cpu, 2u) == -17.0f, "FLOAT single interpretiert FPUL nicht vorzeichenbehaftet.");
    write_fr_single(cpu, 3u, -12.75f);
    fpu_truncate_to_fpul(cpu, 3u);
    require(cpu.fpul == static_cast<std::uint32_t>(-12), "FTRC rundet nicht gegen null.");

    write_fr_single(cpu, 4u, std::numeric_limits<float>::quiet_NaN());
    write_fr_single(cpu, 5u, 1.0f);
    fpu_binary(cpu, FpuBinaryOperation::Add, 4u, 5u);
    require(cpu.fr[5] == 0x7FBFFFFFu, "NaN-Ergebnis wird nicht deterministisch kanonisiert.");
    fpu_compare_equal(cpu, 4u, 5u);
    require(!cpu.t, "FCMP/EQ behandelt NaN faelschlich als gleich.");

    write_fr_single(cpu, 6u, std::numeric_limits<float>::infinity());
    write_fr_single(cpu, 7u, 2.0f);
    fpu_binary(cpu, FpuBinaryOperation::Divide, 7u, 6u);
    require(std::isinf(read_fr_single(cpu, 6u)), "Infinity geht in FDIV verloren.");

    cpu.write_fpscr(0u);
    write_fr_single(cpu, 0u, 1.0f);
    write_fr_single(cpu, 1u, std::ldexp(3.0f, -25));
    fpu_binary(cpu, FpuBinaryOperation::Add, 1u, 0u);
    const float rounded_nearest = read_fr_single(cpu, 0u);
    cpu.write_fpscr(1u);
    write_fr_single(cpu, 0u, 1.0f);
    fpu_binary(cpu, FpuBinaryOperation::Add, 1u, 0u);
    require(
        rounded_nearest == std::nextafter(1.0f, 2.0f) &&
        read_fr_single(cpu, 0u) == 1.0f,
        "FPSCR.RM unterscheidet Round-to-Nearest und Round-to-Zero nicht."
    );

    cpu.write_fpscr(fpscr_pr_mask);
    write_dr_double(cpu, 8u, 1.25);
    write_dr_double(cpu, 10u, 2.5);
    fpu_binary(cpu, FpuBinaryOperation::Add, 8u, 10u);
    require(read_dr_double(cpu, 10u) == 3.75, "FADD double liefert ein falsches Ergebnis.");
    fpu_compare_greater(cpu, 8u, 10u);
    require(cpu.t, "FCMP/GT double liefert ein falsches T-Bit.");

    cpu.fpul = std::bit_cast<std::uint32_t>(1.5f);
    fpu_convert_single_to_double(cpu, 12u);
    require(read_dr_double(cpu, 12u) == 1.5, "FCNVSD konvertiert FPUL falsch.");
    fpu_convert_double_to_single(cpu, 12u);
    require(std::bit_cast<float>(cpu.fpul) == 1.5f, "FCNVDS konvertiert DRn falsch.");

    std::cout << "SH-4-FPU-Runtime-Grundoperationen erfolgreich.\n";
    return EXIT_SUCCESS;
}

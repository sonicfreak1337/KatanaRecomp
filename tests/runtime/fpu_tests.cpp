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

} // namespace

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
    require(read_fr_single(cpu, 2u) == -17.0f,
            "FLOAT single interpretiert FPUL nicht vorzeichenbehaftet.");
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
    require(rounded_nearest == std::nextafter(1.0f, 2.0f) && read_fr_single(cpu, 0u) == 1.0f,
            "FPSCR.RM unterscheidet Round-to-Nearest und Round-to-Zero nicht.");

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

    cpu.write_fpscr(fpscr_pr_mask);
    write_dr_double(cpu, 0u, 2147483647.0);
    fpu_truncate_to_fpul(cpu, 0u);
    require(cpu.fpul == 0x7FFFFFFFu, "FTRC verliert die exakte positive Integergrenze.");
    write_dr_double(cpu, 0u, -2147483648.0);
    fpu_truncate_to_fpul(cpu, 0u);
    require(cpu.fpul == 0x80000000u, "FTRC verliert die exakte negative Integergrenze.");
    write_dr_double(cpu, 0u, 2147483648.0);
    fpu_truncate_to_fpul(cpu, 0u);
    require(cpu.fpul == 0x7FFFFFFFu, "FTRC saettigt positive Uebersteuerung nicht.");
    write_dr_double(cpu, 0u, -2147483649.0);
    fpu_truncate_to_fpul(cpu, 0u);
    require(cpu.fpul == 0x80000000u, "FTRC saettigt negative Uebersteuerung nicht.");
    write_dr_double(cpu, 0u, std::numeric_limits<double>::quiet_NaN());
    fpu_truncate_to_fpul(cpu, 0u);
    require(cpu.fpul == 0x80000000u, "FTRC behandelt NaN nicht deterministisch.");

    cpu.write_fpscr(0u);
    write_fr_single(cpu, 0u, 0.0f);
    write_fr_single(cpu, 1u, 1.0f);
    fpu_binary(cpu, FpuBinaryOperation::Divide, 0u, 1u);
    require(cpu.fr[1] == 0x7F800000u, "FDIV durch +0 liefert nicht +Infinity.");
    write_fr_single(cpu, 0u, -0.0f);
    write_fr_single(cpu, 1u, 1.0f);
    fpu_binary(cpu, FpuBinaryOperation::Divide, 0u, 1u);
    require(cpu.fr[1] == 0xFF800000u, "FDIV durch -0 verliert das Vorzeichen.");
    write_fr_single(cpu, 0u, 0.0f);
    write_fr_single(cpu, 1u, 0.0f);
    fpu_binary(cpu, FpuBinaryOperation::Divide, 0u, 1u);
    require(cpu.fr[1] == 0x7FBFFFFFu, "FDIV 0/0 liefert kein kanonisches NaN.");

    write_fr_single(cpu, 2u, -1.0f);
    fpu_square_root(cpu, 2u);
    require(cpu.fr[2] == 0x7FBFFFFFu, "FSQRT eines negativen Werts liefert kein kanonisches NaN.");
    write_fr_single(cpu, 2u, -0.0f);
    fpu_square_root(cpu, 2u);
    require(cpu.fr[2] == 0x80000000u, "FSQRT verliert das Vorzeichen von -0.");

    cpu.fr[3] = 0xFFC00000u;
    cpu.fr[4] = 0x7F800001u;
    write_fr_single(cpu, 5u, 1.0f);
    fpu_compare_equal(cpu, 3u, 5u);
    require(!cpu.t, "FCMP/EQ behandelt ein negatives NaN als geordnet.");
    fpu_compare_greater(cpu, 4u, 5u);
    require(!cpu.t, "FCMP/GT behandelt ein Signaling-NaN als geordnet.");
    fpu_binary(cpu, FpuBinaryOperation::Add, 3u, 5u);
    require(cpu.fr[5] == 0x7FBFFFFFu, "Ein negatives NaN wird nicht positiv kanonisiert.");
    write_fr_single(cpu, 5u, 1.0f);
    fpu_binary(cpu, FpuBinaryOperation::Add, 4u, 5u);
    require(cpu.fr[5] == 0x7FBFFFFFu, "Ein Signaling-NaN wird nicht kanonisiert.");
    write_fr_single(cpu, 6u, std::numeric_limits<float>::infinity());
    write_fr_single(cpu, 7u, std::numeric_limits<float>::infinity());
    fpu_compare_equal(cpu, 6u, 7u);
    require(cpu.t, "FCMP/EQ erkennt gleiche Infinities nicht.");
    write_fr_single(cpu, 7u, 1.0f);
    fpu_compare_greater(cpu, 7u, 6u);
    require(cpu.t, "FCMP/GT ordnet Infinity nicht oberhalb endlicher Werte ein.");

    cpu.write_fpscr(fpscr_pr_mask);
    write_dr_double(cpu, 0u, 1.0);
    write_dr_double(cpu, 2u, std::ldexp(3.0, -54));
    fpu_binary(cpu, FpuBinaryOperation::Add, 2u, 0u);
    const double double_nearest = read_dr_double(cpu, 0u);
    cpu.write_fpscr(fpscr_pr_mask | 1u);
    write_dr_double(cpu, 0u, 1.0);
    fpu_binary(cpu, FpuBinaryOperation::Add, 2u, 0u);
    require(double_nearest == std::nextafter(1.0, 2.0) && read_dr_double(cpu, 0u) == 1.0,
            "FPSCR.RM wirkt nicht auf Double-Precision-Ergebnisse.");

    cpu.write_fpscr(fpscr_pr_mask);
    write_dr_double(cpu, 4u, 1.0 + std::ldexp(3.0, -25));
    fpu_convert_double_to_single(cpu, 4u);
    const float conversion_nearest = std::bit_cast<float>(cpu.fpul);
    cpu.write_fpscr(fpscr_pr_mask | 1u);
    fpu_convert_double_to_single(cpu, 4u);
    require(conversion_nearest == std::nextafter(1.0f, 2.0f) &&
                std::bit_cast<float>(cpu.fpul) == 1.0f,
            "FPSCR.RM wirkt nicht auf FCNVDS.");

    cpu.write_fpscr(fpscr_pr_mask | fpscr_dn_mask);
    write_dr_double(cpu, 4u, std::ldexp(1.0, -140));
    fpu_convert_double_to_single(cpu, 4u);
    require(cpu.fpul == 0x00000000u,
            "FCNVDS spuelt ein positives subnormales Ergebnis bei DN=1 nicht auf +0.");
    write_dr_double(cpu, 4u, -std::ldexp(1.0, -140));
    fpu_convert_double_to_single(cpu, 4u);
    require(cpu.fpul == 0x80000000u,
            "FCNVDS spuelt ein negatives subnormales Ergebnis bei DN=1 nicht auf -0.");

    cpu.write_fpscr(0u);
    cpu.fpul = 0x7FFFFFFFu;
    fpu_float_from_fpul(cpu, 8u);
    const std::uint32_t float_nearest_bits = cpu.fr[8];
    cpu.write_fpscr(1u);
    fpu_float_from_fpul(cpu, 8u);
    require(float_nearest_bits == 0x4F000000u && cpu.fr[8] == 0x4EFFFFFFu,
            "FPSCR.RM wirkt nicht auf FLOAT.");

    cpu.write_fpscr(0u);
    write_fr_single(cpu, 0u, 2.0f);
    fpu_multiply_accumulate(cpu, 0u, 0u);
    require(read_fr_single(cpu, 0u) == 6.0f, "FMAC scheitert bei FR0 == FRm == FRn.");

    cpu.write_fpscr(0u);
    cpu.fr[0] = 0x00000001u;
    cpu.fr[1] = 0x3F800000u;
    fpu_binary(cpu, FpuBinaryOperation::Multiply, 0u, 1u);
    require(cpu.fr[1] == 0x00000001u, "DN=0 erhaelt ein Single-Denormalergebnis nicht.");
    cpu.write_fpscr(fpscr_dn_mask);
    cpu.fr[0] = 0x80000001u;
    cpu.fr[1] = 0x3F800000u;
    fpu_binary(cpu, FpuBinaryOperation::Multiply, 0u, 1u);
    require(cpu.fr[1] == 0x80000000u, "DN=1 erhaelt das Vorzeichen einer denormalen Null nicht.");
    cpu.fr[0] = 0x40000000u;
    cpu.fr[1] = 0x00800000u;
    fpu_binary(cpu, FpuBinaryOperation::Divide, 0u, 1u);
    require(cpu.fr[1] == 0x00000000u, "DN=1 spuelt ein denormales Single-Ergebnis nicht auf null.");
    cpu.fr[2] = 0x80000001u;
    fpu_absolute(cpu, 2u);
    require(cpu.fr[2] == 0x00000001u, "FABS darf Denormalwerte bei DN=1 nicht spuellen.");
    fpu_negate(cpu, 2u);
    require(cpu.fr[2] == 0x80000001u, "FNEG darf Denormalwerte bei DN=1 nicht spuellen.");

    cpu.write_fpscr(fpscr_pr_mask);
    cpu.fr[0] = 0u;
    cpu.fr[1] = 1u;
    write_dr_double(cpu, 2u, 1.0);
    fpu_binary(cpu, FpuBinaryOperation::Multiply, 0u, 2u);
    require(cpu.fr[2] == 0u && cpu.fr[3] == 1u, "DN=0 erhaelt ein Double-Denormalergebnis nicht.");
    cpu.write_fpscr(fpscr_pr_mask | fpscr_dn_mask);
    cpu.fr[0] = 0u;
    cpu.fr[1] = 1u;
    write_dr_double(cpu, 2u, 1.0);
    fpu_binary(cpu, FpuBinaryOperation::Multiply, 0u, 2u);
    require(cpu.fr[2] == 0u && cpu.fr[3] == 0u,
            "DN=1 spuelt ein denormales Double-Ergebnis nicht auf null.");

    cpu.write_fpscr(0u);
    for (const auto angle : {0x0000u, 0x4000u, 0x8000u, 0xC000u}) {
        cpu.fpul = angle;
        fpu_sine_cosine(cpu, 2u);
        const float sine = read_fr_single(cpu, 2u);
        const float cosine = read_fr_single(cpu, 3u);
        require(std::fabs(sine * sine + cosine * cosine - 1.0f) <= 1.0e-6f,
                "FSCA-Quadrantenanker liegt nicht auf dem Einheitskreis.");
    }
    cpu.fpul = 0x2000u;
    fpu_sine_cosine(cpu, 4u);
    require(std::fabs(read_fr_single(cpu, 4u) - 0.70710677f) <= 2.0e-7f &&
                std::fabs(read_fr_single(cpu, 5u) - 0.70710677f) <= 2.0e-7f,
            "FSCA verlaesst die dokumentierte Single-Toleranz.");

    write_fr_single(cpu, 6u, 4.0f);
    fpu_reciprocal_square_root(cpu, 6u);
    require(std::fabs(read_fr_single(cpu, 6u) - 0.5f) <= 2.0e-7f,
            "FSRRA liefert fuer 4 nicht 1/2.");
    write_fr_single(cpu, 6u, -1.0f);
    fpu_reciprocal_square_root(cpu, 6u);
    require(cpu.fr[6] == 0x7FBFFFFFu, "FSRRA kanonisiert negative Eingaben nicht.");

    for (std::uint8_t i = 0; i < 4u; ++i) {
        write_fr_single(cpu, i, static_cast<float>(i + 1u));
    }
    fpu_inner_product(cpu, 0u, 0u);
    require(read_fr_single(cpu, 3u) == 30.0f,
            "FIPR scheitert bei vollstaendig ueberlappenden Vektoren.");

    for (std::uint8_t i = 0; i < 16u; ++i) {
        cpu.xf[i] = std::bit_cast<std::uint32_t>(0.0f);
    }
    cpu.xf[0] = std::bit_cast<std::uint32_t>(2.0f);
    cpu.xf[5] = std::bit_cast<std::uint32_t>(3.0f);
    cpu.xf[10] = std::bit_cast<std::uint32_t>(4.0f);
    cpu.xf[15] = std::bit_cast<std::uint32_t>(5.0f);
    for (std::uint8_t i = 0; i < 4u; ++i) {
        write_fr_single(cpu, static_cast<std::uint8_t>(8u + i), 1.0f);
    }
    fpu_transform_vector(cpu, 8u);
    require(read_fr_single(cpu, 8u) == 2.0f && read_fr_single(cpu, 9u) == 3.0f &&
                read_fr_single(cpu, 10u) == 4.0f && read_fr_single(cpu, 11u) == 5.0f,
            "FTRV liest XMTRX nicht aus der XF-Hintergrundbank.");

    std::cout << "SH-4-FPU-Runtime-Grundoperationen erfolgreich.\n";
    return EXIT_SUCCESS;
}

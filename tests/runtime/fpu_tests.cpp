#include "katana/runtime/fpu.hpp"
#include "katana/runtime/exception.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#if defined(__SSE__) || defined(_M_X64) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define KATANA_TEST_HAS_SSE_MXCSR 1
#else
#define KATANA_TEST_HAS_SSE_MXCSR 0
#endif

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "TEST FEHLGESCHLAGEN: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// The SH-4 FPU documentation gives an absolute (not relative) bound for the
// approximating instructions. Keep the stricter repository bound here so a
// large reference value cannot hide a materially wrong result.
constexpr double sh4_approximation_error_bound = 2.0e-7;

[[nodiscard]] bool within_sh4_approximation(const float actual,
                                            const float reference) {
    return std::fabs(static_cast<double>(actual) -
                     static_cast<double>(reference)) <=
           sh4_approximation_error_bound;
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
#if KATANA_TEST_HAS_SSE_MXCSR
    constexpr std::uint32_t host_denormals_are_zero_mask = 0x00000040u;
    constexpr std::uint32_t host_flush_to_zero_mask = 0x00008000u;
    const auto original_mxcsr = _mm_getcsr();
    const auto poisoned_mxcsr =
        original_mxcsr | host_denormals_are_zero_mask | host_flush_to_zero_mask;
    _mm_setcsr(poisoned_mxcsr);
    cpu.fr[0] = 0x00800000u;
    cpu.fr[1] = 0x3F000000u;
    fpu_binary(cpu, FpuBinaryOperation::Multiply, 0u, 1u);
    const auto ambient_result = cpu.fr[1];
    const auto restored_mxcsr = _mm_getcsr();
    _mm_setcsr(original_mxcsr);
    require(ambient_result == 0x00400000u,
            "Ambient MXCSR.FTZ/DAZ verletzt FPSCR.DN=0.");
    require(restored_mxcsr == poisoned_mxcsr,
            "FPU-Operation stellt den ambienten Host-MXCSR nicht wieder her.");

    cpu.write_fpscr(0u);
    _mm_setcsr(poisoned_mxcsr);
    {
        const HostFpuExecutionEpoch epoch(cpu);
        const auto epoch_mxcsr = _mm_getcsr();
        require((epoch_mxcsr & (_MM_ROUND_MASK | host_denormals_are_zero_mask |
                                host_flush_to_zero_mask)) == _MM_ROUND_NEAREST,
                "FPU-Epoche bindet FPSCR.RM oder DN nicht an den Hostzustand.");
        cpu.fr[0] = 0x00800000u;
        cpu.fr[1] = 0x3F000000u;
        fpu_binary(cpu, FpuBinaryOperation::Multiply, 0u, 1u);
        require(cpu.fr[1] == 0x00400000u && _mm_getcsr() == epoch_mxcsr,
                "FPU-Operation verlaesst oder veraendert ihre aktive Hostepoche.");

        cpu.write_fpscr(1u);
        cpu.fpul = 0x7FFFFFFFu;
        fpu_float_from_fpul(cpu, 8u);
        require(cpu.fr[8] == 0x4EFFFFFFu && _mm_getcsr() == epoch_mxcsr,
                "Abweichendes FPSCR innerhalb einer Epoche verliert den lokalen Fallback.");
        cpu.write_fpscr(0u);
    }
    require(_mm_getcsr() == poisoned_mxcsr,
            "FPU-Epoche stellt den ambienten Host-MXCSR nicht exakt wieder her.");

    constexpr std::uint32_t host_exception_status_mask = 0x0000003Fu;
    const auto matching_mxcsr =
        (original_mxcsr &
         ~(_MM_ROUND_MASK | host_denormals_are_zero_mask |
           host_flush_to_zero_mask | host_exception_status_mask)) |
        _MM_ROUND_NEAREST;
    _mm_setcsr(matching_mxcsr);
    {
        const HostFpuExecutionEpoch epoch(cpu);
        _mm_setcsr(_mm_getcsr() | 0x00000001u);
        require(_mm_getcsr() != matching_mxcsr,
                "MXCSR-Sticky-Flag wurde fuer den Epochentest nicht gesetzt.");
    }
    require(_mm_getcsr() == matching_mxcsr,
            "FPU-Epoche laesst bei bereits passenden Controls ein "
            "Host-MXCSR-Sticky-Flag nach aussen lecken.");
    _mm_setcsr(original_mxcsr);
#endif
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
    require(within_sh4_approximation(read_fr_single(cpu, 4u), 0.70710677f) &&
                within_sh4_approximation(read_fr_single(cpu, 5u), 0.70710677f),
            "FSCA verlaesst die dokumentierte Single-Toleranz.");

    constexpr double tau = 6.283185307179586476925286766559;
    {
        const HostFpuExecutionEpoch epoch(cpu);
        for (std::uint32_t angle = 0u; angle <= 0xFFFFu; ++angle) {
            cpu.fpul = angle;
            fpu_sine_cosine(cpu, 2u);
            const double radians = static_cast<double>(angle) * tau / 65536.0;
            const float reference_sine = static_cast<float>(std::sin(radians));
            const float reference_cosine = static_cast<float>(std::cos(radians));
            require(within_sh4_approximation(
                        read_fr_single(cpu, 2u), reference_sine) &&
                        within_sh4_approximation(
                            read_fr_single(cpu, 3u), reference_cosine),
                    "Schneller FSCA-Pfad verletzt die 16-Bit-Phasenreferenz.");
        }
    }
    constexpr std::array<std::uint32_t, 8> fsca_cardinal_bits = {
        0x00000000u, 0x3F800000u,
        0x3F800000u, 0x00000000u,
        0x00000000u, 0xBF800000u,
        0xBF800000u, 0x00000000u};
    for (std::uint32_t quadrant = 0u; quadrant < 4u; ++quadrant) {
        cpu.fpul = quadrant * 0x4000u;
        fpu_sine_cosine(cpu, 2u);
        require(cpu.fr[2] == fsca_cardinal_bits[quadrant * 2u] &&
                    cpu.fr[3] == fsca_cardinal_bits[quadrant * 2u + 1u],
                "FSCA verliert einen exakten Quadrantenanker oder dessen Nullvorzeichen.");
    }

    write_fr_single(cpu, 6u, 4.0f);
    fpu_reciprocal_square_root(cpu, 6u);
    require(within_sh4_approximation(read_fr_single(cpu, 6u), 0.5f),
            "FSRRA liefert fuer 4 nicht 1/2.");
    write_fr_single(cpu, 6u, -1.0f);
    fpu_reciprocal_square_root(cpu, 6u);
    require(cpu.fr[6] == 0x7FBFFFFFu, "FSRRA kanonisiert negative Eingaben nicht.");
    constexpr std::array<std::uint32_t, 7> fsrra_mantissas = {
        0u, 1u, 0x12345u, 0x200000u, 0x3FFFFFu, 0x555555u, 0x7FFFFFu};
    // The repository contract is absolute, so cover the complete positive
    // normal exponent range rather than hiding large-magnitude error behind a
    // relative tolerance.
    for (std::uint32_t exponent = 1u; exponent <= 254u; ++exponent) {
        for (const auto mantissa : fsrra_mantissas) {
            const auto input_bits = (exponent << 23u) | mantissa;
            const float input = std::bit_cast<float>(input_bits);
            const float reference = static_cast<float>(
                1.0 / std::sqrt(static_cast<double>(input)));
            cpu.fr[6] = input_bits;
            fpu_reciprocal_square_root(cpu, 6u);
            require(within_sh4_approximation(
                        read_fr_single(cpu, 6u), reference),
                    "Schneller FSRRA-Pfad verlaesst die dokumentierte "
                    "Approximationstoleranz: exponent=" +
                        std::to_string(exponent) + " mantissa=" +
                        std::to_string(mantissa) + " actual=" +
                        std::to_string(read_fr_single(cpu, 6u)) +
                        " reference=" + std::to_string(reference));
        }
    }
    cpu.fpscr = 0u;
    cpu.trap_pending = false;
    for (const auto input_bits :
         std::array<std::uint32_t, 7>{1u, 2u, 3u, 0x100u, 0x10000u, 0x400000u, 0x7FFFFFu}) {
        cpu.fr[6] = input_bits;
        const auto before = cpu.fr[6];
        fpu_reciprocal_square_root(cpu, 6u);
        require(cpu.fr[6] == before &&
                    (cpu.fpscr & fpscr_cause_fpu_error_mask) != 0u &&
                    (cpu.fpscr & fpscr_flag_mask) == 0u &&
                    cpu.last_exception_cause == ExceptionCause::FpuException &&
                    cpu.expevt == event_fpu_exception && cpu.trap_pending,
                "FSRRA behandelt positive Binary32-Subnormale nicht als "
                "unmaskierbaren SH-4-FPU-Fehler.");
        cpu.fpscr = 0u;
        cpu.trap_pending = false;
        cpu.write_sr(0u);
    }

    auto arm_fpu_exception_cpu = [](CpuState& state, const std::uint32_t fpscr) {
        state.pc = 0x8C001234u;
        state.active_instruction_pc = state.pc;
        state.vbr = 0x8C000000u;
        state.write_fpscr(fpscr);
    };

    {
        CpuState exception_cpu;
        arm_fpu_exception_cpu(exception_cpu, fpscr_cause_invalid_mask | fpscr_flag_invalid_mask);
        exception_cpu.fr[6] = std::bit_cast<std::uint32_t>(4.0f);
        fpu_reciprocal_square_root(exception_cpu, 6u);
        require((exception_cpu.fpscr & fpscr_cause_inexact_mask) != 0u &&
                    (exception_cpu.fpscr & fpscr_flag_inexact_mask) != 0u &&
                    (exception_cpu.fpscr & fpscr_cause_invalid_mask) == 0u &&
                    (exception_cpu.fpscr & fpscr_flag_invalid_mask) != 0u &&
                    !exception_cpu.trap_pending,
                "FSRRA setzt fuer positive Normalwerte nicht exakt Cause/Flag I.");
    }

    {
        CpuState exception_cpu;
        arm_fpu_exception_cpu(exception_cpu, 0u);
        exception_cpu.fr[6] = 0x00000000u;
        fpu_reciprocal_square_root(exception_cpu, 6u);
        require(exception_cpu.fr[6] == 0x7F800000u &&
                    (exception_cpu.fpscr & fpscr_cause_divide_by_zero_mask) != 0u &&
                    (exception_cpu.fpscr & fpscr_flag_divide_by_zero_mask) != 0u &&
                    !exception_cpu.trap_pending,
                "FSRRA setzt fuer +0 nicht Cause/Flag Z und +Infinity.");

        exception_cpu = CpuState{};
        arm_fpu_exception_cpu(exception_cpu, 0u);
        exception_cpu.fr[6] = 0x80000000u;
        fpu_reciprocal_square_root(exception_cpu, 6u);
        require(exception_cpu.fr[6] == 0xFF800000u &&
                    (exception_cpu.fpscr & fpscr_cause_divide_by_zero_mask) != 0u &&
                    (exception_cpu.fpscr & fpscr_flag_divide_by_zero_mask) != 0u,
                "FSRRA verliert bei -0 das Vorzeichen oder Cause/Flag Z.");
    }

    for (const auto input_bits :
         std::array<std::uint32_t, 3>{0xBF800000u, 0xFF800000u, 0x7FC00001u}) {
        CpuState exception_cpu;
        arm_fpu_exception_cpu(exception_cpu, 0u);
        exception_cpu.fr[6] = input_bits;
        fpu_reciprocal_square_root(exception_cpu, 6u);
        require(exception_cpu.fr[6] == 0x7FBFFFFFu &&
                    (exception_cpu.fpscr & fpscr_cause_invalid_mask) != 0u &&
                    (exception_cpu.fpscr & fpscr_flag_invalid_mask) != 0u &&
                    !exception_cpu.trap_pending,
                "FSRRA behandelt negative/Signaling-NaN-Eingaenge nicht als Invalid.");
    }

    {
        CpuState exception_cpu;
        arm_fpu_exception_cpu(exception_cpu, 0u);
        exception_cpu.fr[6] = 0x7F800001u;
        fpu_reciprocal_square_root(exception_cpu, 6u);
        require(exception_cpu.fr[6] == 0x7FBFFFFFu &&
                    (exception_cpu.fpscr & fpscr_cause_invalid_mask) == 0u &&
                    (exception_cpu.fpscr & fpscr_flag_invalid_mask) == 0u &&
                    !exception_cpu.trap_pending,
                "FSRRA loest einen Quiet-NaN faelschlich als Invalid aus.");
    }

    {
        CpuState exception_cpu;
        arm_fpu_exception_cpu(exception_cpu, 0u);
        exception_cpu.fr[6] = 0x00000001u;
        const auto before = exception_cpu.fr[6];
        fpu_reciprocal_square_root(exception_cpu, 6u);
        require(exception_cpu.fr[6] == before &&
                    (exception_cpu.fpscr & fpscr_cause_fpu_error_mask) != 0u &&
                    (exception_cpu.fpscr & fpscr_flag_mask) == 0u &&
                    exception_cpu.trap_pending &&
                    exception_cpu.last_exception_cause == ExceptionCause::FpuException &&
                    exception_cpu.expevt == event_fpu_exception &&
                    exception_cpu.spc == exception_cpu.active_instruction_pc,
                "FSRRA behandelt positive Denormale nicht als unmaskierbaren FPU-Fehler.");
    }

    {
        CpuState exception_cpu;
        arm_fpu_exception_cpu(exception_cpu, fpscr_enable_inexact_mask);
        exception_cpu.fr[6] = std::bit_cast<std::uint32_t>(4.0f);
        const auto before = exception_cpu.fr[6];
        fpu_reciprocal_square_root(exception_cpu, 6u);
        require(exception_cpu.fr[6] == before && exception_cpu.trap_pending &&
                    (exception_cpu.fpscr & fpscr_cause_inexact_mask) != 0u &&
                    (exception_cpu.fpscr & fpscr_flag_inexact_mask) != 0u &&
                    exception_cpu.last_exception_cause == ExceptionCause::FpuException &&
                    exception_cpu.expevt == event_fpu_exception,
                "FSRRA EN.I trappt nicht vor dem Schreiben des Zielregisters.");

        exception_cpu = CpuState{};
        arm_fpu_exception_cpu(exception_cpu, fpscr_enable_divide_by_zero_mask);
        exception_cpu.fr[6] = 0x00000000u;
        fpu_reciprocal_square_root(exception_cpu, 6u);
        require(exception_cpu.fr[6] == 0x00000000u && exception_cpu.trap_pending &&
                    (exception_cpu.fpscr & fpscr_cause_divide_by_zero_mask) != 0u &&
                    (exception_cpu.fpscr & fpscr_flag_divide_by_zero_mask) != 0u,
                "FSRRA EN.Z veraendert das Zielregister trotz Divide-by-zero-Trap.");

        exception_cpu = CpuState{};
        arm_fpu_exception_cpu(exception_cpu, fpscr_enable_invalid_mask);
        exception_cpu.fr[6] = std::bit_cast<std::uint32_t>(-1.0f);
        fpu_reciprocal_square_root(exception_cpu, 6u);
        require(exception_cpu.fr[6] == std::bit_cast<std::uint32_t>(-1.0f) &&
                    exception_cpu.trap_pending &&
                    (exception_cpu.fpscr & fpscr_cause_invalid_mask) != 0u &&
                    (exception_cpu.fpscr & fpscr_flag_invalid_mask) != 0u,
                "FSRRA EN.V veraendert das Zielregister trotz Invalid-Trap.");
    }

    {
        CpuState exception_cpu;
        arm_fpu_exception_cpu(exception_cpu, fpscr_enable_inexact_mask);
        exception_cpu.fpul = 0x2000u;
        exception_cpu.fr[2] = 0x12345678u;
        exception_cpu.fr[3] = 0x9ABCDEF0u;
        fpu_sine_cosine(exception_cpu, 2u);
        require(exception_cpu.fr[2] == 0x12345678u && exception_cpu.fr[3] == 0x9ABCDEF0u &&
                    exception_cpu.trap_pending &&
                    (exception_cpu.fpscr & fpscr_cause_inexact_mask) != 0u &&
                    (exception_cpu.fpscr & fpscr_flag_inexact_mask) != 0u &&
                    exception_cpu.last_exception_cause == ExceptionCause::FpuException &&
                    exception_cpu.expevt == event_fpu_exception,
                "FSCA EN.I veraendert die beiden Zielregister trotz Inexact-Trap.");
    }

    {
        CpuState exception_cpu;
        arm_fpu_exception_cpu(exception_cpu, fpscr_enable_inexact_mask);
        constexpr std::uint32_t delay_slot_owner = 0x8C001232u;
        exception_cpu.fr[6] = std::bit_cast<std::uint32_t>(4.0f);
        const bool trapped = fpu_reciprocal_square_root(
            exception_cpu, 6u, delay_slot_owner);
        require(trapped && exception_cpu.trap_pending &&
                    exception_cpu.spc == delay_slot_owner &&
                    exception_cpu.exception_in_delay_slot &&
                    exception_cpu.last_exception_instruction_pc == 0x8C001234u &&
                    exception_cpu.last_exception_owner_pc == delay_slot_owner,
                "FSRRA-Exception verliert Delay-Slot-Owner oder exakte Instruction-PC.");
    }

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

    bool simd_available = true;
    for (std::uint32_t iteration = 0u; iteration < 256u && simd_available;
         ++iteration) {
        CpuState scalar_cpu;
        CpuState simd_cpu;
        const auto fpscr = (iteration & 1u) |
                           ((iteration & 2u) != 0u ? fpscr_dn_mask : 0u);
        scalar_cpu.write_fpscr(fpscr);
        simd_cpu.write_fpscr(fpscr);
        for (std::uint32_t index = 0u; index < 16u; ++index) {
            const auto signed_value = static_cast<std::int32_t>(
                                          (iteration * 131u + index * 47u) %
                                          2001u) -
                                      1000;
            const auto value = static_cast<float>(signed_value) / 64.0f;
            scalar_cpu.xf[index] = std::bit_cast<std::uint32_t>(value);
            simd_cpu.xf[index] = scalar_cpu.xf[index];
        }
        for (std::uint32_t index = 0u; index < 4u; ++index) {
            const auto signed_value = static_cast<std::int32_t>(
                                          (iteration * 73u + index * 29u) %
                                          1001u) -
                                      500;
            const auto value = static_cast<float>(signed_value) / 32.0f;
            scalar_cpu.fr[8u + index] = std::bit_cast<std::uint32_t>(value);
            simd_cpu.fr[8u + index] = scalar_cpu.fr[8u + index];
        }
        const auto before = simd_cpu.fr;
        fpu_transform_vector(scalar_cpu, 8u);
        simd_available = try_fpu_transform_vector_simd(simd_cpu, 8u);
        if (!simd_available) {
            require(simd_cpu.fr == before,
                    "Nicht verfuegbarer FTRV-SIMD-Pfad veraendert CpuState.");
            break;
        }
        require(std::equal(scalar_cpu.fr.begin() + 8u,
                           scalar_cpu.fr.begin() + 12u,
                           simd_cpu.fr.begin() + 8u),
                "FTRV-SIMD-Pfad aendert FMA- oder Rundungsreihenfolge.");
    }

    CpuState nonfinite_simd_cpu;
    nonfinite_simd_cpu.fr[8u] =
        std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity());
    const auto nonfinite_before = nonfinite_simd_cpu.fr;
    require(!try_fpu_transform_vector_simd(nonfinite_simd_cpu, 8u) &&
                nonfinite_simd_cpu.fr == nonfinite_before,
            "FTRV-SIMD-Pfad akzeptiert nicht-endliche Providerdaten.");

    std::cout << "SH-4-FPU-Runtime-Grundoperationen erfolgreich.\n";
    return EXIT_SUCCESS;
}

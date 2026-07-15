#include "katana/runtime/fpu.hpp"

#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <limits>

namespace katana::runtime {
namespace {

constexpr std::uint32_t canonical_single_nan = 0x7FBFFFFFu;
constexpr std::uint64_t canonical_double_nan = 0x7FF7FFFFFFFFFFFFull;

class ScopedHostRounding final {
public:
    explicit ScopedHostRounding(const CpuState& cpu) noexcept
        : previous_(std::fegetround()) {
        const int requested =
            (cpu.read_fpscr() & fpscr_rounding_mode_mask) == 1u
            ? FE_TOWARDZERO
            : FE_TONEAREST;
        static_cast<void>(std::fesetround(requested));
    }

    ~ScopedHostRounding() {
        static_cast<void>(std::fesetround(previous_));
    }

private:
    int previous_ = FE_TONEAREST;
};

std::uint8_t even_register(const std::uint8_t index) noexcept {
    return static_cast<std::uint8_t>(index & 0x0Eu);
}

template<typename Float>
Float flush_denormalized(const CpuState& cpu, const Float value) noexcept {
    if (cpu.fpu_flush_denormals() && std::fpclassify(value) == FP_SUBNORMAL) {
        return std::copysign(static_cast<Float>(0.0), value);
    }
    return value;
}

float read_single_operand(const CpuState& cpu, const std::uint8_t index) noexcept {
    return flush_denormalized(
        cpu,
        std::bit_cast<float>(cpu.fr[index & 0x0Fu])
    );
}

double read_double_operand(const CpuState& cpu, const std::uint8_t index) noexcept {
    return flush_denormalized(cpu, read_dr_double(cpu, index));
}

void write_single_result(CpuState& cpu, const std::uint8_t index, const float value) noexcept {
    const float normalized = flush_denormalized(cpu, value);
    cpu.fr[index & 0x0Fu] = std::isnan(normalized)
        ? canonical_single_nan
        : std::bit_cast<std::uint32_t>(normalized);
}

void write_double_result(CpuState& cpu, const std::uint8_t index, const double value) noexcept {
    const std::uint8_t even = even_register(index);
    const double normalized = flush_denormalized(cpu, value);
    const std::uint64_t bits = std::isnan(normalized)
        ? canonical_double_nan
        : std::bit_cast<std::uint64_t>(normalized);
    cpu.fr[even] = static_cast<std::uint32_t>(bits >> 32u);
    cpu.fr[even + 1u] = static_cast<std::uint32_t>(bits);
}

template<typename Float>
Float binary_result(
    const FpuBinaryOperation operation,
    const Float destination,
    const Float source
) noexcept {
    switch (operation) {
        case FpuBinaryOperation::Add: return destination + source;
        case FpuBinaryOperation::Subtract: return destination - source;
        case FpuBinaryOperation::Multiply: return destination * source;
        case FpuBinaryOperation::Divide: return destination / source;
    }
    return std::numeric_limits<Float>::quiet_NaN();
}

template<typename Float>
std::uint32_t truncate_to_integer_bits(const Float value) noexcept {
    if (std::isnan(value) || value <= static_cast<Float>(std::numeric_limits<std::int32_t>::min())) {
        return 0x80000000u;
    }
    if (value >= static_cast<Float>(std::numeric_limits<std::int32_t>::max())) {
        return 0x7FFFFFFFu;
    }
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}

} // namespace

float read_fr_single(const CpuState& cpu, const std::uint8_t index) noexcept {
    return std::bit_cast<float>(cpu.fr[index & 0x0Fu]);
}

void write_fr_single(CpuState& cpu, const std::uint8_t index, const float value) noexcept {
    write_single_result(cpu, index, value);
}

double read_dr_double(const CpuState& cpu, const std::uint8_t even_index) noexcept {
    const std::uint8_t even = even_register(even_index);
    const std::uint64_t bits =
        (static_cast<std::uint64_t>(cpu.fr[even]) << 32u) |
        cpu.fr[even + 1u];
    return std::bit_cast<double>(bits);
}

void write_dr_double(CpuState& cpu, const std::uint8_t even_index, const double value) noexcept {
    write_double_result(cpu, even_index, value);
}

std::uint64_t read_fpu_pair_bits(
    const CpuState& cpu,
    const std::uint8_t encoded_index
) noexcept {
    const auto& bank = (encoded_index & 1u) != 0u ? cpu.xf : cpu.fr;
    const std::uint8_t even = even_register(encoded_index);
    return (static_cast<std::uint64_t>(bank[even]) << 32u) | bank[even + 1u];
}

void write_fpu_pair_bits(
    CpuState& cpu,
    const std::uint8_t encoded_index,
    const std::uint64_t bits
) noexcept {
    auto& bank = (encoded_index & 1u) != 0u ? cpu.xf : cpu.fr;
    const std::uint8_t even = even_register(encoded_index);
    bank[even] = static_cast<std::uint32_t>(bits >> 32u);
    bank[even + 1u] = static_cast<std::uint32_t>(bits);
}

void fpu_binary(
    CpuState& cpu,
    const FpuBinaryOperation operation,
    const std::uint8_t source,
    const std::uint8_t destination
) noexcept {
    const ScopedHostRounding rounding(cpu);
    if (cpu.fpu_double_precision()) {
        write_double_result(
            cpu,
            destination,
            binary_result(
                operation,
                read_double_operand(cpu, destination),
                read_double_operand(cpu, source)
            )
        );
        return;
    }
    write_single_result(
        cpu,
        destination,
        binary_result(
            operation,
            read_single_operand(cpu, destination),
            read_single_operand(cpu, source)
        )
    );
}

void fpu_absolute(CpuState& cpu, const std::uint8_t destination) noexcept {
    if (cpu.fpu_double_precision()) {
        cpu.fr[even_register(destination)] &= 0x7FFFFFFFu;
    } else {
        cpu.fr[destination & 0x0Fu] &= 0x7FFFFFFFu;
    }
}

void fpu_negate(CpuState& cpu, const std::uint8_t destination) noexcept {
    cpu.fr[cpu.fpu_double_precision() ? even_register(destination) : destination & 0x0Fu] ^=
        0x80000000u;
}

void fpu_square_root(CpuState& cpu, const std::uint8_t destination) noexcept {
    const ScopedHostRounding rounding(cpu);
    if (cpu.fpu_double_precision()) {
        write_double_result(cpu, destination, std::sqrt(read_double_operand(cpu, destination)));
    } else {
        write_single_result(cpu, destination, std::sqrt(read_single_operand(cpu, destination)));
    }
}

void fpu_reciprocal_square_root(CpuState& cpu, const std::uint8_t destination) noexcept {
    const ScopedHostRounding rounding(cpu);
    write_single_result(cpu, destination, 1.0f / std::sqrt(read_single_operand(cpu, destination)));
}

void fpu_sine_cosine(CpuState& cpu, const std::uint8_t destination_even) noexcept {
    const auto angle = static_cast<std::uint16_t>(cpu.fpul);
    float sine = 0.0f;
    float cosine = 0.0f;
    switch (angle) {
        case 0x0000u: sine = 0.0f; cosine = 1.0f; break;
        case 0x4000u: sine = 1.0f; cosine = 0.0f; break;
        case 0x8000u: sine = 0.0f; cosine = -1.0f; break;
        case 0xC000u: sine = -1.0f; cosine = 0.0f; break;
        default: {
            constexpr double tau = 6.283185307179586476925286766559;
            const double radians = static_cast<double>(angle) * tau / 65536.0;
            sine = static_cast<float>(std::sin(radians));
            cosine = static_cast<float>(std::cos(radians));
            break;
        }
    }
    write_single_result(cpu, destination_even, sine);
    write_single_result(cpu, static_cast<std::uint8_t>(destination_even + 1u), cosine);
}

void fpu_inner_product(
    CpuState& cpu,
    const std::uint8_t source_vector,
    const std::uint8_t destination_vector
) noexcept {
    const ScopedHostRounding rounding(cpu);
    float source[4];
    float destination[4];
    for (std::uint8_t i = 0; i < 4u; ++i) {
        source[i] = read_single_operand(cpu, static_cast<std::uint8_t>(source_vector + i));
        destination[i] = read_single_operand(cpu, static_cast<std::uint8_t>(destination_vector + i));
    }
    float result = source[0] * destination[0];
    for (std::uint8_t i = 1; i < 4u; ++i) {
        result = std::fma(source[i], destination[i], result);
    }
    write_single_result(cpu, static_cast<std::uint8_t>(destination_vector + 3u), result);
}

void fpu_transform_vector(CpuState& cpu, const std::uint8_t destination_vector) noexcept {
    const ScopedHostRounding rounding(cpu);
    float vector[4];
    float matrix[16];
    for (std::uint8_t i = 0; i < 4u; ++i) {
        vector[i] = read_single_operand(cpu, static_cast<std::uint8_t>(destination_vector + i));
    }
    for (std::uint8_t i = 0; i < 16u; ++i) {
        matrix[i] = flush_denormalized(cpu, std::bit_cast<float>(cpu.xf[i]));
    }
    for (std::uint8_t row = 0; row < 4u; ++row) {
        float result = matrix[row] * vector[0];
        for (std::uint8_t column = 1; column < 4u; ++column) {
            result = std::fma(matrix[column * 4u + row], vector[column], result);
        }
        write_single_result(cpu, static_cast<std::uint8_t>(destination_vector + row), result);
    }
}

void fpu_multiply_accumulate(
    CpuState& cpu,
    const std::uint8_t source,
    const std::uint8_t destination
) noexcept {
    const ScopedHostRounding rounding(cpu);
    write_single_result(
        cpu,
        destination,
        std::fma(
            read_single_operand(cpu, 0u),
            read_single_operand(cpu, source),
            read_single_operand(cpu, destination)
        )
    );
}

void fpu_compare_equal(
    CpuState& cpu,
    const std::uint8_t source,
    const std::uint8_t destination
) noexcept {
    cpu.t = cpu.fpu_double_precision()
        ? read_double_operand(cpu, destination) == read_double_operand(cpu, source)
        : read_single_operand(cpu, destination) == read_single_operand(cpu, source);
}

void fpu_compare_greater(
    CpuState& cpu,
    const std::uint8_t source,
    const std::uint8_t destination
) noexcept {
    cpu.t = cpu.fpu_double_precision()
        ? read_double_operand(cpu, destination) > read_double_operand(cpu, source)
        : read_single_operand(cpu, destination) > read_single_operand(cpu, source);
}

void fpu_float_from_fpul(CpuState& cpu, const std::uint8_t destination) noexcept {
    const ScopedHostRounding rounding(cpu);
    const auto value = static_cast<std::int32_t>(cpu.fpul);
    if (cpu.fpu_double_precision()) {
        write_double_result(cpu, destination, static_cast<double>(value));
    } else {
        write_single_result(cpu, destination, static_cast<float>(value));
    }
}

void fpu_truncate_to_fpul(CpuState& cpu, const std::uint8_t source) noexcept {
    cpu.fpul = cpu.fpu_double_precision()
        ? truncate_to_integer_bits(read_double_operand(cpu, source))
        : truncate_to_integer_bits(read_single_operand(cpu, source));
}

void fpu_convert_double_to_single(CpuState& cpu, const std::uint8_t source) noexcept {
    const ScopedHostRounding rounding(cpu);
    const float result = static_cast<float>(read_double_operand(cpu, source));
    cpu.fpul = std::isnan(result)
        ? canonical_single_nan
        : std::bit_cast<std::uint32_t>(result);
}

void fpu_convert_single_to_double(CpuState& cpu, const std::uint8_t destination) noexcept {
    write_double_result(
        cpu,
        destination,
        static_cast<double>(flush_denormalized(cpu, std::bit_cast<float>(cpu.fpul)))
    );
}

} // namespace katana::runtime

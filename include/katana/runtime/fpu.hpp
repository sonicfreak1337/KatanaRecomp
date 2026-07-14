#pragma once

#include "katana/runtime/runtime.hpp"

#include <cstdint>

namespace katana::runtime {

enum class FpuBinaryOperation : std::uint8_t {
    Add,
    Subtract,
    Multiply,
    Divide
};

[[nodiscard]] float read_fr_single(const CpuState& cpu, std::uint8_t index) noexcept;
void write_fr_single(CpuState& cpu, std::uint8_t index, float value) noexcept;
[[nodiscard]] double read_dr_double(const CpuState& cpu, std::uint8_t even_index) noexcept;
void write_dr_double(CpuState& cpu, std::uint8_t even_index, double value) noexcept;
[[nodiscard]] std::uint64_t read_fpu_pair_bits(
    const CpuState& cpu,
    std::uint8_t encoded_index
) noexcept;
void write_fpu_pair_bits(
    CpuState& cpu,
    std::uint8_t encoded_index,
    std::uint64_t bits
) noexcept;

void fpu_binary(
    CpuState& cpu,
    FpuBinaryOperation operation,
    std::uint8_t source,
    std::uint8_t destination
) noexcept;
void fpu_absolute(CpuState& cpu, std::uint8_t destination) noexcept;
void fpu_negate(CpuState& cpu, std::uint8_t destination) noexcept;
void fpu_square_root(CpuState& cpu, std::uint8_t destination) noexcept;
void fpu_multiply_accumulate(
    CpuState& cpu,
    std::uint8_t source,
    std::uint8_t destination
) noexcept;
void fpu_compare_equal(
    CpuState& cpu,
    std::uint8_t source,
    std::uint8_t destination
) noexcept;
void fpu_compare_greater(
    CpuState& cpu,
    std::uint8_t source,
    std::uint8_t destination
) noexcept;
void fpu_float_from_fpul(CpuState& cpu, std::uint8_t destination) noexcept;
void fpu_truncate_to_fpul(CpuState& cpu, std::uint8_t source) noexcept;
void fpu_convert_double_to_single(CpuState& cpu, std::uint8_t source) noexcept;
void fpu_convert_single_to_double(CpuState& cpu, std::uint8_t destination) noexcept;

} // namespace katana::runtime

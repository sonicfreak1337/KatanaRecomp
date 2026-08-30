#pragma once

#include "katana/runtime/runtime.hpp"

#include <cfenv>
#include <cstdint>
#include <optional>

namespace katana::runtime {

enum class FpuBinaryOperation : std::uint8_t { Add, Subtract, Multiply, Divide };

// Establishes the host floating-point control state for a contiguous run of
// SH-4 FPU instructions whose FPSCR rounding contract is unchanged. Generated
// AOT code uses this to avoid saving and restoring MXCSR around every
// individual operation. Standalone runtime calls retain their per-operation
// fallback, and nested epochs restore the exact ambient host state.
class HostFpuExecutionEpoch final {
  public:
    explicit HostFpuExecutionEpoch(const CpuState& cpu) noexcept;
    ~HostFpuExecutionEpoch();

    HostFpuExecutionEpoch(const HostFpuExecutionEpoch&) = delete;
    HostFpuExecutionEpoch& operator=(const HostFpuExecutionEpoch&) = delete;
    HostFpuExecutionEpoch(HostFpuExecutionEpoch&&) = delete;
    HostFpuExecutionEpoch& operator=(HostFpuExecutionEpoch&&) = delete;

  private:
#if defined(__SSE__) || defined(_M_X64) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
    std::uint32_t previous_host_control_ = 0u;
#else
    std::fenv_t previous_host_environment_{};
#endif
    std::uint32_t previous_epoch_depth_ = 0u;
    std::uint8_t previous_epoch_rounding_ = 0u;
};

[[nodiscard]] float read_fr_single(const CpuState& cpu, std::uint8_t index) noexcept;
void write_fr_single(CpuState& cpu, std::uint8_t index, float value) noexcept;
[[nodiscard]] double read_dr_double(const CpuState& cpu, std::uint8_t even_index) noexcept;
void write_dr_double(CpuState& cpu, std::uint8_t even_index, double value) noexcept;
[[nodiscard]] std::uint64_t read_fpu_pair_bits(const CpuState& cpu,
                                               std::uint8_t encoded_index) noexcept;
void write_fpu_pair_bits(CpuState& cpu, std::uint8_t encoded_index, std::uint64_t bits) noexcept;

void fpu_binary(CpuState& cpu,
                FpuBinaryOperation operation,
                std::uint8_t source,
                std::uint8_t destination) noexcept;
void fpu_absolute(CpuState& cpu, std::uint8_t destination) noexcept;
void fpu_negate(CpuState& cpu, std::uint8_t destination) noexcept;
void fpu_square_root(CpuState& cpu, std::uint8_t destination) noexcept;
void fpu_reciprocal_square_root(CpuState& cpu, std::uint8_t destination) noexcept;
[[nodiscard]] bool fpu_reciprocal_square_root(
    CpuState& cpu,
    std::uint8_t destination,
    std::optional<std::uint32_t> delay_slot_owner) noexcept;
void fpu_sine_cosine(CpuState& cpu, std::uint8_t destination_even) noexcept;
[[nodiscard]] bool fpu_sine_cosine(
    CpuState& cpu,
    std::uint8_t destination_even,
    std::optional<std::uint32_t> delay_slot_owner) noexcept;
void fpu_inner_product(CpuState& cpu,
                       std::uint8_t source_vector,
                       std::uint8_t destination_vector) noexcept;
void fpu_transform_vector(CpuState& cpu, std::uint8_t destination_vector) noexcept;
// Executes the same four-lane FTRV accumulation order as
// fpu_transform_vector through an AVX2/FMA implementation when the current
// host supports it. Unsupported hosts return false without mutating CpuState;
// callers must then use fpu_transform_vector. Title providers remain
// responsible for identity-binding any larger matrix/vector transaction.
[[nodiscard]] bool try_fpu_transform_vector_simd(
    CpuState& cpu,
    std::uint8_t destination_vector) noexcept;
void fpu_multiply_accumulate(CpuState& cpu, std::uint8_t source, std::uint8_t destination) noexcept;
void fpu_compare_equal(CpuState& cpu, std::uint8_t source, std::uint8_t destination) noexcept;
void fpu_compare_greater(CpuState& cpu, std::uint8_t source, std::uint8_t destination) noexcept;
void fpu_float_from_fpul(CpuState& cpu, std::uint8_t destination) noexcept;
void fpu_truncate_to_fpul(CpuState& cpu, std::uint8_t source) noexcept;
void fpu_convert_double_to_single(CpuState& cpu, std::uint8_t source) noexcept;
void fpu_convert_single_to_double(CpuState& cpu, std::uint8_t destination) noexcept;

} // namespace katana::runtime

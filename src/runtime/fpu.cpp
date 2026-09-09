#include "katana/runtime/fpu.hpp"
#include "katana/runtime/exception.hpp"

#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <type_traits>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || \
    defined(__i386__)
#define KATANA_RUNTIME_HAS_X86_AVX2_FMA_DISPATCH 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#else
#define KATANA_RUNTIME_HAS_X86_AVX2_FMA_DISPATCH 0
#endif

#if defined(__SSE__) || defined(_M_X64) || \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define KATANA_RUNTIME_HAS_SSE_ROUNDING 1
#else
#define KATANA_RUNTIME_HAS_SSE_ROUNDING 0
#endif

namespace katana::runtime {

#if KATANA_RUNTIME_HAS_X86_AVX2_FMA_DISPATCH
namespace detail {
void fpu_transform_vector_avx2_fma(const float* matrix,
                                   const float* vector,
                                   float* result) noexcept;
float fpu_inner_product_avx2_fma(const float* source,
                                const float* destination) noexcept;
float fpu_multiply_accumulate_avx2_fma(float first,
                                      float second,
                                      float addend) noexcept;
} // namespace detail
#endif

namespace {

constexpr std::uint32_t canonical_single_nan = 0x7FBFFFFFu;
constexpr std::uint64_t canonical_double_nan = 0x7FF7FFFFFFFFFFFFull;

[[nodiscard]] bool double_precision(const CpuState& cpu) noexcept {
    return (cpu.fpscr & fpscr_pr_mask) != 0u;
}

[[nodiscard]] bool flush_denormals(const CpuState& cpu) noexcept {
    return (cpu.fpscr & fpscr_dn_mask) != 0u;
}

[[nodiscard]] std::uint8_t guest_rounding_mode(const CpuState& cpu) noexcept {
    return static_cast<std::uint8_t>(cpu.fpscr & fpscr_rounding_mode_mask);
}

struct HostFpuEpochState {
    std::uint32_t depth = 0u;
    std::uint8_t rounding_mode = 0u;
};

thread_local HostFpuEpochState host_fpu_epoch_state;

[[nodiscard]] bool host_avx2_fma_available() noexcept {
#if defined(_MSC_VER) && KATANA_RUNTIME_HAS_X86_AVX2_FMA_DISPATCH
    static const bool available = [] {
        int registers[4]{};
        __cpuid(registers, 0);
        if (registers[0] < 7) return false;
        __cpuid(registers, 1);
        const auto feature_ecx = static_cast<std::uint32_t>(registers[2]);
        constexpr std::uint32_t fma = 1u << 12u;
        constexpr std::uint32_t osxsave = 1u << 27u;
        constexpr std::uint32_t avx = 1u << 28u;
        if ((feature_ecx & (fma | osxsave | avx)) != (fma | osxsave | avx) ||
            (_xgetbv(0) & 0x6u) != 0x6u)
            return false;
        __cpuidex(registers, 7, 0);
        constexpr std::uint32_t avx2 = 1u << 5u;
        return (static_cast<std::uint32_t>(registers[1]) & avx2) != 0u;
    }();
    return available;
#elif KATANA_RUNTIME_HAS_X86_AVX2_FMA_DISPATCH
    static const bool available = [] {
        if (__get_cpuid_max(0, nullptr) < 7u) return false;
        unsigned eax = 0u;
        unsigned ebx = 0u;
        unsigned ecx = 0u;
        unsigned edx = 0u;
        __cpuid(1u, eax, ebx, ecx, edx);
        constexpr unsigned fma = 1u << 12u;
        constexpr unsigned osxsave = 1u << 27u;
        constexpr unsigned avx = 1u << 28u;
        if ((ecx & (fma | osxsave | avx)) != (fma | osxsave | avx))
            return false;
        std::uint32_t xcr0_low = 0u;
        std::uint32_t xcr0_high = 0u;
        __asm__ volatile("xgetbv"
                         : "=a"(xcr0_low), "=d"(xcr0_high)
                         : "c"(0u));
        if ((xcr0_low & 0x6u) != 0x6u) return false;
        __cpuid_count(7u, 0u, eax, ebx, ecx, edx);
        constexpr unsigned avx2 = 1u << 5u;
        return (ebx & avx2) != 0u;
    }();
    return available;
#else
    return false;
#endif
}

#if KATANA_RUNTIME_HAS_SSE_ROUNDING
constexpr std::uint32_t host_denormals_are_zero_mask = 0x00000040u;
constexpr std::uint32_t host_flush_to_zero_mask = 0x00008000u;
constexpr std::uint32_t host_exception_status_mask = 0x0000003Fu;
constexpr std::uint32_t host_exception_mask_mask = 0x00001F80u;

[[nodiscard]] std::uint32_t requested_host_control(const std::uint32_t current,
                                                   const std::uint8_t rounding_mode) noexcept {
    const std::uint32_t requested =
        rounding_mode == 1u ? _MM_ROUND_TOWARD_ZERO : _MM_ROUND_NEAREST;
    return (current & ~(_MM_ROUND_MASK | host_denormals_are_zero_mask |
                        host_flush_to_zero_mask |
                        host_exception_status_mask |
                        host_exception_mask_mask)) |
           requested | host_exception_mask_mask;
}
#else
[[nodiscard]] int requested_host_rounding(const std::uint8_t rounding_mode) noexcept {
    return rounding_mode == 1u ? FE_TOWARDZERO : FE_TONEAREST;
}
#endif

class ScopedHostRounding final {
  public:
    explicit ScopedHostRounding(const CpuState& cpu) noexcept {
        const auto rounding_mode = guest_rounding_mode(cpu);
        if (host_fpu_epoch_state.depth != 0u &&
            host_fpu_epoch_state.rounding_mode == rounding_mode) {
            return;
        }
#if KATANA_RUNTIME_HAS_SSE_ROUNDING
        previous_ = _mm_getcsr();
        const std::uint32_t scoped = requested_host_control(previous_, rounding_mode);
        restore_ = true;
        if (scoped != previous_) {
            _mm_setcsr(scoped);
        }
#else
        if (std::feholdexcept(&previous_) != 0) return;
        restore_ = true;
        const int requested = requested_host_rounding(rounding_mode);
        if (requested != std::fegetround()) {
            static_cast<void>(std::fesetround(requested));
        }
#endif
    }

    ~ScopedHostRounding() {
        if (!restore_) return;
#if KATANA_RUNTIME_HAS_SSE_ROUNDING
        _mm_setcsr(previous_);
#else
        static_cast<void>(std::fesetenv(&previous_));
#endif
    }

  private:
#if KATANA_RUNTIME_HAS_SSE_ROUNDING
    std::uint32_t previous_ = _MM_ROUND_NEAREST;
#else
    std::fenv_t previous_{};
#endif
    bool restore_ = false;
};

std::uint8_t even_register(const std::uint8_t index) noexcept {
    return static_cast<std::uint8_t>(index & 0x0Eu);
}

template <typename Float>
Float flush_denormalized(const CpuState& cpu, const Float value) noexcept {
    if (flush_denormals(cpu) && std::fpclassify(value) == FP_SUBNORMAL) {
        return std::copysign(static_cast<Float>(0.0), value);
    }
    return value;
}

[[nodiscard]] std::uint32_t active_fpu_instruction_pc(const CpuState& cpu) noexcept {
    return cpu.active_instruction_pc != 0u ? cpu.active_instruction_pc : cpu.pc;
}

void clear_fpu_causes(CpuState& cpu) noexcept {
    cpu.fpscr &= ~fpscr_cause_mask;
}

[[nodiscard]] bool signal_fpu_exception(CpuState& cpu,
                                         const std::uint32_t cause_mask,
                                         const std::uint32_t flag_mask,
                                         const std::uint32_t enable_mask,
                                         const bool unmaskable,
                                         const std::optional<std::uint32_t>
                                             delay_slot_owner) noexcept {
    cpu.fpscr |= cause_mask | flag_mask;
    if (!unmaskable && (cpu.fpscr & enable_mask) == 0u) return false;

    const auto instruction_pc = active_fpu_instruction_pc(cpu);
    ExceptionRequest request;
    request.cause = ExceptionCause::FpuException;
    request.event_code = event_fpu_exception;
    request.vector_offset = general_exception_vector;
    request.return_pc = delay_slot_owner.value_or(instruction_pc);
    request.in_delay_slot = delay_slot_owner.has_value();
    request.instruction_pc = instruction_pc;
    request.delay_slot_owner_pc = delay_slot_owner;
    enter_exception(cpu, request);
    return true;
}

float read_single_operand(const CpuState& cpu, const std::uint8_t index) noexcept {
    return flush_denormalized(cpu, std::bit_cast<float>(cpu.fr[index & 0x0Fu]));
}

double read_double_operand(const CpuState& cpu, const std::uint8_t index) noexcept {
    return flush_denormalized(cpu, read_dr_double(cpu, index));
}

void write_single_result(CpuState& cpu, const std::uint8_t index, const float value) noexcept {
    const float normalized = flush_denormalized(cpu, value);
    cpu.fr[index & 0x0Fu] =
        std::isnan(normalized) ? canonical_single_nan : std::bit_cast<std::uint32_t>(normalized);
}

void write_double_result(CpuState& cpu, const std::uint8_t index, const double value) noexcept {
    const std::uint8_t even = even_register(index);
    const double normalized = flush_denormalized(cpu, value);
    const std::uint64_t bits =
        std::isnan(normalized) ? canonical_double_nan : std::bit_cast<std::uint64_t>(normalized);
    cpu.fr[even] = static_cast<std::uint32_t>(bits >> 32u);
    cpu.fr[even + 1u] = static_cast<std::uint32_t>(bits);
}

template <typename Float>
Float binary_result(const FpuBinaryOperation operation,
                    const Float destination,
                    const Float source) noexcept {
    switch (operation) {
    case FpuBinaryOperation::Add:
        return destination + source;
    case FpuBinaryOperation::Subtract:
        return destination - source;
    case FpuBinaryOperation::Multiply:
        return destination * source;
    case FpuBinaryOperation::Divide:
        return destination / source;
    }
    return std::numeric_limits<Float>::quiet_NaN();
}

template <typename Float> struct ArithmeticOperand {
    using Bits = std::conditional_t<sizeof(Float) == 4u, std::uint32_t, std::uint64_t>;
    static constexpr unsigned fraction_bits = sizeof(Float) == 4u ? 23u : 52u;
    static constexpr Bits sign_mask = Bits{1} << (sizeof(Float) * 8u - 1u);
    static constexpr Bits fraction_mask = (Bits{1} << fraction_bits) - 1u;
    static constexpr Bits exponent_mask = ~(sign_mask | fraction_mask);
    Bits bits;
    ArithmeticOperand(const Float value, const bool dn) noexcept
        : bits(std::bit_cast<Bits>(value)) {
        if (dn && denormal()) bits &= sign_mask;
    }
    bool nan() const noexcept {
        return (bits & exponent_mask) == exponent_mask && (bits & fraction_mask) != 0u;
    }
    bool signaling_nan() const noexcept {
        // SH-4's signaling bit is the inverse of the host IEEE quiet bit.
        return nan() && (bits & (Bits{1} << (fraction_bits - 1u))) != 0u;
    }
    bool denormal() const noexcept {
        return (bits & exponent_mask) == 0u && (bits & fraction_mask) != 0u;
    }
    bool zero() const noexcept { return (bits & ~sign_mask) == 0u; }
    bool infinity() const noexcept { return (bits & ~sign_mask) == exponent_mask; }
    bool negative() const noexcept { return (bits & sign_mask) != 0u; }
    Float value() const noexcept { return std::bit_cast<Float>(bits); }
};

void clear_host_arithmetic_exceptions() noexcept {
#if KATANA_RUNTIME_HAS_SSE_ROUNDING
    _mm_setcsr(_mm_getcsr() & ~host_exception_status_mask);
#else
    static_cast<void>(std::feclearexcept(FE_ALL_EXCEPT));
#endif
}

[[nodiscard]] std::uint32_t host_arithmetic_causes() noexcept {
#if KATANA_RUNTIME_HAS_SSE_ROUNDING
    const auto status = _mm_getcsr();
    return ((status & _MM_EXCEPT_OVERFLOW) ? fpscr_cause_overflow_mask : 0u) |
           ((status & _MM_EXCEPT_UNDERFLOW) ? fpscr_cause_underflow_mask : 0u) |
           ((status & _MM_EXCEPT_INEXACT) ? fpscr_cause_inexact_mask : 0u);
#else
    const auto status = std::fetestexcept(FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT);
    return ((status & FE_OVERFLOW) ? fpscr_cause_overflow_mask : 0u) |
           ((status & FE_UNDERFLOW) ? fpscr_cause_underflow_mask : 0u) |
           ((status & FE_INEXACT) ? fpscr_cause_inexact_mask : 0u);
#endif
}

template <typename Float> struct ArithmeticResult {
    Float value = std::numeric_limits<Float>::quiet_NaN();
    std::uint32_t causes = 0u;
};

template <typename Float>
ArithmeticResult<Float> calculate_binary(const CpuState& cpu,
                                        const FpuBinaryOperation operation,
                                        const Float destination,
                                        const Float source) noexcept {
    const ArithmeticOperand<Float> n(destination, flush_denormals(cpu));
    const ArithmeticOperand<Float> m(source, flush_denormals(cpu));
    ArithmeticResult<Float> result;
    const bool invalid = n.signaling_nan() || m.signaling_nan() ||
        ((operation == FpuBinaryOperation::Add || operation == FpuBinaryOperation::Subtract) &&
         n.infinity() && m.infinity() &&
         ((n.negative() != m.negative()) == (operation == FpuBinaryOperation::Add))) ||
        (operation == FpuBinaryOperation::Multiply &&
         ((n.zero() && m.infinity()) || (n.infinity() && m.zero()))) ||
        (operation == FpuBinaryOperation::Divide &&
         ((n.zero() && m.zero()) || (n.infinity() && m.infinity())));
    if (invalid) {
        result.causes = fpscr_cause_invalid_mask;
    } else if (n.nan() || m.nan()) {
        // Never feed SH-4 NaNs to host arithmetic: its quiet bit is inverted.
    } else if (operation == FpuBinaryOperation::Divide && m.zero() && !n.infinity()) {
        result.causes = fpscr_cause_divide_by_zero_mask;
        result.value = std::copysign(std::numeric_limits<Float>::infinity(),
                                    n.negative() != m.negative() ? Float{-1} : Float{1});
    } else if (n.denormal() || m.denormal()) {
        result.causes = fpscr_cause_fpu_error_mask;
    } else {
        clear_host_arithmetic_exceptions();
        // Strict FP plus volatile operands prevents folding across the status
        // boundary, including inside an already active host rounding epoch.
        volatile Float left = n.value();
        volatile Float right = m.value();
        result.value = binary_result(operation, Float(left), Float(right));
        result.causes = host_arithmetic_causes();
        if (flush_denormals(cpu) && ArithmeticOperand<Float>(result.value, false).denormal())
            result.causes |= fpscr_cause_underflow_mask | fpscr_cause_inexact_mask;
        if ((result.causes & (fpscr_cause_overflow_mask | fpscr_cause_underflow_mask)) != 0u)
            result.causes |= fpscr_cause_inexact_mask;
    }
    return result;
}

[[nodiscard]] bool finish_arithmetic(CpuState& cpu, const std::uint32_t causes,
                                    const std::uint32_t conservative_enables,
                                    const std::optional<std::uint32_t> delay_owner) noexcept {
    // SH-4 CPU Core Architecture (ADCS 7182230F), instruction wrappers:
    // O/U/I enables can trap even with Cause=0. They do not fabricate flags.
    return signal_fpu_exception(cpu, causes, (causes >> 10u) & fpscr_flag_mask,
                                (causes >> 5u) | conservative_enables,
                                (causes & fpscr_cause_fpu_error_mask) != 0u,
                                delay_owner);
}

struct SingleBinaryResult {
    std::uint32_t bits = 0u;
    std::uint32_t causes = 0u;
};

[[nodiscard]] bool try_normal_single_product(const std::uint32_t n,
                                              const std::uint32_t m,
                                              const std::uint8_t rounding_mode,
                                              SingleBinaryResult& result) noexcept {
    const auto n_exponent = (n >> 23u) & 0xFFu;
    const auto m_exponent = (m >> 23u) & 0xFFu;
    if (n_exponent == 0xFFu || m_exponent == 0xFFu) return false;
    if (n_exponent == 0u || m_exponent == 0u) {
        if ((n_exponent == 0u && (n & 0x7FFFFFu) != 0u) ||
            (m_exponent == 0u && (m & 0x7FFFFFu) != 0u)) return false;
        result = {(n ^ m) & 0x80000000u, 0u};
        return true;
    }
    // Two 24-bit significands have an exact 48-bit product. Derive rounding
    // and Inexact from the discarded integer bits, avoiding an MXCSR
    // save/clear/read sequence for the common finite FMUL used by NINJA math.
    const auto product = std::uint64_t((n & 0x7FFFFFu) | 0x800000u) *
                         std::uint64_t((m & 0x7FFFFFu) | 0x800000u);
    const auto normalization = static_cast<std::uint32_t>(product >> 47u);
    auto exponent = static_cast<std::int32_t>(n_exponent + m_exponent) -
                    127 + static_cast<std::int32_t>(normalization);
    // Boundary results retain the complete existing overflow/underflow path,
    // including its tininess and FPSCR.DN rules.
    if (exponent <= 0 || exponent >= 254) return false;
    const auto shift = 23u + normalization;
    const auto remainder = product & ((std::uint64_t{1} << shift) - 1u);
    auto significand = static_cast<std::uint32_t>(product >> shift);
    if (rounding_mode != 1u) {
        const auto halfway = std::uint64_t{1} << (shift - 1u);
        significand += remainder > halfway ||
                       (remainder == halfway && (significand & 1u) != 0u);
    }
    if (significand == 0x1000000u) {
        significand >>= 1u;
        ++exponent;
    }
    result.bits = ((n ^ m) & 0x80000000u) |
                  (static_cast<std::uint32_t>(exponent) << 23u) |
                  (significand & 0x7FFFFFu);
    result.causes = remainder != 0u ? fpscr_cause_inexact_mask : 0u;
    return true;
}

[[nodiscard]] bool try_normal_single_sum(std::uint32_t n, std::uint32_t m,
                                          const bool subtract,
                                          const std::uint8_t rounding_mode,
                                          SingleBinaryResult& result) noexcept {
    auto n_magnitude = n & 0x7FFFFFFFu;
    auto m_magnitude = m & 0x7FFFFFFFu;
    if (n_magnitude >= 0x7F800000u || m_magnitude >= 0x7F800000u ||
        (n_magnitude != 0u && n_magnitude < 0x00800000u) ||
        (m_magnitude != 0u && m_magnitude < 0x00800000u))
        return false;
    if (subtract) m ^= 0x80000000u;
    if (n_magnitude == 0u || m_magnitude == 0u) {
        result = {n_magnitude != 0u ? n : m_magnitude != 0u ? m : n & m, 0u};
        return true;
    }
    if (n_magnitude < m_magnitude) {
        std::swap(n, m);
        std::swap(n_magnitude, m_magnitude);
    }
    auto exponent = static_cast<std::int32_t>(n_magnitude >> 23u);
    const auto distance = (n_magnitude >> 23u) - (m_magnitude >> 23u);
    const auto large = ((n & 0x7FFFFFu) | 0x800000u) << 3u;
    auto small = ((m & 0x7FFFFFu) | 0x800000u) << 3u;
    // Three rounding bits plus a jammed sticky bit retain every distinction
    // needed by nearest-even and toward-zero, even for a very small addend.
    if (distance >= 32u) small = 1u;
    else if (distance != 0u)
        small = (small >> distance) |
                ((small & ((std::uint32_t{1} << distance) - 1u)) != 0u);
    std::uint32_t extended = 0u;
    if (((n ^ m) & 0x80000000u) == 0u) {
        extended = large + small;
        if ((extended & 0x08000000u) != 0u) {
            extended = (extended >> 1u) | (extended & 1u);
            ++exponent;
        }
    } else {
        extended = large - small;
        if (extended == 0u) {
            result = {}; // Equal opposite finite operands produce positive zero.
            return true;
        }
        const auto normalization = std::countl_zero(extended) - 5;
        extended <<= normalization;
        exponent -= normalization;
    }
    // Keep subnormal, overflow and boundary-rounding semantics on the
    // existing complete path. No guest state has been changed here.
    if (exponent <= 0 || exponent >= 254) return false;
    const auto remainder = extended & 7u;
    auto significand = extended >> 3u;
    if (rounding_mode != 1u)
        significand += remainder > 4u ||
                       (remainder == 4u && (significand & 1u) != 0u);
    if (significand == 0x1000000u) {
        significand >>= 1u;
        ++exponent;
    }
    result.bits = (n & 0x80000000u) |
                  (static_cast<std::uint32_t>(exponent) << 23u) |
                  (significand & 0x7FFFFFu);
    result.causes = remainder != 0u ? fpscr_cause_inexact_mask : 0u;
    return true;
}

[[nodiscard]] bool try_normal_single_quotient(const std::uint32_t n,
                                               const std::uint32_t m,
                                               const std::uint8_t rounding_mode,
                                               SingleBinaryResult& result) noexcept {
    const auto n_exponent = (n >> 23u) & 0xFFu;
    const auto m_exponent = (m >> 23u) & 0xFFu;
    if (n_exponent == 0xFFu || m_exponent == 0u || m_exponent == 0xFFu)
        return false;
    if (n_exponent == 0u) {
        if ((n & 0x7FFFFFu) != 0u) return false;
        result = {(n ^ m) & 0x80000000u, 0u};
        return true;
    }
    const auto numerator = (n & 0x7FFFFFu) | 0x800000u;
    const auto denominator = (m & 0x7FFFFFu) | 0x800000u;
    const auto normalization = static_cast<unsigned>(numerator < denominator);
    auto exponent = static_cast<std::int32_t>(n_exponent) -
                    static_cast<std::int32_t>(m_exponent) + 127 -
                    static_cast<std::int32_t>(normalization);
    if (exponent <= 0 || exponent >= 254) return false;
    const auto dividend = std::uint64_t(numerator) << (26u + normalization);
    auto extended = static_cast<std::uint32_t>(dividend / denominator);
    extended |= (dividend % denominator) != 0u;
    const auto remainder = extended & 7u;
    auto significand = extended >> 3u;
    if (rounding_mode != 1u)
        significand += remainder > 4u ||
                       (remainder == 4u && (significand & 1u) != 0u);
    if (significand == 0x1000000u) {
        significand >>= 1u;
        ++exponent;
    }
    result.bits = ((n ^ m) & 0x80000000u) |
                  (static_cast<std::uint32_t>(exponent) << 23u) |
                  (significand & 0x7FFFFFu);
    result.causes = remainder != 0u ? fpscr_cause_inexact_mask : 0u;
    return true;
}

template <typename Float> std::uint32_t truncate_to_integer_bits(const Float value) noexcept {
    if (std::isnan(value) ||
        value <= static_cast<Float>(std::numeric_limits<std::int32_t>::min())) {
        return 0x80000000u;
    }
    if (value >= static_cast<Float>(std::numeric_limits<std::int32_t>::max())) {
        return 0x7FFFFFFFu;
    }
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}

struct SineCosineResult {
    float sine = 0.0f;
    float cosine = 1.0f;
};

[[nodiscard]] SineCosineResult fast_sine_cosine(const std::uint16_t angle) noexcept {
    // FSCA consumes a 16-bit phase. Reduce it around the nearest quadrant so
    // both polynomials operate only on [-pi/4, pi/4]. The degree-13/12
    // expansions stay below single-precision rounding error across every one
    // of the 65,536 architectural inputs and avoid host libm dispatch.
    constexpr double phase_to_radians =
        3.141592653589793238462643383279502884 / 32768.0;
    const auto nearest_quadrant =
        (static_cast<std::uint32_t>(angle) + 0x2000u) >> 14u;
    const auto phase_delta =
        static_cast<std::int32_t>(angle) -
        static_cast<std::int32_t>(nearest_quadrant * 0x4000u);
    const double x = static_cast<double>(phase_delta) * phase_to_radians;
    const double x2 = x * x;
    const double sine =
        x + x * x2 *
                (-1.0 / 6.0 +
                 x2 * (1.0 / 120.0 +
                       x2 * (-1.0 / 5040.0 +
                             x2 * (1.0 / 362880.0 +
                                   x2 * (-1.0 / 39916800.0 +
                                         x2 * (1.0 / 6227020800.0))))));
    const double cosine =
        1.0 +
        x2 * (-1.0 / 2.0 +
              x2 * (1.0 / 24.0 +
                    x2 * (-1.0 / 720.0 +
                          x2 * (1.0 / 40320.0 +
                                x2 * (-1.0 / 3628800.0 +
                                      x2 * (1.0 / 479001600.0))))));

    switch (nearest_quadrant & 3u) {
    case 0u:
        return {static_cast<float>(sine), static_cast<float>(cosine)};
    case 1u:
        return {static_cast<float>(cosine), static_cast<float>(-sine)};
    case 2u:
        return {static_cast<float>(-sine), static_cast<float>(-cosine)};
    default:
        return {static_cast<float>(-cosine), static_cast<float>(sine)};
    }
}

[[nodiscard]] float fast_reciprocal_square_root(
    const float value,
    const bool round_to_zero) noexcept {
    // Normalize subnormals before the seed. On the native x64 product path,
    // RSQRTSS supplies a bounded hardware seed and two binary64 Newton steps
    // brings it inside the documented SH-4 approximation envelope without a
    // scalar sqrt/divide pair. The portable seed uses three steps.
    constexpr float subnormal_scale = 16777216.0f; // 2^24
    constexpr double reciprocal_subnormal_scale_root = 4096.0; // sqrt(2^24)
    const bool subnormal = std::fpclassify(value) == FP_SUBNORMAL;
    const float normalized = subnormal ? value * subnormal_scale : value;
#if KATANA_RUNTIME_HAS_SSE_ROUNDING
    double estimate = static_cast<double>(
        _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(normalized))));
    constexpr unsigned newton_iterations = 2u;
#else
    const auto seed_bits =
        0x5F375A86u - (std::bit_cast<std::uint32_t>(normalized) >> 1u);
    double estimate = static_cast<double>(std::bit_cast<float>(seed_bits));
    constexpr unsigned newton_iterations = 3u;
#endif
    const double half = 0.5 * static_cast<double>(normalized);
    for (unsigned iteration = 0u; iteration < newton_iterations; ++iteration) {
        estimate *= 1.5 - half * estimate * estimate;
    }
    if (subnormal) {
        estimate *= reciprocal_subnormal_scale_root;
    }
    float rounded = static_cast<float>(estimate);
    const auto residual = [value](const float candidate) noexcept {
        const auto wide = static_cast<double>(candidate);
        return static_cast<double>(value) * wide * wide;
    };

    // Two Newton steps place the seed within one Binary32 neighbor, but an
    // extreme exponent can turn that final one-ULP choice into a large
    // absolute error. Select the architectural rounding neighbor by comparing
    // squared midpoint/residual values; this retains the reciprocal-square-
    // root fastpath and avoids reintroducing a scalar sqrt/divide pair.
    for (unsigned correction = 0u; correction < 3u; ++correction) {
        const auto bits = std::bit_cast<std::uint32_t>(rounded);
        const auto lower = std::bit_cast<float>(bits - 1u);
        const auto upper = std::bit_cast<float>(bits + 1u);
        if (round_to_zero) {
            if (residual(rounded) > 1.0) {
                rounded = lower;
                continue;
            }
            if (residual(upper) <= 1.0) {
                rounded = upper;
                continue;
            }
            break;
        }

        const auto upper_midpoint =
            (static_cast<double>(rounded) + static_cast<double>(upper)) * 0.5;
        const auto upper_midpoint_residual =
            static_cast<double>(value) * upper_midpoint * upper_midpoint;
        if (upper_midpoint_residual < 1.0 ||
            (upper_midpoint_residual == 1.0 && (bits & 1u) != 0u)) {
            rounded = upper;
            continue;
        }

        const auto lower_midpoint =
            (static_cast<double>(lower) + static_cast<double>(rounded)) * 0.5;
        const auto lower_midpoint_residual =
            static_cast<double>(value) * lower_midpoint * lower_midpoint;
        if (lower_midpoint_residual > 1.0 ||
            (lower_midpoint_residual == 1.0 && (bits & 1u) != 0u)) {
            rounded = lower;
            continue;
        }
        break;
    }
    return rounded;
}

} // namespace

HostFpuExecutionEpoch::HostFpuExecutionEpoch(const CpuState& cpu) noexcept
    : previous_epoch_depth_(host_fpu_epoch_state.depth),
      previous_epoch_rounding_(host_fpu_epoch_state.rounding_mode) {
    const auto rounding_mode = guest_rounding_mode(cpu);
#if KATANA_RUNTIME_HAS_SSE_ROUNDING
    previous_host_control_ = _mm_getcsr();
    const auto requested = requested_host_control(previous_host_control_, rounding_mode);
    if (requested != previous_host_control_) {
        _mm_setcsr(requested);
    }
#else
    static_cast<void>(std::feholdexcept(&previous_host_environment_));
    const auto requested = requested_host_rounding(rounding_mode);
    if (requested != std::fegetround()) {
        static_cast<void>(std::fesetround(requested));
    }
#endif
    host_fpu_epoch_state.depth = previous_epoch_depth_ + 1u;
    host_fpu_epoch_state.rounding_mode = rounding_mode;
}

HostFpuExecutionEpoch::~HostFpuExecutionEpoch() {
    host_fpu_epoch_state.depth = previous_epoch_depth_;
    host_fpu_epoch_state.rounding_mode = previous_epoch_rounding_;
#if KATANA_RUNTIME_HAS_SSE_ROUNDING
    _mm_setcsr(previous_host_control_);
#else
    static_cast<void>(std::fesetenv(&previous_host_environment_));
#endif
}

float read_fr_single(const CpuState& cpu, const std::uint8_t index) noexcept {
    return std::bit_cast<float>(cpu.fr[index & 0x0Fu]);
}

void write_fr_single(CpuState& cpu, const std::uint8_t index, const float value) noexcept {
    write_single_result(cpu, index, value);
}

double read_dr_double(const CpuState& cpu, const std::uint8_t even_index) noexcept {
    const std::uint8_t even = even_register(even_index);
    const std::uint64_t bits =
        (static_cast<std::uint64_t>(cpu.fr[even]) << 32u) | cpu.fr[even + 1u];
    return std::bit_cast<double>(bits);
}

void write_dr_double(CpuState& cpu, const std::uint8_t even_index, const double value) noexcept {
    write_double_result(cpu, even_index, value);
}

std::uint64_t read_fpu_pair_bits(const CpuState& cpu, const std::uint8_t encoded_index) noexcept {
    const auto& bank = (encoded_index & 1u) != 0u ? cpu.xf : cpu.fr;
    const std::uint8_t even = even_register(encoded_index);
    // FPSCR.SZ selects a raw 64-bit FMOV register pair, not the numeric DR
    // bit layout used by double-precision arithmetic.  On the little-endian
    // SH-4 used by Dreamcast, the lower-addressed memory longword maps to the
    // even FR/XF register and the upper longword maps to the odd register.
    return static_cast<std::uint64_t>(bank[even]) |
           (static_cast<std::uint64_t>(bank[even + 1u]) << 32u);
}

void write_fpu_pair_bits(CpuState& cpu,
                         const std::uint8_t encoded_index,
                         const std::uint64_t bits) noexcept {
    auto& bank = (encoded_index & 1u) != 0u ? cpu.xf : cpu.fr;
    const std::uint8_t even = even_register(encoded_index);
    bank[even] = static_cast<std::uint32_t>(bits);
    bank[even + 1u] = static_cast<std::uint32_t>(bits >> 32u);
}

void fpu_binary(CpuState& cpu,
                const FpuBinaryOperation operation,
                const std::uint8_t source,
                const std::uint8_t destination) noexcept {
    static_cast<void>(fpu_binary(cpu, operation, source, destination, std::nullopt));
}

bool fpu_binary(CpuState& cpu, const FpuBinaryOperation operation,
                const std::uint8_t source, const std::uint8_t destination,
                const std::optional<std::uint32_t> delay_owner) noexcept {
    constexpr auto conservative = fpscr_enable_overflow_mask |
                                  fpscr_enable_underflow_mask | fpscr_enable_inexact_mask;
    SingleBinaryResult fast_result;
    bool fast = false;
    if (!double_precision(cpu)) {
        const auto n = cpu.fr[destination & 0x0Fu];
        const auto m = cpu.fr[source & 0x0Fu];
        if (operation == FpuBinaryOperation::Multiply)
            fast = try_normal_single_product(n, m, guest_rounding_mode(cpu), fast_result);
        else if (operation == FpuBinaryOperation::Add ||
                 operation == FpuBinaryOperation::Subtract)
            fast = try_normal_single_sum(n, m, operation == FpuBinaryOperation::Subtract,
                                        guest_rounding_mode(cpu), fast_result);
        else if (operation == FpuBinaryOperation::Divide)
            fast = try_normal_single_quotient(n, m, guest_rounding_mode(cpu), fast_result);
    }
    if (fast) {
        clear_fpu_causes(cpu);
        if (finish_arithmetic(cpu, fast_result.causes, conservative, delay_owner)) return true;
        cpu.fr[destination & 0x0Fu] = fast_result.bits;
        return false;
    }
    const ScopedHostRounding rounding(cpu);
    clear_fpu_causes(cpu);
    if (double_precision(cpu)) {
        const auto result = calculate_binary(cpu, operation, read_dr_double(cpu, destination),
                                              read_dr_double(cpu, source));
        if (finish_arithmetic(cpu, result.causes, conservative, delay_owner)) return true;
        write_double_result(cpu, destination, result.value);
    } else {
        const auto result = calculate_binary(cpu, operation, read_fr_single(cpu, destination),
                                              read_fr_single(cpu, source));
        if (finish_arithmetic(cpu, result.causes, conservative, delay_owner)) return true;
        write_single_result(cpu, destination, result.value);
    }
    return false;
}

void fpu_absolute(CpuState& cpu, const std::uint8_t destination) noexcept {
    if (double_precision(cpu)) {
        cpu.fr[even_register(destination)] &= 0x7FFFFFFFu;
    } else {
        cpu.fr[destination & 0x0Fu] &= 0x7FFFFFFFu;
    }
}

void fpu_negate(CpuState& cpu, const std::uint8_t destination) noexcept {
    cpu.fr[double_precision(cpu) ? even_register(destination) : destination & 0x0Fu] ^=
        0x80000000u;
}

void fpu_square_root(CpuState& cpu, const std::uint8_t destination) noexcept {
    static_cast<void>(fpu_square_root(cpu, destination, std::nullopt));
}

bool fpu_square_root(CpuState& cpu, const std::uint8_t destination,
                     const std::optional<std::uint32_t> delay_owner) noexcept {
    const ScopedHostRounding rounding(cpu);
    clear_fpu_causes(cpu);
    const auto calculate = [&cpu]<typename Float>(const Float value) {
        const ArithmeticOperand<Float> operand(value, flush_denormals(cpu));
        ArithmeticResult<Float> result;
        if (operand.signaling_nan() ||
            (!operand.nan() && operand.negative() && !operand.zero())) {
            result.causes = fpscr_cause_invalid_mask;
        } else if (operand.nan()) {
        } else if (operand.denormal()) {
            result.causes = fpscr_cause_fpu_error_mask;
        } else {
            clear_host_arithmetic_exceptions();
            volatile Float source_value = operand.value();
            result.value = std::sqrt(Float(source_value));
            result.causes = host_arithmetic_causes() & fpscr_cause_inexact_mask;
        }
        return result;
    };
    if (double_precision(cpu)) {
        const auto result = calculate(read_dr_double(cpu, destination));
        if (finish_arithmetic(cpu, result.causes, fpscr_enable_inexact_mask, delay_owner)) return true;
        write_double_result(cpu, destination, result.value);
    } else {
        const auto result = calculate(read_fr_single(cpu, destination));
        if (finish_arithmetic(cpu, result.causes, fpscr_enable_inexact_mask, delay_owner)) return true;
        write_single_result(cpu, destination, result.value);
    }
    return false;
}

bool fpu_reciprocal_square_root(
    CpuState& cpu,
    const std::uint8_t destination,
    const std::optional<std::uint32_t> delay_slot_owner) noexcept {
    const ScopedHostRounding rounding(cpu);
    const auto index = static_cast<std::uint8_t>(destination & 0x0Fu);
    const auto bits = cpu.fr[index];
    const auto magnitude = bits & 0x7FFFFFFFu;
    const auto exponent = magnitude >> 23u;
    const auto fraction = magnitude & 0x007FFFFFu;
    const bool negative = (bits & 0x80000000u) != 0u;

    // SH-4 clears the per-instruction Cause field before classifying the
    // operand. Sticky Flag bits are intentionally left intact.
    clear_fpu_causes(cpu);

    if (exponent == 0u) {
        if (fraction == 0u || flush_denormals(cpu)) {
            if (signal_fpu_exception(cpu,
                                     fpscr_cause_divide_by_zero_mask,
                                     fpscr_flag_divide_by_zero_mask,
                                     fpscr_enable_divide_by_zero_mask,
                                     false,
                                     delay_slot_owner)) {
                return true;
            }
            write_single_result(
                cpu,
                index,
                std::copysign(std::numeric_limits<float>::infinity(),
                              negative ? -1.0f : 1.0f));
            return false;
        }

        // With DN clear, a positive denormal is the SH-4's unmaskable FPU
        // error. It has no corresponding Flag or Enable bit and leaves FRn
        // untouched (as does every enabled arithmetic exception).
        if (!negative) {
            static_cast<void>(signal_fpu_exception(cpu,
                                                   fpscr_cause_fpu_error_mask,
                                                   0u,
                                                   0u,
                                                   true,
                                                   delay_slot_owner));
            return true;
        }

        if (signal_fpu_exception(cpu,
                                 fpscr_cause_invalid_mask,
                                 fpscr_flag_invalid_mask,
                                 fpscr_enable_invalid_mask,
                                 false,
                                 delay_slot_owner)) {
            return true;
        }
        write_single_result(cpu, index, std::numeric_limits<float>::quiet_NaN());
        return false;
    }

    if (exponent == 0xFFu) {
        if (fraction == 0u) {
            if (negative) {
                if (signal_fpu_exception(cpu,
                                         fpscr_cause_invalid_mask,
                                         fpscr_flag_invalid_mask,
                                         fpscr_enable_invalid_mask,
                                         false,
                                         delay_slot_owner)) {
                    return true;
                }
                write_single_result(cpu, index, std::numeric_limits<float>::quiet_NaN());
            } else {
                write_single_result(cpu, index, 0.0f);
            }
            return false;
        }

        // SH-4 deliberately uses the inverse of the common host IEEE quiet
        // bit convention: fraction bit 22 clear denotes qNaN (canonical
        // 0x7FBFFFFF), while bit 22 set denotes sNaN. Never classify this by
        // converting through a host float first.
        const bool signaling_nan = (fraction & 0x00400000u) != 0u;
        if (signaling_nan &&
            signal_fpu_exception(cpu,
                                 fpscr_cause_invalid_mask,
                                 fpscr_flag_invalid_mask,
                                 fpscr_enable_invalid_mask,
                                 false,
                                 delay_slot_owner)) {
            return true;
        }
        write_single_result(cpu, index, std::numeric_limits<float>::quiet_NaN());
        return false;
    }

    if (negative) {
        if (signal_fpu_exception(cpu,
                                 fpscr_cause_invalid_mask,
                                 fpscr_flag_invalid_mask,
                                 fpscr_enable_invalid_mask,
                                 false,
                                 delay_slot_owner)) {
            return true;
        }
        write_single_result(cpu, index, std::numeric_limits<float>::quiet_NaN());
        return false;
    }

    // Every finite positive normal FSRRA is architecturally inexact. The
    // enabled case traps before the approximation writes FRn.
    if (signal_fpu_exception(cpu,
                             fpscr_cause_inexact_mask,
                             fpscr_flag_inexact_mask,
                             fpscr_enable_inexact_mask,
                             false,
                             delay_slot_owner)) {
        return true;
    }
    const float value = std::bit_cast<float>(bits);
    write_single_result(
        cpu,
        index,
        fast_reciprocal_square_root(value, guest_rounding_mode(cpu) == 1u));
    return false;
}

void fpu_reciprocal_square_root(CpuState& cpu,
                                const std::uint8_t destination) noexcept {
    static_cast<void>(
        fpu_reciprocal_square_root(cpu, destination, std::nullopt));
}

bool fpu_sine_cosine(
    CpuState& cpu,
    const std::uint8_t destination_even,
    const std::optional<std::uint32_t> delay_slot_owner) noexcept {
    const ScopedHostRounding rounding(cpu);
    clear_fpu_causes(cpu);
    // FSCA's table/polynomial result is always architecturally inexact,
    // including the four exact quadrant anchors. EN.I therefore has to
    // abort before either destination register is touched.
    if (signal_fpu_exception(cpu,
                             fpscr_cause_inexact_mask,
                             fpscr_flag_inexact_mask,
                             fpscr_enable_inexact_mask,
                             false,
                             delay_slot_owner)) {
        return true;
    }
    const auto angle = static_cast<std::uint16_t>(cpu.fpul);
    SineCosineResult result;
    switch (angle) {
    case 0x0000u:
        result = {0.0f, 1.0f};
        break;
    case 0x4000u:
        result = {1.0f, 0.0f};
        break;
    case 0x8000u:
        result = {0.0f, -1.0f};
        break;
    case 0xC000u:
        result = {-1.0f, 0.0f};
        break;
    default:
        result = fast_sine_cosine(angle);
        break;
    }
    write_single_result(cpu, destination_even, result.sine);
    write_single_result(cpu, static_cast<std::uint8_t>(destination_even + 1u), result.cosine);
    return false;
}

void fpu_sine_cosine(CpuState& cpu,
                     const std::uint8_t destination_even) noexcept {
    static_cast<void>(fpu_sine_cosine(cpu, destination_even, std::nullopt));
}

namespace detail {

// Retain the scalar implementation as the non-finite/unsupported-host fallback
// and an independent numerical oracle for the dispatched vector operations.
void fpu_inner_product_scalar(CpuState& cpu,
                       const std::uint8_t source_vector,
                       const std::uint8_t destination_vector) noexcept {
    const ScopedHostRounding rounding(cpu);
    clear_fpu_causes(cpu);
    float source[4];
    float destination[4];
    for (std::uint8_t i = 0; i < 4u; ++i) {
        source[i] = read_single_operand(cpu, static_cast<std::uint8_t>(source_vector + i));
        destination[i] =
            read_single_operand(cpu, static_cast<std::uint8_t>(destination_vector + i));
    }
    float result = source[0] * destination[0];
    for (std::uint8_t i = 1; i < 4u; ++i) {
        result = std::fma(source[i], destination[i], result);
    }
    write_single_result(cpu, static_cast<std::uint8_t>(destination_vector + 3u), result);
}

void fpu_transform_vector_scalar(CpuState& cpu, const std::uint8_t destination_vector) noexcept {
    const ScopedHostRounding rounding(cpu);
    clear_fpu_causes(cpu);
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

} // namespace detail

void fpu_inner_product(CpuState& cpu,
                       const std::uint8_t source_vector,
                       const std::uint8_t destination_vector) noexcept {
#if KATANA_RUNTIME_HAS_X86_AVX2_FMA_DISPATCH
    if (host_avx2_fma_available()) {
        const ScopedHostRounding rounding(cpu);
        float source[4];
        float destination[4];
        bool finite = true;
        for (std::uint8_t i = 0u; i < 4u; ++i) {
            source[i] = read_single_operand(
                cpu, static_cast<std::uint8_t>(source_vector + i));
            destination[i] = read_single_operand(
                cpu, static_cast<std::uint8_t>(destination_vector + i));
            finite = finite && std::isfinite(source[i]) &&
                     std::isfinite(destination[i]);
        }
        if (finite) {
            clear_fpu_causes(cpu);
            write_single_result(
                cpu, static_cast<std::uint8_t>(destination_vector + 3u),
                detail::fpu_inner_product_avx2_fma(source, destination));
            return;
        }
    }
#endif
    detail::fpu_inner_product_scalar(cpu, source_vector, destination_vector);
}

void fpu_transform_vector(CpuState& cpu,
                          const std::uint8_t destination_vector) noexcept {
    if (!try_fpu_transform_vector_simd(cpu, destination_vector)) {
        detail::fpu_transform_vector_scalar(cpu, destination_vector);
    }
}

bool try_fpu_transform_vector_simd(
    CpuState& cpu,
    const std::uint8_t destination_vector) noexcept {
#if KATANA_RUNTIME_HAS_X86_AVX2_FMA_DISPATCH
    if (!host_avx2_fma_available()) return false;

    const ScopedHostRounding rounding(cpu);
    float vector[4]{};
    float matrix[16]{};
    float result[4]{};
    for (std::uint8_t index = 0u; index < 4u; ++index) {
        vector[index] = read_single_operand(
            cpu, static_cast<std::uint8_t>(destination_vector + index));
    }
    for (std::uint8_t index = 0u; index < 16u; ++index) {
        matrix[index] =
            flush_denormalized(cpu, std::bit_cast<float>(cpu.xf[index]));
    }
    if (!std::all_of(std::begin(vector), std::end(vector), [](const float value) {
            return std::isfinite(value);
        }) ||
        !std::all_of(std::begin(matrix), std::end(matrix), [](const float value) {
            return std::isfinite(value);
        }))
        return false;
    clear_fpu_causes(cpu);
    detail::fpu_transform_vector_avx2_fma(matrix, vector, result);
    for (std::uint8_t row = 0u; row < 4u; ++row) {
        write_single_result(
            cpu, static_cast<std::uint8_t>(destination_vector + row), result[row]);
    }
    return true;
#else
    static_cast<void>(cpu);
    static_cast<void>(destination_vector);
    return false;
#endif
}

namespace detail {

void fpu_multiply_accumulate_scalar(CpuState& cpu,
                             const std::uint8_t source,
                             const std::uint8_t destination) noexcept {
    const ScopedHostRounding rounding(cpu);
    clear_fpu_causes(cpu);
    write_single_result(cpu,
                        destination,
                        std::fma(read_single_operand(cpu, 0u),
                                 read_single_operand(cpu, source),
                                 read_single_operand(cpu, destination)));
}

} // namespace detail

void fpu_multiply_accumulate(CpuState& cpu,
                             const std::uint8_t source,
                             const std::uint8_t destination) noexcept {
#if KATANA_RUNTIME_HAS_X86_AVX2_FMA_DISPATCH
    if (host_avx2_fma_available()) {
        const ScopedHostRounding rounding(cpu);
        const float first = read_single_operand(cpu, 0u);
        const float second = read_single_operand(cpu, source);
        const float addend = read_single_operand(cpu, destination);
        if (std::isfinite(first) && std::isfinite(second) &&
            std::isfinite(addend)) {
            clear_fpu_causes(cpu);
            write_single_result(cpu, destination,
                detail::fpu_multiply_accumulate_avx2_fma(first, second, addend));
            return;
        }
    }
#endif
    detail::fpu_multiply_accumulate_scalar(cpu, source, destination);
}

namespace {

[[nodiscard]] bool normal_or_zero_single_bits(const std::uint32_t bits) noexcept {
    const auto magnitude = bits & 0x7FFFFFFFu;
    return magnitude == 0u ||
           (magnitude >= 0x00800000u && magnitude < 0x7F800000u);
}

} // namespace

void fpu_compare_equal(CpuState& cpu,
                       const std::uint8_t source,
                       const std::uint8_t destination) noexcept {
    if (!double_precision(cpu)) {
        const auto n = cpu.fr[destination & 0x0Fu];
        const auto m = cpu.fr[source & 0x0Fu];
        if (normal_or_zero_single_bits(n) && normal_or_zero_single_bits(m)) {
            clear_fpu_causes(cpu);
            cpu.t = n == m || ((n | m) & 0x7FFFFFFFu) == 0u;
            return;
        }
    }
    const ScopedHostRounding rounding(cpu);
    clear_fpu_causes(cpu);
    cpu.t = double_precision(cpu)
                ? read_double_operand(cpu, destination) == read_double_operand(cpu, source)
                : read_single_operand(cpu, destination) == read_single_operand(cpu, source);
}

void fpu_compare_greater(CpuState& cpu,
                         const std::uint8_t source,
                         const std::uint8_t destination) noexcept {
    if (!double_precision(cpu)) {
        const auto n = cpu.fr[destination & 0x0Fu];
        const auto m = cpu.fr[source & 0x0Fu];
        if (normal_or_zero_single_bits(n) && normal_or_zero_single_bits(m)) {
            clear_fpu_causes(cpu);
            const auto n_magnitude = n & 0x7FFFFFFFu;
            const auto m_magnitude = m & 0x7FFFFFFFu;
            cpu.t = (n_magnitude | m_magnitude) != 0u &&
                    (((n ^ m) & 0x80000000u) != 0u
                         ? (n & 0x80000000u) == 0u
                         : (n & 0x80000000u) != 0u
                               ? n_magnitude < m_magnitude
                               : n_magnitude > m_magnitude);
            return;
        }
    }
    const ScopedHostRounding rounding(cpu);
    clear_fpu_causes(cpu);
    cpu.t = double_precision(cpu)
                ? read_double_operand(cpu, destination) > read_double_operand(cpu, source)
                : read_single_operand(cpu, destination) > read_single_operand(cpu, source);
}

void fpu_float_from_fpul(CpuState& cpu, const std::uint8_t destination) noexcept {
    const ScopedHostRounding rounding(cpu);
    clear_fpu_causes(cpu);
    const auto value = static_cast<std::int32_t>(cpu.fpul);
    if (double_precision(cpu)) {
        write_double_result(cpu, destination, static_cast<double>(value));
    } else {
        write_single_result(cpu, destination, static_cast<float>(value));
    }
}

void fpu_truncate_to_fpul(CpuState& cpu, const std::uint8_t source) noexcept {
    const ScopedHostRounding rounding(cpu);
    clear_fpu_causes(cpu);
    cpu.fpul = double_precision(cpu)
                   ? truncate_to_integer_bits(read_double_operand(cpu, source))
                   : truncate_to_integer_bits(read_single_operand(cpu, source));
}

void fpu_convert_double_to_single(CpuState& cpu, const std::uint8_t source) noexcept {
    const ScopedHostRounding rounding(cpu);
    clear_fpu_causes(cpu);
    const float result =
        flush_denormalized(cpu, static_cast<float>(read_double_operand(cpu, source)));
    cpu.fpul = std::isnan(result) ? canonical_single_nan : std::bit_cast<std::uint32_t>(result);
}

void fpu_convert_single_to_double(CpuState& cpu, const std::uint8_t destination) noexcept {
    const ScopedHostRounding rounding(cpu);
    clear_fpu_causes(cpu);
    write_double_result(
        cpu,
        destination,
        static_cast<double>(flush_denormalized(cpu, std::bit_cast<float>(cpu.fpul))));
}

} // namespace katana::runtime

#undef KATANA_RUNTIME_HAS_SSE_ROUNDING

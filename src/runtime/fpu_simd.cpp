#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || \
    defined(__i386__)

#include <immintrin.h>
#include <cstdint>

namespace katana::runtime::detail {

#if defined(_MSC_VER)
#define KATANA_FPU_SIMD_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define KATANA_FPU_SIMD_NOINLINE __attribute__((noinline))
#else
#define KATANA_FPU_SIMD_NOINLINE
#endif

// This translation unit is compiled for AVX2/FMA while the dispatcher remains
// in baseline fpu.cpp. Keeping the body noinline prevents IPO from moving an
// AVX instruction ahead of the CPUID/XGETBV gate on older hosts.
namespace {

bool all_finite(const __m128i bits) noexcept {
    const auto exponent = _mm_set1_epi32(0x7f800000);
    return _mm_movemask_epi8(_mm_cmpeq_epi32(
               _mm_and_si128(bits, exponent), exponent)) == 0;
}

__m128 flush_input_bits(const __m128i bits, const bool dn) noexcept {
    if (!dn) return _mm_castsi128_ps(bits);
    const auto exponent_zero = _mm_cmpeq_epi32(
        _mm_and_si128(bits, _mm_set1_epi32(0x7f800000)),
        _mm_setzero_si128());
    // Preserve the sign of zeros and flushed subnormals; never change MXCSR.
    const auto magnitude_to_clear =
        _mm_and_si128(exponent_zero, _mm_set1_epi32(0x7fffffff));
    return _mm_castsi128_ps(_mm_andnot_si128(magnitude_to_clear, bits));
}

__m128i result_bits(const __m128 value, const bool dn) noexcept {
    const auto bits = _mm_castps_si128(value);
    const auto magnitude = _mm_and_si128(bits, _mm_set1_epi32(0x7fffffff));
    const auto nan = _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7f800000));
    const auto canonical = _mm_or_si128(
        _mm_and_si128(nan, _mm_set1_epi32(0x7fbfffff)),
        _mm_andnot_si128(nan, bits));
    return _mm_castps_si128(flush_input_bits(canonical, dn));
}

} // namespace

// All inputs are loaded and checked before any result write. The dispatcher
// supplies aligned register groups, but their host addresses need not align.
KATANA_FPU_SIMD_NOINLINE bool fpu_transform_vector_bits_avx2_fma(
    const std::uint32_t* matrix,
    std::uint32_t* vector,
    const bool dn) noexcept {
    const auto vbits = _mm_loadu_si128(reinterpret_cast<const __m128i*>(vector));
    const auto b0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(matrix));
    const auto b1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(matrix + 4));
    const auto b2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(matrix + 8));
    const auto b3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(matrix + 12));
    if (!all_finite(vbits) || !all_finite(b0) || !all_finite(b1) ||
        !all_finite(b2) || !all_finite(b3)) return false;
    const auto v = flush_input_bits(vbits, dn);
    auto result = _mm_mul_ps(flush_input_bits(b0, dn),
                            _mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 0, 0, 0)));
    result = _mm_fmadd_ps(flush_input_bits(b1, dn),
                         _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1)), result);
    result = _mm_fmadd_ps(flush_input_bits(b2, dn),
                         _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 2, 2, 2)), result);
    result = _mm_fmadd_ps(flush_input_bits(b3, dn),
                         _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 3, 3, 3)), result);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(vector), result_bits(result, dn));
    return true;
}

KATANA_FPU_SIMD_NOINLINE bool fpu_inner_product_bits_avx2_fma(
    const std::uint32_t* source,
    std::uint32_t* destination,
    const bool dn) noexcept {
    const auto sbits = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source));
    const auto dbits = _mm_loadu_si128(reinterpret_cast<const __m128i*>(destination));
    if (!all_finite(sbits) || !all_finite(dbits)) return false;
    const auto s = flush_input_bits(sbits, dn);
    const auto d = flush_input_bits(dbits, dn);
    // One rounded multiplication, then three ordered scalar FMAs. No horizontal
    // reduction; source and destination may be the same register group.
    auto result = _mm_mul_ss(s, d);
    result = _mm_fmadd_ss(_mm_shuffle_ps(s, s, _MM_SHUFFLE(1, 1, 1, 1)),
                         _mm_shuffle_ps(d, d, _MM_SHUFFLE(1, 1, 1, 1)), result);
    result = _mm_fmadd_ss(_mm_shuffle_ps(s, s, _MM_SHUFFLE(2, 2, 2, 2)),
                         _mm_shuffle_ps(d, d, _MM_SHUFFLE(2, 2, 2, 2)), result);
    result = _mm_fmadd_ss(_mm_shuffle_ps(s, s, _MM_SHUFFLE(3, 3, 3, 3)),
                         _mm_shuffle_ps(d, d, _MM_SHUFFLE(3, 3, 3, 3)), result);
    destination[3] = static_cast<std::uint32_t>(
        _mm_cvtsi128_si32(result_bits(result, dn)));
    return true;
}

KATANA_FPU_SIMD_NOINLINE float fpu_multiply_accumulate_avx2_fma(
    const float first,
    const float second,
    const float addend) noexcept {
    return _mm_cvtss_f32(_mm_fmadd_ss(
        _mm_set_ss(first), _mm_set_ss(second), _mm_set_ss(addend)));
}

#undef KATANA_FPU_SIMD_NOINLINE

} // namespace katana::runtime::detail

#endif

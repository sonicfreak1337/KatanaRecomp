#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || \
    defined(__i386__)

#include <immintrin.h>

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
KATANA_FPU_SIMD_NOINLINE void fpu_transform_vector_avx2_fma(
    const float* matrix,
    const float* vector,
    float* result) noexcept {
    const auto column0 = _mm_loadu_ps(matrix + 0u);
    const auto column1 = _mm_loadu_ps(matrix + 4u);
    const auto column2 = _mm_loadu_ps(matrix + 8u);
    const auto column3 = _mm_loadu_ps(matrix + 12u);

    auto transformed = _mm_mul_ps(column0, _mm_set1_ps(vector[0u]));
    transformed =
        _mm_fmadd_ps(column1, _mm_set1_ps(vector[1u]), transformed);
    transformed =
        _mm_fmadd_ps(column2, _mm_set1_ps(vector[2u]), transformed);
    transformed =
        _mm_fmadd_ps(column3, _mm_set1_ps(vector[3u]), transformed);
    _mm_storeu_ps(result, transformed);
}

KATANA_FPU_SIMD_NOINLINE float fpu_inner_product_avx2_fma(
    const float* source,
    const float* destination) noexcept {
    // Match the scalar contract: one rounded multiply, then three ordered
    // fused additions. A horizontal dot product would reassociate the sum.
    auto result = _mm_mul_ss(_mm_load_ss(source), _mm_load_ss(destination));
    result = _mm_fmadd_ss(_mm_load_ss(source + 1u),
                         _mm_load_ss(destination + 1u), result);
    result = _mm_fmadd_ss(_mm_load_ss(source + 2u),
                         _mm_load_ss(destination + 2u), result);
    result = _mm_fmadd_ss(_mm_load_ss(source + 3u),
                         _mm_load_ss(destination + 3u), result);
    return _mm_cvtss_f32(result);
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

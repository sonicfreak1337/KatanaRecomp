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

#undef KATANA_FPU_SIMD_NOINLINE

} // namespace katana::runtime::detail

#endif
